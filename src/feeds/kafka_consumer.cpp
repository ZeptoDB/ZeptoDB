// ============================================================================
// APEX-DB: Apache Kafka Consumer — implementation
// ============================================================================

#include "apex/feeds/kafka_consumer.h"
#include "spdlog/spdlog.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>   // strtoll, strtod
#include <sstream>
#include <string>
#include <thread>    // std::this_thread::sleep_for
#include <chrono>

#ifdef APEX_KAFKA_AVAILABLE
#include <librdkafka/rdkafkacpp.h>
#endif

namespace apex::feeds {

// ============================================================================
// Internal JSON parsing helpers
// ============================================================================
namespace {

// Find "key": <value> in a JSON string.
// Writes parsed int64 into `out`.  Returns false if key not found.
static bool parse_int64_field(const char* json, const char* key, int64_t& out) {
    // Build search needle: "key"
    char needle[64];
    int n = std::snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return false;

    const char* p = std::strstr(json, needle);
    if (!p) return false;

    p += std::strlen(needle);
    // skip whitespace and ':'
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (!*p) return false;

    char* end;
    out = std::strtoll(p, &end, 10);
    return end != p;  // at least one digit consumed
}

// Parse a double field.
static bool parse_double_field(const char* json, const char* key, double& out) {
    char needle[64];
    int n = std::snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return false;

    const char* p = std::strstr(json, needle);
    if (!p) return false;

    p += std::strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (!*p) return false;

    char* end;
    out = std::strtod(p, &end);
    return end != p;
}

// Parse a quoted string field.  out receives the value between the quotes.
static bool parse_string_field(const char* json, const char* key, std::string& out) {
    char needle[64];
    int n = std::snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return false;

    const char* p = std::strstr(json, needle);
    if (!p) return false;

    p += std::strlen(needle);
    // find opening quote of value
    while (*p && *p != '"') p++;
    if (!*p) return false;
    p++;  // skip opening '"'

    const char* start = p;
    while (*p && *p != '"') p++;
    if (!*p) return false;

    out.assign(start, static_cast<size_t>(p - start));
    return true;
}

} // anonymous namespace

// ============================================================================
// Static decoders
// ============================================================================

// {"symbol_id":1,"price":15000,"volume":100,"ts":1234567890000000000}
std::optional<apex::ingestion::TickMessage>
KafkaConsumer::decode_json(const char* data, size_t len) {
    if (!data || len == 0) return std::nullopt;

    // Null-terminate for strstr (work on a local copy of max 4KB)
    constexpr size_t MAX_JSON = 4096;
    char buf[MAX_JSON];
    size_t copy_len = len < MAX_JSON - 1 ? len : MAX_JSON - 1;
    std::memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';

    int64_t symbol_id = 0, price = 0, volume = 0, ts = 0;
    if (!parse_int64_field(buf, "symbol_id", symbol_id)) return std::nullopt;
    if (!parse_int64_field(buf, "price",     price))     return std::nullopt;
    if (!parse_int64_field(buf, "volume",    volume))    return std::nullopt;
    // ts is optional — pipeline overwrites recv_ts anyway
    parse_int64_field(buf, "ts", ts);

    apex::ingestion::TickMessage msg{};
    msg.symbol_id = static_cast<SymbolId>(symbol_id);
    msg.price     = static_cast<Price>(price);
    msg.volume    = static_cast<Volume>(volume);
    msg.recv_ts   = static_cast<Timestamp>(ts);
    msg.msg_type  = 0;  // Trade
    return msg;
}

// Raw TickMessage bytes (must be exactly sizeof(TickMessage) = 64 bytes)
std::optional<apex::ingestion::TickMessage>
KafkaConsumer::decode_binary(const char* data, size_t len) {
    if (!data || len != sizeof(apex::ingestion::TickMessage)) return std::nullopt;

    apex::ingestion::TickMessage msg{};
    std::memcpy(&msg, data, sizeof(msg));
    return msg;
}

// {"symbol":"AAPL","price":150.25,"volume":100,"ts":1234567890000000000}
std::optional<apex::ingestion::TickMessage>
KafkaConsumer::decode_json_human(
    const char* data, size_t len,
    const std::unordered_map<std::string, SymbolId>& symbol_map,
    double price_scale)
{
    if (!data || len == 0) return std::nullopt;

    constexpr size_t MAX_JSON = 4096;
    char buf[MAX_JSON];
    size_t copy_len = len < MAX_JSON - 1 ? len : MAX_JSON - 1;
    std::memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';

    std::string symbol_name;
    if (!parse_string_field(buf, "symbol", symbol_name)) return std::nullopt;

    auto it = symbol_map.find(symbol_name);
    if (it == symbol_map.end()) return std::nullopt;

    double price_f = 0.0;
    int64_t volume = 0, ts = 0;
    if (!parse_double_field(buf, "price",  price_f)) return std::nullopt;
    if (!parse_int64_field(buf,  "volume", volume))  return std::nullopt;
    parse_int64_field(buf, "ts", ts);

    apex::ingestion::TickMessage msg{};
    msg.symbol_id = it->second;
    msg.price     = static_cast<Price>(price_f * price_scale);
    msg.volume    = static_cast<Volume>(volume);
    msg.recv_ts   = static_cast<Timestamp>(ts);
    msg.msg_type  = 0;
    return msg;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

KafkaConsumer::KafkaConsumer(KafkaConfig config)
    : config_(std::move(config)) {}

KafkaConsumer::~KafkaConsumer() {
    stop();
}

// ============================================================================
// Routing setup
// ============================================================================

void KafkaConsumer::set_pipeline(apex::core::ApexPipeline* pipeline) {
    pipeline_ = pipeline;
}

void KafkaConsumer::set_routing(
    apex::cluster::NodeId local_id,
    std::shared_ptr<apex::cluster::PartitionRouter> router,
    std::unordered_map<apex::cluster::NodeId,
        std::shared_ptr<apex::cluster::TcpRpcClient>> remotes)
{
    local_id_ = local_id;
    router_   = std::move(router);
    remotes_  = std::move(remotes);
}

// ============================================================================
// Dispatch (routing)
// ============================================================================

// Attempt to call ingest_fn() up to (1 + backpressure_retries) times.
// Sleeps backpressure_sleep_us between attempts.
// Returns true as soon as one attempt succeeds.
template <typename Fn>
static bool try_with_backpressure(Fn ingest_fn, int retries, int sleep_us) {
    for (int i = 0; i <= retries; ++i) {
        if (ingest_fn()) return true;
        if (i < retries)
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    }
    return false;
}

bool KafkaConsumer::ingest_decoded(apex::ingestion::TickMessage msg) {
    const int  retries   = config_.backpressure_retries;
    const int  sleep_us  = config_.backpressure_sleep_us;

    if (router_) {
        // Multi-node: route via consistent-hash ring
        apex::cluster::NodeId target = router_->route(msg.symbol_id);
        if (target == local_id_) {
            if (!pipeline_) {
                std::lock_guard<std::mutex> lk(stats_mu_);
                stats_.ingest_failures++;
                return false;
            }
            bool ok = try_with_backpressure(
                [&] { return pipeline_->ingest_tick(msg); }, retries, sleep_us);
            std::lock_guard<std::mutex> lk(stats_mu_);
            if (ok) stats_.route_local++;
            else    stats_.ingest_failures++;
            return ok;
        } else {
            auto it = remotes_.find(target);
            if (it == remotes_.end()) {
                std::lock_guard<std::mutex> lk(stats_mu_);
                stats_.ingest_failures++;
                return false;
            }
            bool ok = try_with_backpressure(
                [&] { return it->second->ingest_tick(msg); }, retries, sleep_us);
            std::lock_guard<std::mutex> lk(stats_mu_);
            if (ok) stats_.route_remote++;
            else    stats_.ingest_failures++;
            return ok;
        }
    } else if (pipeline_) {
        // Single-node: all ticks to local pipeline
        bool ok = try_with_backpressure(
            [&] { return pipeline_->ingest_tick(msg); }, retries, sleep_us);
        std::lock_guard<std::mutex> lk(stats_mu_);
        if (ok) stats_.route_local++;
        else    stats_.ingest_failures++;
        return ok;
    }
    return false;  // no pipeline or router configured
}

// ============================================================================
// on_message: decode + dispatch
// ============================================================================

bool KafkaConsumer::on_message(const char* data, size_t len) {
    std::optional<apex::ingestion::TickMessage> tick;

    switch (config_.format) {
        case MessageFormat::JSON:
            tick = decode_json(data, len);
            break;
        case MessageFormat::BINARY:
            tick = decode_binary(data, len);
            break;
        case MessageFormat::JSON_HUMAN:
            tick = decode_json_human(data, len,
                                     config_.symbol_map,
                                     config_.price_scale);
            break;
    }

    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.bytes_consumed += len;
        if (!tick) {
            stats_.decode_errors++;
            return false;
        }
        stats_.messages_consumed++;
    }

    return ingest_decoded(*tick);
}

// ============================================================================
// Lifecycle: start / stop
// ============================================================================

bool KafkaConsumer::start() {
#ifdef APEX_KAFKA_AVAILABLE
    if (running_.exchange(true)) return true;  // already running

    std::string errstr;

    auto* conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);
    conf->set("bootstrap.servers", config_.brokers,           errstr);
    conf->set("group.id",          config_.group_id,          errstr);
    conf->set("auto.offset.reset", config_.auto_offset_reset, errstr);

