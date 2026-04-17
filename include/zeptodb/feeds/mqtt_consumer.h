#pragma once
// ============================================================================
// ZeptoDB: MQTT Consumer (IoT / Physical AI)
// ============================================================================
// Subscribes to one or more MQTT topics (single topic or wildcard such as
// `sensors/#` or `plant/+/temp`) and routes ticks into the ZeptoDB ingestion
// pipeline.  This is a near-drop-in twin of KafkaConsumer — the decode and
// routing paths are shared in spirit: JSON / BINARY / JSON_HUMAN are reused
// via `MessageFormat` from kafka_consumer.h to avoid type duplication.
//
// Single-node:   set_pipeline()  → all ticks → local ZeptoPipeline
// Multi-node:    set_routing()   → PartitionRouter decides local vs remote
//                                  remote ticks sent via TcpRpcClient::ingest_tick()
//
// Message formats (shared with Kafka):
//   JSON         {"symbol_id":1,"price":15000,"volume":100,"ts":1234567890}
//   BINARY       raw TickMessage bytes (sizeof(TickMessage) == 64)
//   JSON_HUMAN   {"symbol":"AAPL","price":150.25,"volume":100,"ts":...}
//                requires a symbol_map in MqttConfig
//
// QoS levels (MQTT 3.1.1 / 5.0):
//   0 — at-most-once  (fire-and-forget, lowest overhead)
//   1 — at-least-once (broker PUBACK, default — recommended for sensor data)
//   2 — exactly-once  (4-way handshake, highest overhead)
//
// Compile-time optional: define ZEPTO_MQTT_AVAILABLE and link
// -lpaho-mqttpp3 -lpaho-mqtt3a to enable actual MQTT broker connectivity.
// Without it, start() returns false but all decode / routing functions
// remain fully functional for testing.
//
// Deliberate deltas from KafkaConsumer:
//   1. No poll thread — Paho owns its own callback thread and invokes our
//      message handler directly; we do not run a consume()/poll loop.
//   2. No CommitMode — MQTT has no consumer offsets; delivery guarantees
//      are expressed via QoS (0/1/2) at the broker protocol level.
// ============================================================================

#include "zeptodb/common/types.h"
#include "zeptodb/feeds/kafka_consumer.h"      // reuse MessageFormat
#include "zeptodb/ingestion/tick_plant.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/tcp_rpc.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace zeptodb::feeds {

// ============================================================================
// MqttConfig: consumer configuration
// ============================================================================
struct MqttConfig {
    // tcp://host:1883  |  ssl://host:8883  |  ws://host:9001
    std::string broker_uri = "tcp://localhost:1883";

    // Topic filter — single topic ("sensors/temp") or wildcard
    // ("sensors/#" multi-level, "plant/+/temp" single-level).
    std::string topic;  // REQUIRED — must not be empty; start() returns false otherwise

    std::string client_id  = "zepto-mqtt-consumer";
    std::string username;             // optional
    std::string password;             // optional

    // 0 = at-most-once, 1 = at-least-once, 2 = exactly-once
    int qos = 1;

    // MQTT keepalive interval (seconds) and clean session flag.
    int  keepalive_sec  = 30;
    bool clean_session  = true;

    // Reuses KafkaConsumer's MessageFormat / price_scale / symbol_map to
    // avoid type duplication across Kafka and MQTT decode paths.
    MessageFormat format      = MessageFormat::JSON;
    double        price_scale = 10000.0;
    std::unordered_map<std::string, SymbolId> symbol_map;

    // ------------------------------------------------------------------
    // Backpressure: retry when the ingestion ring buffer is full
    // ------------------------------------------------------------------
    int backpressure_retries   = 3;
    int backpressure_sleep_us  = 100;
};

// ============================================================================
// MqttStats: per-consumer statistics (snapshot)
// ============================================================================
struct MqttStats {
    uint64_t messages_consumed = 0;
    uint64_t bytes_consumed    = 0;
    uint64_t decode_errors     = 0;
    uint64_t route_local       = 0;
    uint64_t route_remote      = 0;
    uint64_t ingest_failures   = 0;
};

