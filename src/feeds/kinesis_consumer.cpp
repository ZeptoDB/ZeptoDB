// ============================================================================
// ZeptoDB: AWS Kinesis Consumer — implementation
// ============================================================================

#include "zeptodb/feeds/kinesis_consumer.h"

#include "zeptodb/auth/license_validator.h"
#include "zeptodb/common/logger.h"

#include <chrono>
#include <sstream>
#include <thread>

#ifdef ZEPTO_KINESIS_AVAILABLE
#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/kinesis/KinesisClient.h>
#include <aws/kinesis/model/GetRecordsRequest.h>
#include <aws/kinesis/model/GetShardIteratorRequest.h>
#include <aws/kinesis/model/ShardIteratorType.h>
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

#ifdef ZEPTO_KINESIS_AVAILABLE
Aws::Kinesis::Model::ShardIteratorType to_aws_iterator_type(
    KinesisIteratorType type)
{
    using AwsType = Aws::Kinesis::Model::ShardIteratorType;
    switch (type) {
        case KinesisIteratorType::TRIM_HORIZON: return AwsType::TRIM_HORIZON;
        case KinesisIteratorType::AT_SEQUENCE_NUMBER: return AwsType::AT_SEQUENCE_NUMBER;
        case KinesisIteratorType::AFTER_SEQUENCE_NUMBER: return AwsType::AFTER_SEQUENCE_NUMBER;
        case KinesisIteratorType::LATEST:
        default:
            return AwsType::LATEST;
    }
}
#endif

} // namespace

KinesisConsumer::KinesisConsumer(KinesisConfig config)
    : config_(std::move(config)) {}

KinesisConsumer::~KinesisConsumer() {
    stop();
}

void KinesisConsumer::set_pipeline(zeptodb::core::ZeptoPipeline* pipeline) {
    pipeline_ = pipeline;
    if (pipeline_ && !config_.table_name.empty()) {
        uint16_t tid = pipeline_->schema_registry().get_table_id(config_.table_name);
        if (tid == 0) {
            ZEPTO_ERROR("KinesisConsumer: unknown table '{}' — records will be dropped",
                        config_.table_name);
        }
        table_id_ = tid;
    } else {
        table_id_ = 0;
    }
}

void KinesisConsumer::set_routing(
    zeptodb::cluster::NodeId local_id,
    std::shared_ptr<zeptodb::cluster::PartitionRouter> router,
    std::unordered_map<zeptodb::cluster::NodeId,
        std::shared_ptr<zeptodb::cluster::RpcClientBase>> remotes)
{
    local_id_ = local_id;
    router_ = std::move(router);
    remotes_ = std::move(remotes);
}

bool KinesisConsumer::ingest_decoded(zeptodb::ingestion::TickMessage msg) {
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

bool KinesisConsumer::on_record(const char* data, size_t len) {
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
        stats_.records_consumed++;
    }

    return ingest_decoded(*tick);
}

bool KinesisConsumer::start() {
    if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::IOT_CONNECTORS)) {
        ZEPTO_WARN("Kinesis consumer requires Enterprise license (IOT_CONNECTORS) — not starting");
        return false;
    }
    if (config_.stream_name.empty()) {
        ZEPTO_ERROR("KinesisConsumer: stream_name must not be empty");
        return false;
    }
    if (config_.shard_id.empty()) {
        ZEPTO_ERROR("KinesisConsumer: shard_id must not be empty");
        return false;
    }
    if (!is_valid_max_records(config_.max_records_per_poll)) {
        ZEPTO_ERROR("KinesisConsumer: max_records_per_poll must be in [1, 10000]");
        return false;
    }
    if ((config_.iterator_type == KinesisIteratorType::AT_SEQUENCE_NUMBER ||
         config_.iterator_type == KinesisIteratorType::AFTER_SEQUENCE_NUMBER) &&
        config_.starting_sequence_number.empty()) {
        ZEPTO_ERROR("KinesisConsumer: sequence-number iterator requires starting_sequence_number");
        return false;
    }

#ifdef ZEPTO_KINESIS_AVAILABLE
    if (running_.load(std::memory_order_relaxed)) return true;

    auto* options = new Aws::SDKOptions();
    Aws::InitAPI(*options);

    Aws::Client::ClientConfiguration client_config;
    client_config.region = config_.region.c_str();
    if (!config_.endpoint_override.empty()) {
        client_config.endpointOverride = config_.endpoint_override.c_str();
    }

    auto* client = new Aws::Kinesis::KinesisClient(client_config);

    Aws::Kinesis::Model::GetShardIteratorRequest req;
    req.SetStreamName(config_.stream_name.c_str());
    req.SetShardId(config_.shard_id.c_str());
    req.SetShardIteratorType(to_aws_iterator_type(config_.iterator_type));
    if (!config_.starting_sequence_number.empty()) {
        req.SetStartingSequenceNumber(config_.starting_sequence_number.c_str());
    }

    auto outcome = client->GetShardIterator(req);
    if (!outcome.IsSuccess()) {
        ZEPTO_ERROR("KinesisConsumer: GetShardIterator failed: {}",
                    outcome.GetError().GetMessage());
        delete client;
        Aws::ShutdownAPI(*options);
        delete options;
        return false;
    }

    next_shard_iterator_ = outcome.GetResult().GetShardIterator();
    if (next_shard_iterator_.empty()) {
        ZEPTO_ERROR("KinesisConsumer: GetShardIterator returned an empty iterator");
        delete client;
        Aws::ShutdownAPI(*options);
        delete options;
        return false;
    }

    sdk_options_ = options;
    client_handle_ = client;
    running_.store(true, std::memory_order_relaxed);
    poll_thread_ = std::thread(&KinesisConsumer::poll_loop, this);
    ZEPTO_INFO("KinesisConsumer: polling stream '{}' shard '{}' in region '{}'",
               config_.stream_name, config_.shard_id, config_.region);
    return true;