    if (config_.commit_mode == CommitMode::AFTER_INGEST) {
        // Disable auto-commit so we control when offsets advance.
        // Offsets are committed manually after successful ingest.
        conf->set("enable.auto.commit",       "false", errstr);
        conf->set("enable.auto.offset.store", "false", errstr);
    }

    auto* consumer = RdKafka::KafkaConsumer::create(conf, errstr);
    delete conf;

    if (!consumer) {
        spdlog::error("KafkaConsumer: failed to create consumer: {}", errstr);
        running_ = false;
        return false;
    }

    std::vector<std::string> topics{config_.topic};
    RdKafka::ErrorCode err = consumer->subscribe(topics);
    if (err != RdKafka::ERR_NO_ERROR) {
        spdlog::error("KafkaConsumer: subscribe failed: {}",
                      RdKafka::err2str(err));
        consumer->close();
        delete consumer;
        running_ = false;
        return false;
    }

    consumer_handle_ = consumer;
    spdlog::info("KafkaConsumer: subscribed to topic '{}' on '{}'",
                 config_.topic, config_.brokers);

    poll_thread_ = std::thread([this] { poll_loop(); });
    return true;

#else
    spdlog::warn("KafkaConsumer: Kafka support not compiled in "
                 "(rebuild with APEX_USE_KAFKA=ON and librdkafka++ installed)");
    return false;
#endif
}

