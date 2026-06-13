#pragma once
// ============================================================================
// ZeptoDB: AWS Kinesis Consumer
// ============================================================================
// Polls one Kinesis shard and routes decoded records into the ZeptoDB ingestion
// pipeline. The decode and routing path mirrors KafkaConsumer so unit tests do
// not need live AWS credentials:
//
// Single-node:   set_pipeline()  -> all ticks -> local ZeptoPipeline
// Multi-node:    set_routing()   -> PartitionRouter decides local vs remote
//                                  remote ticks sent via RpcClientBase
//
// Message formats:
//   JSON         {"symbol_id":1,"price":15000,"volume":100,"ts":1234567890}
//   BINARY       raw TickMessage bytes (sizeof(TickMessage) == 64)
//   JSON_HUMAN   {"symbol":"AAPL","price":150.25,"volume":100,"ts":...}
//                requires a symbol_map in KinesisConfig
//
// Compile-time optional: define ZEPTO_KINESIS_AVAILABLE and link AWS SDK C++
// kinesis to enable live polling. Without it, start() returns false but decode,
// routing, table-aware ingest, and metrics formatting remain fully testable.
// ============================================================================

#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/rpc_client_base.h"
#include "zeptodb/common/types.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/feeds/kafka_consumer.h"  // MessageFormat + shared decoders
#include "zeptodb/ingestion/tick_plant.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace zeptodb::feeds {

enum class KinesisIteratorType {
    LATEST,
    TRIM_HORIZON,
    AT_SEQUENCE_NUMBER,
    AFTER_SEQUENCE_NUMBER,
};

struct KinesisConfig {
    std::string region = "us-east-1";
    std::string stream_name;
    std::string shard_id = "shardId-000000000000";
    std::string endpoint_override;

    KinesisIteratorType iterator_type = KinesisIteratorType::LATEST;
    std::string starting_sequence_number;

    int poll_interval_ms = 200;
    int max_records_per_poll = 1000;

    MessageFormat format = MessageFormat::JSON;
    double price_scale = 10000.0;
    std::unordered_map<std::string, SymbolId> symbol_map;

    // Optional destination table. Empty string = legacy/default table_id 0.
    std::string table_name;

    int backpressure_retries = 3;
    int backpressure_sleep_us = 100;
};

struct KinesisStats {
    uint64_t records_consumed = 0;
    uint64_t bytes_consumed = 0;
    uint64_t decode_errors = 0;
    uint64_t route_local = 0;
    uint64_t route_remote = 0;
    uint64_t ingest_failures = 0;
    uint64_t get_records_errors = 0;
};

class KinesisConsumer {
public:
    explicit KinesisConsumer(KinesisConfig config);
    ~KinesisConsumer();

    KinesisConsumer(const KinesisConsumer&) = delete;
    KinesisConsumer& operator=(const KinesisConsumer&) = delete;

    void set_pipeline(zeptodb::core::ZeptoPipeline* pipeline);

    void set_routing(
        zeptodb::cluster::NodeId local_id,
        std::shared_ptr<zeptodb::cluster::PartitionRouter> router,
        std::unordered_map<zeptodb::cluster::NodeId,
            std::shared_ptr<zeptodb::cluster::RpcClientBase>> remotes);

    /// Start background polling for one configured stream shard.
    /// Returns false if the Enterprise IoT connector gate is unavailable,
    /// config is invalid, AWS SDK support was not compiled in, or AWS rejects
    /// the shard iterator request.
    bool start();

    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    KinesisStats stats() const;

    bool on_record(const char* data, size_t len);
    bool ingest_decoded(zeptodb::ingestion::TickMessage msg);

    static std::optional<zeptodb::ingestion::TickMessage>
    decode_json(const char* data, size_t len) {
        return KafkaConsumer::decode_json(data, len);
    }

    static std::optional<zeptodb::ingestion::TickMessage>
    decode_binary(const char* data, size_t len) {
        return KafkaConsumer::decode_binary(data, len);
    }

    static std::optional<zeptodb::ingestion::TickMessage>
    decode_json_human(const char* data, size_t len,
                      const std::unordered_map<std::string, SymbolId>& symbol_map,
                      double price_scale) {
        return KafkaConsumer::decode_json_human(data, len, symbol_map, price_scale);
    }

    static bool is_valid_max_records(int max_records) {
        return max_records >= 1 && max_records <= 10000;
    }

    static std::string format_prometheus(const std::string& consumer_name,
                                         const KinesisStats& stats);

private:
    void poll_loop();

    KinesisConfig config_;

    zeptodb::core::ZeptoPipeline* pipeline_ = nullptr;

    zeptodb::cluster::NodeId local_id_ = 0;
    std::shared_ptr<zeptodb::cluster::PartitionRouter> router_;
    std::unordered_map<zeptodb::cluster::NodeId,
        std::shared_ptr<zeptodb::cluster::RpcClientBase>> remotes_;

    std::atomic<bool> running_{false};
    std::thread poll_thread_;

    mutable std::mutex stats_mu_;
    KinesisStats stats_;

    void* sdk_options_ = nullptr;
    void* client_handle_ = nullptr;
    std::string next_shard_iterator_;

    uint16_t table_id_ = 0;
};

} // namespace zeptodb::feeds