#else
    ZEPTO_WARN("KinesisConsumer: AWS Kinesis support not compiled in "
               "(rebuild with ZEPTO_USE_KINESIS=ON and aws-sdk-cpp-kinesis installed)");
    return false;
#endif
}

void KinesisConsumer::stop() {
#ifndef ZEPTO_KINESIS_AVAILABLE
    (void)sdk_options_;
    (void)client_handle_;
#endif
    running_.store(false, std::memory_order_relaxed);

    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }

#ifdef ZEPTO_KINESIS_AVAILABLE
    if (client_handle_) {
        auto* client = static_cast<Aws::Kinesis::KinesisClient*>(client_handle_);
        delete client;
        client_handle_ = nullptr;
    }
    if (sdk_options_) {
        auto* options = static_cast<Aws::SDKOptions*>(sdk_options_);
        Aws::ShutdownAPI(*options);
        delete options;
        sdk_options_ = nullptr;
    }
#endif
}

void KinesisConsumer::poll_loop() {
#ifdef ZEPTO_KINESIS_AVAILABLE
    auto* client = static_cast<Aws::Kinesis::KinesisClient*>(client_handle_);
    while (running_.load(std::memory_order_relaxed)) {
        Aws::Kinesis::Model::GetRecordsRequest req;
        req.SetShardIterator(next_shard_iterator_.c_str());
        req.SetLimit(config_.max_records_per_poll);

        auto outcome = client->GetRecords(req);
        if (!outcome.IsSuccess()) {
            {
                std::lock_guard<std::mutex> lk(stats_mu_);
                stats_.get_records_errors++;
            }
            ZEPTO_WARN("KinesisConsumer: GetRecords failed: {}",
                       outcome.GetError().GetMessage());
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.poll_interval_ms));
            continue;
        }

        const auto& result = outcome.GetResult();
        for (const auto& record : result.GetRecords()) {
            const auto& data = record.GetData();
            on_record(reinterpret_cast<const char*>(data.GetUnderlyingData()),
                      data.GetLength());
        }

        next_shard_iterator_ = result.GetNextShardIterator();
        if (next_shard_iterator_.empty()) {
            ZEPTO_WARN("KinesisConsumer: shard iterator expired or closed");
            running_.store(false, std::memory_order_relaxed);
            break;
        }

        if (result.GetRecords().empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.poll_interval_ms));
        }
    }
#endif
}

KinesisStats KinesisConsumer::stats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    return stats_;
}

std::string KinesisConsumer::format_prometheus(
    const std::string& consumer_name,
    const KinesisStats& stats)
{
    std::ostringstream os;
    const auto& n = consumer_name;
    os << "# HELP zepto_kinesis_records_consumed_total Records successfully decoded and dispatched\n";
    os << "# TYPE zepto_kinesis_records_consumed_total counter\n";
    os << "zepto_kinesis_records_consumed_total{consumer=\"" << n << "\"} "
       << stats.records_consumed << "\n\n";

    os << "# HELP zepto_kinesis_bytes_consumed_total Total payload bytes received from Kinesis\n";
    os << "# TYPE zepto_kinesis_bytes_consumed_total counter\n";
    os << "zepto_kinesis_bytes_consumed_total{consumer=\"" << n << "\"} "
       << stats.bytes_consumed << "\n\n";

    os << "# HELP zepto_kinesis_decode_errors_total Records that failed to decode\n";
    os << "# TYPE zepto_kinesis_decode_errors_total counter\n";
    os << "zepto_kinesis_decode_errors_total{consumer=\"" << n << "\"} "
       << stats.decode_errors << "\n\n";

    os << "# HELP zepto_kinesis_route_local_total Records sent to the local pipeline\n";
    os << "# TYPE zepto_kinesis_route_local_total counter\n";
    os << "zepto_kinesis_route_local_total{consumer=\"" << n << "\"} "
       << stats.route_local << "\n\n";

    os << "# HELP zepto_kinesis_route_remote_total Records forwarded via RPC\n";
    os << "# TYPE zepto_kinesis_route_remote_total counter\n";
    os << "zepto_kinesis_route_remote_total{consumer=\"" << n << "\"} "
       << stats.route_remote << "\n\n";

    os << "# HELP zepto_kinesis_ingest_failures_total Records dropped after all backpressure retries\n";
    os << "# TYPE zepto_kinesis_ingest_failures_total counter\n";
    os << "zepto_kinesis_ingest_failures_total{consumer=\"" << n << "\"} "
       << stats.ingest_failures << "\n\n";

    os << "# HELP zepto_kinesis_get_records_errors_total AWS GetRecords failures\n";
    os << "# TYPE zepto_kinesis_get_records_errors_total counter\n";
    os << "zepto_kinesis_get_records_errors_total{consumer=\"" << n << "\"} "
       << stats.get_records_errors << "\n";
    return os.str();
}

} // namespace zeptodb::feeds