void KafkaConsumer::stop() {
    (void)consumer_handle_;  // suppress unused-field warning when !APEX_KAFKA_AVAILABLE
    if (!running_.exchange(false)) return;  // already stopped
    if (poll_thread_.joinable()) poll_thread_.join();

#ifdef APEX_KAFKA_AVAILABLE
    if (consumer_handle_) {
        auto* consumer = static_cast<RdKafka::KafkaConsumer*>(consumer_handle_);
        // close() is called inside poll_loop() after the run-loop exits;
        // here we just delete the object.
        delete consumer;
        consumer_handle_ = nullptr;
    }
#endif
}

// ============================================================================
// Background poll loop
// ============================================================================

void KafkaConsumer::poll_loop() {
#ifdef APEX_KAFKA_AVAILABLE
    auto* consumer = static_cast<RdKafka::KafkaConsumer*>(consumer_handle_);

    while (running_.load(std::memory_order_relaxed)) {
        std::unique_ptr<RdKafka::Message> msg(
            consumer->consume(config_.poll_timeout_ms));

        if (!msg) continue;

        switch (msg->err()) {
            case RdKafka::ERR_NO_ERROR: {
                bool ingested = on_message(
                    static_cast<const char*>(msg->payload()), msg->len());
                // Commit offset only after confirmed ingest (at-least-once).
                // On failure the offset stays uncommitted; the message is
                // re-delivered after a consumer restart.
                if (ingested && config_.commit_mode == CommitMode::AFTER_INGEST)
                    consumer->commitAsync(msg.get());
                break;
            }

            case RdKafka::ERR__TIMED_OUT:
            case RdKafka::ERR__PARTITION_EOF:
                break;  // normal — no new messages

            default:
                spdlog::warn("KafkaConsumer: message error: {}",
                             msg->errstr());
                break;
        }
    }

    consumer->close();
#endif
    // Without APEX_KAFKA_AVAILABLE the poll_thread_ is never started,
    // so this function is never called.
}

