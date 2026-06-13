// ============================================================================
// ZeptoDB: Apache Pulsar Consumer - implementation
// ============================================================================

#include "zeptodb/feeds/pulsar_consumer.h"

#include "zeptodb/auth/license_validator.h"
#include "zeptodb/common/logger.h"

#include <chrono>
#include <sstream>
#include <thread>

#ifdef ZEPTO_PULSAR_AVAILABLE
#include <pulsar/Client.h>
#endif

namespace zeptodb::feeds {
namespace {

template <typename Fn>
bool try_with_backpressure(Fn ingest_fn, int retries, int sleep_us) {
    for (int i = 0; i <= retries; ++i) {
        if (ingest_fn()) return true;
        if (i < retries) {
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        }
    }
    return false;
}

#ifdef ZEPTO_PULSAR_AVAILABLE
pulsar::ConsumerType to_pulsar_subscription_type(PulsarSubscriptionType type) {
    switch (type) {
        case PulsarSubscriptionType::Exclusive:
            return pulsar::ConsumerExclusive;
        case PulsarSubscriptionType::Failover:
            return pulsar::ConsumerFailover;
        case PulsarSubscriptionType::KeyShared:
            return pulsar::ConsumerKeyShared;
        case PulsarSubscriptionType::Shared:
        default:
            return pulsar::ConsumerShared;
    }
}

pulsar::InitialPosition to_pulsar_initial_position(PulsarInitialPosition pos) {
    switch (pos) {
        case PulsarInitialPosition::Earliest:
            return pulsar::InitialPositionEarliest;
        case PulsarInitialPosition::Latest:
        default:
            return pulsar::InitialPositionLatest;
    }
}
#endif

} // namespace

PulsarConsumer::PulsarConsumer(PulsarConfig config)
    : config_(std::move(config)) {}

PulsarConsumer::~PulsarConsumer() {
    stop();
}

void PulsarConsumer::set_pipeline(zeptodb::core::ZeptoPipeline* pipeline) {
    pipeline_ = pipeline;
    if (pipeline_ && !config_.table_name.empty()) {
        uint16_t tid = pipeline_->schema_registry().get_table_id(config_.table_name);
        if (tid == 0) {
            ZEPTO_ERROR("PulsarConsumer: unknown table '{}' - messages will be dropped",
                        config_.table_name);
        }
        table_id_ = tid;
    } else {
        table_id_ = 0;
    }
}

void PulsarConsumer::set_routing(
    zeptodb::cluster::NodeId local_id,
    std::shared_ptr<zeptodb::cluster::PartitionRouter> router,
    std::unordered_map<zeptodb::cluster::NodeId,
        std::shared_ptr<zeptodb::cluster::RpcClientBase>> remotes)
{
    local_id_ = local_id;
    router_ = std::move(router);
    remotes_ = std::move(remotes);
}

bool PulsarConsumer::ingest_decoded(zeptodb::ingestion::TickMessage msg) {
    const int retries = config_.backpressure_retries;
    const int sleep_us = config_.backpressure_sleep_us;

    if (!config_.table_name.empty() && table_id_ == 0) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.ingest_failures++;
        return false;
    }
    msg.table_id = table_id_;

    if (router_) {
        zeptodb::cluster::NodeId target = router_->route(msg.table_id, msg.symbol_id);
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
            else stats_.ingest_failures++;
            return ok;
        }

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
        else stats_.ingest_failures++;
        return ok;
    }

    if (pipeline_) {
        bool ok = try_with_backpressure(
            [&] { return pipeline_->ingest_tick(msg); }, retries, sleep_us);
        std::lock_guard<std::mutex> lk(stats_mu_);
        if (ok) stats_.route_local++;
        else stats_.ingest_failures++;
        return ok;
    }

    return false;
}

