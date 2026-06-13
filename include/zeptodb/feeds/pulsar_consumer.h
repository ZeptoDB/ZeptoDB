#pragma once
// ============================================================================
// ZeptoDB: Apache Pulsar Consumer
// ============================================================================
// Subscribes to one Pulsar topic and routes decoded messages into the ZeptoDB
// ingestion pipeline. The decode and routing path mirrors KafkaConsumer,
// MqttConsumer, and KinesisConsumer so unit tests do not need a live broker:
//
// Single-node:   set_pipeline() -> all ticks -> local ZeptoPipeline
// Multi-node:    set_routing()  -> PartitionRouter decides local vs remote
//                                 remote ticks sent via RpcClientBase
//
// Message formats:
//   JSON         {"symbol_id":1,"price":15000,"volume":100,"ts":1234567890}
//   BINARY       raw TickMessage bytes (sizeof(TickMessage) == 64)
//   JSON_HUMAN   {"symbol":"AAPL","price":150.25,"volume":100,"ts":...}
//                requires a symbol_map in PulsarConfig
//
// Compile-time optional: define ZEPTO_PULSAR_AVAILABLE and link the Apache
// Pulsar C++ client to enable live broker polling. Without it, start() returns
// false but decode, routing, table-aware ingest, and metrics formatting remain
// fully testable.
// ============================================================================

#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/rpc_client_base.h"
#include "zeptodb/common/types.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/feeds/kafka_consumer.h"
#include "zeptodb/ingestion/tick_plant.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace zeptodb::feeds {

enum class PulsarSubscriptionType {
    Exclusive,
    Shared,
    Failover,
    KeyShared,
};

enum class PulsarInitialPosition {
    Latest,
    Earliest,
};

struct PulsarConfig {
    std::string service_url = "pulsar://localhost:6650";
    std::string topic;
    std::string subscription_name = "zepto-consumer";
    std::string consumer_name;

    PulsarSubscriptionType subscription_type = PulsarSubscriptionType::Shared;
    PulsarInitialPosition initial_position = PulsarInitialPosition::Latest;

    int receive_timeout_ms = 100;
    int max_messages_per_poll = 1024;
    int receiver_queue_size = 1000;
    int ack_grouping_time_ms = 100;
    int negative_ack_redelivery_delay_ms = 60000;

    MessageFormat format = MessageFormat::JSON;
    double price_scale = 10000.0;
    std::unordered_map<std::string, SymbolId> symbol_map;

    // Optional destination table. Empty string = legacy/default table_id 0.
    std::string table_name;

    int backpressure_retries = 3;
    int backpressure_sleep_us = 100;
};

struct PulsarStats {
    uint64_t messages_consumed = 0;
    uint64_t bytes_consumed = 0;
    uint64_t decode_errors = 0;
    uint64_t route_local = 0;
    uint64_t route_remote = 0;
    uint64_t ingest_failures = 0;
    uint64_t receive_timeouts = 0;
    uint64_t receive_errors = 0;
    uint64_t ack_errors = 0;
};

class PulsarConsumer {
public:
    explicit PulsarConsumer(PulsarConfig config);
    ~PulsarConsumer();

    PulsarConsumer(const PulsarConsumer&) = delete;
    PulsarConsumer& operator=(const PulsarConsumer&) = delete;

    void set_pipeline(zeptodb::core::ZeptoPipeline* pipeline);

    void set_routing(
        zeptodb::cluster::NodeId local_id,
        std::shared_ptr<zeptodb::cluster::PartitionRouter> router,
        std::unordered_map<zeptodb::cluster::NodeId,
            std::shared_ptr<zeptodb::cluster::RpcClientBase>> remotes);

    /// Start background receive polling for the configured topic.
    /// Returns false when config is invalid, the Enterprise IoT connector gate
    /// is unavailable, Pulsar support was not compiled in, or the broker
    /// subscribe request fails.
    bool start();

    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    PulsarStats stats() const;

    bool on_message(const char* data, size_t len);
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

    static bool is_valid_max_messages_per_poll(int max_messages) {
        return max_messages >= 1 && max_messages <= 100000;
    }

    static bool is_valid_receiver_queue_size(int queue_size) {
        return queue_size >= 1;
    }

    static const char* subscription_type_name(PulsarSubscriptionType type);

    static std::string format_prometheus(const std::string& consumer_name,
                                         const PulsarStats& stats);

private:
    void poll_loop();
    bool validate_config() const;

    PulsarConfig config_;

    zeptodb::core::ZeptoPipeline* pipeline_ = nullptr;

    zeptodb::cluster::NodeId local_id_ = 0;
    std::shared_ptr<zeptodb::cluster::PartitionRouter> router_;
    std::unordered_map<zeptodb::cluster::NodeId,
        std::shared_ptr<zeptodb::cluster::RpcClientBase>> remotes_;

    std::atomic<bool> running_{false};
    std::thread poll_thread_;

    mutable std::mutex stats_mu_;
    PulsarStats stats_;

    void* client_handle_ = nullptr;
    void* consumer_handle_ = nullptr;

    uint16_t table_id_ = 0;
};

} // namespace zeptodb::feeds