// ============================================================================
// Stats
// ============================================================================

KafkaStats KafkaConsumer::stats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    return stats_;
}

// ============================================================================
// Prometheus / OpenMetrics formatter
// ============================================================================
// HELP and TYPE lines are emitted once (before the first consumer series) by
// convention.  When multiple consumers register with the same HttpServer the
// caller is responsible for picking unique consumer_name label values.
// The generated text follows the OpenMetrics text format (text/plain 0.0.4).
// ============================================================================
std::string KafkaConsumer::format_prometheus(const std::string& consumer_name,
                                              const KafkaStats& stats) {
    std::ostringstream os;
    const auto& n = consumer_name;  // shorter alias for label value

    os << "# HELP apex_kafka_messages_consumed_total Messages successfully decoded and dispatched\n";
    os << "# TYPE apex_kafka_messages_consumed_total counter\n";
    os << "apex_kafka_messages_consumed_total{consumer=\"" << n << "\"} " << stats.messages_consumed << "\n\n";

    os << "# HELP apex_kafka_bytes_consumed_total Total payload bytes received from Kafka\n";
    os << "# TYPE apex_kafka_bytes_consumed_total counter\n";
    os << "apex_kafka_bytes_consumed_total{consumer=\"" << n << "\"} " << stats.bytes_consumed << "\n\n";

    os << "# HELP apex_kafka_decode_errors_total Messages that failed to decode\n";
    os << "# TYPE apex_kafka_decode_errors_total counter\n";
    os << "apex_kafka_decode_errors_total{consumer=\"" << n << "\"} " << stats.decode_errors << "\n\n";

    os << "# HELP apex_kafka_route_local_total Ticks dispatched to the local pipeline\n";
    os << "# TYPE apex_kafka_route_local_total counter\n";
    os << "apex_kafka_route_local_total{consumer=\"" << n << "\"} " << stats.route_local << "\n\n";

    os << "# HELP apex_kafka_route_remote_total Ticks forwarded to a remote node via RPC\n";
    os << "# TYPE apex_kafka_route_remote_total counter\n";
    os << "apex_kafka_route_remote_total{consumer=\"" << n << "\"} " << stats.route_remote << "\n\n";

    os << "# HELP apex_kafka_ingest_failures_total Messages dropped after all backpressure retries\n";
    os << "# TYPE apex_kafka_ingest_failures_total counter\n";
    os << "apex_kafka_ingest_failures_total{consumer=\"" << n << "\"} " << stats.ingest_failures << "\n";

    return os.str();
}

} // namespace apex::feeds