bool PulsarConsumer::on_message(const char* data, size_t len) {
    std::optional<zeptodb::ingestion::TickMessage> tick;
    switch (config_.format) {
        case MessageFormat::JSON:
            tick = decode_json(data, len);
            break;
        case MessageFormat::BINARY:
            tick = decode_binary(data, len);
            break;
        case MessageFormat::JSON_HUMAN:
            tick = decode_json_human(data, len, config_.symbol_map, config_.price_scale);
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

bool PulsarConsumer::validate_config() const {
    if (config_.service_url.empty()) {
        ZEPTO_ERROR("PulsarConsumer: service_url must not be empty");
        return false;
    }
    if (config_.topic.empty()) {
        ZEPTO_ERROR("PulsarConsumer: topic must not be empty");
        return false;
    }
    if (config_.subscription_name.empty()) {
        ZEPTO_ERROR("PulsarConsumer: subscription_name must not be empty");
        return false;
    }
    if (config_.receive_timeout_ms < 1) {
        ZEPTO_ERROR("PulsarConsumer: receive_timeout_ms must be positive");
        return false;
    }
    if (!is_valid_max_messages_per_poll(config_.max_messages_per_poll)) {
        ZEPTO_ERROR("PulsarConsumer: max_messages_per_poll must be in [1, 100000]");
        return false;
    }
    if (!is_valid_receiver_queue_size(config_.receiver_queue_size)) {
        ZEPTO_ERROR("PulsarConsumer: receiver_queue_size must be positive");
        return false;
    }
    if (config_.ack_grouping_time_ms < 0) {
        ZEPTO_ERROR("PulsarConsumer: ack_grouping_time_ms must be non-negative");
        return false;
    }
    if (config_.negative_ack_redelivery_delay_ms < 0) {
        ZEPTO_ERROR("PulsarConsumer: negative_ack_redelivery_delay_ms must be non-negative");
        return false;
    }
    return true;
}

bool PulsarConsumer::start() {
    if (!validate_config()) {
        return false;
    }
    if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::IOT_CONNECTORS)) {
        ZEPTO_WARN("Pulsar consumer requires Enterprise license (IOT_CONNECTORS) - not starting");
        return false;
    }

#ifdef ZEPTO_PULSAR_AVAILABLE
    if (running_.load(std::memory_order_relaxed)) return true;

    auto* client = new pulsar::Client(config_.service_url);
    auto* consumer = new pulsar::Consumer();

    pulsar::ConsumerConfiguration consumer_config;
    consumer_config.setConsumerType(to_pulsar_subscription_type(config_.subscription_type));
    consumer_config.setSubscriptionInitialPosition(
        to_pulsar_initial_position(config_.initial_position));
    consumer_config.setReceiverQueueSize(config_.receiver_queue_size);
    consumer_config.setAckGroupingTimeMs(config_.ack_grouping_time_ms);
    consumer_config.setNegativeAckRedeliveryDelayMs(
        config_.negative_ack_redelivery_delay_ms);
    if (!config_.consumer_name.empty()) {
        consumer_config.setConsumerName(config_.consumer_name);
    }

    pulsar::Result result = client->subscribe(
        config_.topic, config_.subscription_name, consumer_config, *consumer);
    if (result != pulsar::ResultOk) {
        ZEPTO_ERROR("PulsarConsumer: subscribe failed with result {}",
                    static_cast<int>(result));
        delete consumer;
        client->close();
        delete client;
        return false;
    }

    client_handle_ = client;
    consumer_handle_ = consumer;
    running_.store(true, std::memory_order_relaxed);
    poll_thread_ = std::thread(&PulsarConsumer::poll_loop, this);
    ZEPTO_INFO("PulsarConsumer: subscribed to topic '{}' as '{}' on '{}' ({})",
               config_.topic, config_.subscription_name, config_.service_url,
               subscription_type_name(config_.subscription_type));
    return true;
#else
    ZEPTO_WARN("PulsarConsumer: Pulsar support not compiled in "
               "(rebuild with ZEPTO_USE_PULSAR=ON and pulsar-client-cpp installed)");
    return false;
#endif
}

void PulsarConsumer::stop() {
#ifndef ZEPTO_PULSAR_AVAILABLE
    (void)client_handle_;
    (void)consumer_handle_;
#endif
    running_.store(false, std::memory_order_relaxed);

    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }

#ifdef ZEPTO_PULSAR_AVAILABLE
    if (consumer_handle_) {
        auto* consumer = static_cast<pulsar::Consumer*>(consumer_handle_);
        consumer->close();
        delete consumer;
        consumer_handle_ = nullptr;
    }
    if (client_handle_) {
        auto* client = static_cast<pulsar::Client*>(client_handle_);
        client->close();
        delete client;
        client_handle_ = nullptr;
    }
#endif
}