// ============================================================================
// MqttConsumer
// ============================================================================
class MqttConsumer {
public:
    explicit MqttConsumer(MqttConfig config);
    ~MqttConsumer();

    MqttConsumer(const MqttConsumer&)            = delete;
    MqttConsumer& operator=(const MqttConsumer&) = delete;

    // ------------------------------------------------------------------
    // Routing setup — call before start()
    // ------------------------------------------------------------------

    /// Single-node mode: all decoded ticks go to this pipeline.
    void set_pipeline(zeptodb::core::ZeptoPipeline* pipeline);

    /// Multi-node mode: route via consistent-hash ring.
    void set_routing(
        zeptodb::cluster::NodeId local_id,
        std::shared_ptr<zeptodb::cluster::PartitionRouter> router,
        std::unordered_map<zeptodb::cluster::NodeId,
            std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes);

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    /// Connect to the broker and subscribe.  Returns false if:
    ///  - QoS is out of range (must be 0, 1, or 2)
    ///  - topic is empty
    ///  - ZEPTO_MQTT_AVAILABLE is not defined
    ///  - broker connection / subscription fails
    bool start();

    /// Unsubscribe, disconnect and join the callback thread.
    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // ------------------------------------------------------------------
    // Statistics
    // ------------------------------------------------------------------
    MqttStats stats() const;

    // ------------------------------------------------------------------
    // Public message handlers — testable without a live broker
    // ------------------------------------------------------------------

    /// Decode + route a raw payload.  Called by the broker callback; also
    /// callable directly from tests or custom integrations.
    bool on_message(const char* data, size_t len);

    /// Route a pre-decoded TickMessage (skips decoding).
    bool ingest_decoded(zeptodb::ingestion::TickMessage msg);

    // ------------------------------------------------------------------
    // Static decoders — thin wrappers over KafkaConsumer's pure decoders
    // (single source of truth for JSON / BINARY / JSON_HUMAN parsing).
    // ------------------------------------------------------------------

    /// Decode JSON format.  See KafkaConsumer::decode_json.
    static std::optional<zeptodb::ingestion::TickMessage>
    decode_json(const char* data, size_t len) {
        return KafkaConsumer::decode_json(data, len);
    }

    /// Decode raw binary TickMessage.  See KafkaConsumer::decode_binary.
    static std::optional<zeptodb::ingestion::TickMessage>
    decode_binary(const char* data, size_t len) {
        return KafkaConsumer::decode_binary(data, len);
    }

    /// Decode human-readable JSON.  See KafkaConsumer::decode_json_human.
    static std::optional<zeptodb::ingestion::TickMessage>
    decode_json_human(const char* data, size_t len,
                      const std::unordered_map<std::string, SymbolId>& symbol_map,
                      double price_scale) {
        return KafkaConsumer::decode_json_human(data, len, symbol_map, price_scale);
    }

    /// QoS must be 0, 1, or 2 (MQTT 3.1.1 / 5.0).
    static bool is_valid_qos(int qos) { return qos >= 0 && qos <= 2; }

private:
    MqttConfig config_;

    // Single-node routing target
    zeptodb::core::ZeptoPipeline* pipeline_ = nullptr;

    // Multi-node routing
    zeptodb::cluster::NodeId local_id_ = 0;
    std::shared_ptr<zeptodb::cluster::PartitionRouter> router_;
    std::unordered_map<zeptodb::cluster::NodeId,
        std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes_;

    std::atomic<bool> running_{false};

    // Stats
    mutable std::mutex stats_mu_;
    MqttStats          stats_;

    // Opaque Paho handle (mqtt::async_client* when ZEPTO_MQTT_AVAILABLE)
    void* client_handle_ = nullptr;
};

} // namespace zeptodb::feeds