void PulsarConsumer::poll_loop() {
#ifdef ZEPTO_PULSAR_AVAILABLE
    auto* consumer = static_cast<pulsar::Consumer*>(consumer_handle_);
    while (running_.load(std::memory_order_relaxed)) {
        int drained = 0;
        while (running_.load(std::memory_order_relaxed) &&
               drained < config_.max_messages_per_poll) {
            pulsar::Message msg;
            pulsar::Result result = consumer->receive(msg, config_.receive_timeout_ms);
            if (result == pulsar::ResultTimeout) {
                std::lock_guard<std::mutex> lk(stats_mu_);
                stats_.receive_timeouts++;
                break;
            }
            if (result != pulsar::ResultOk) {
                {
                    std::lock_guard<std::mutex> lk(stats_mu_);
                    stats_.receive_errors++;
                }
                ZEPTO_WARN("PulsarConsumer: receive failed with result {}",
                           static_cast<int>(result));
                break;
            }

            ++drained;
            bool ok = on_message(static_cast<const char*>(msg.getData()),
                                 msg.getLength());
            if (ok) {
                pulsar::Result ack = consumer->acknowledge(msg);
                if (ack != pulsar::ResultOk) {
                    std::lock_guard<std::mutex> lk(stats_mu_);
                    stats_.ack_errors++;
                }
            } else {
                consumer->negativeAcknowledge(msg);
            }
        }
    }
#endif
}

PulsarStats PulsarConsumer::stats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    return stats_;
}

const char* PulsarConsumer::subscription_type_name(PulsarSubscriptionType type) {
    switch (type) {
        case PulsarSubscriptionType::Exclusive:
            return "exclusive";
        case PulsarSubscriptionType::Shared:
            return "shared";
        case PulsarSubscriptionType::Failover:
            return "failover";
        case PulsarSubscriptionType::KeyShared:
            return "key_shared";
    }
    return "unknown";
}

std::string PulsarConsumer::format_prometheus(
    const std::string& consumer_name,
    const PulsarStats& stats)
{
    std::ostringstream os;
    const auto& n = consumer_name;
    os << "# HELP zepto_pulsar_messages_consumed_total Messages successfully decoded and dispatched\n";
    os << "# TYPE zepto_pulsar_messages_consumed_total counter\n";
    os << "zepto_pulsar_messages_consumed_total{consumer=\"" << n << "\"} "
       << stats.messages_consumed << "\n\n";

    os << "# HELP zepto_pulsar_bytes_consumed_total Total payload bytes received from Pulsar\n";
    os << "# TYPE zepto_pulsar_bytes_consumed_total counter\n";
    os << "zepto_pulsar_bytes_consumed_total{consumer=\"" << n << "\"} "
       << stats.bytes_consumed << "\n\n";

    os << "# HELP zepto_pulsar_decode_errors_total Messages that failed to decode\n";
    os << "# TYPE zepto_pulsar_decode_errors_total counter\n";
    os << "zepto_pulsar_decode_errors_total{consumer=\"" << n << "\"} "
       << stats.decode_errors << "\n\n";

    os << "# HELP zepto_pulsar_route_local_total Messages sent to the local pipeline\n";
    os << "# TYPE zepto_pulsar_route_local_total counter\n";
    os << "zepto_pulsar_route_local_total{consumer=\"" << n << "\"} "
       << stats.route_local << "\n\n";

    os << "# HELP zepto_pulsar_route_remote_total Messages forwarded via RPC\n";
    os << "# TYPE zepto_pulsar_route_remote_total counter\n";
    os << "zepto_pulsar_route_remote_total{consumer=\"" << n << "\"} "
       << stats.route_remote << "\n\n";

    os << "# HELP zepto_pulsar_ingest_failures_total Messages dropped after all backpressure retries\n";
    os << "# TYPE zepto_pulsar_ingest_failures_total counter\n";
    os << "zepto_pulsar_ingest_failures_total{consumer=\"" << n << "\"} "
       << stats.ingest_failures << "\n\n";

    os << "# HELP zepto_pulsar_receive_timeouts_total Receive calls that timed out waiting for a message\n";
    os << "# TYPE zepto_pulsar_receive_timeouts_total counter\n";
    os << "zepto_pulsar_receive_timeouts_total{consumer=\"" << n << "\"} "
       << stats.receive_timeouts << "\n\n";

    os << "# HELP zepto_pulsar_receive_errors_total Pulsar receive failures\n";
    os << "# TYPE zepto_pulsar_receive_errors_total counter\n";
    os << "zepto_pulsar_receive_errors_total{consumer=\"" << n << "\"} "
       << stats.receive_errors << "\n\n";

    os << "# HELP zepto_pulsar_ack_errors_total Pulsar acknowledge failures after local ingest\n";
    os << "# TYPE zepto_pulsar_ack_errors_total counter\n";
    os << "zepto_pulsar_ack_errors_total{consumer=\"" << n << "\"} "
       << stats.ack_errors << "\n";
    return os.str();
}

} // namespace zeptodb::feeds
