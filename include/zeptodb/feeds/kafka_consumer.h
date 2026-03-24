#pragma once
// ============================================================================
// ZeptoDB: Apache Kafka Consumer
// ============================================================================
// Consumes market data from a Kafka topic and routes ticks into the
// ZeptoDB ingestion pipeline.
//
// Single-node:   set_pipeline()  → all ticks → local ZeptoPipeline
// Multi-node:    set_routing()   → PartitionRouter decides local vs remote
//                                  remote ticks sent via TcpRpcClient::ingest_tick()
//
// Message formats:
//   JSON         {"symbol_id":1,"price":15000,"volume":100,"ts":1234567890}
//   BINARY       raw TickMessage bytes (sizeof(TickMessage) == 64)
//   JSON_HUMAN   {"symbol":"AAPL","price":150.25,"volume":100,"ts":...}
//                requires a symbol_map in KafkaConfig
//
// Compile-time optional: define APEX_KAFKA_AVAILABLE and link -lrdkafka++
// to enable actual Kafka polling.  Without it, start() returns false but
// all decode / routing functions remain fully functional for testing.
// ============================================================================

#include "zeptodb/common/types.h"
#include "zeptodb/ingestion/tick_plant.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/tcp_rpc.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace zeptodb::feeds {

// ============================================================================
// MessageFormat: wire format of messages in the Kafka topic
// ============================================================================
enum class MessageFormat {
    JSON,        // {"symbol_id":1,"price":15000,"volume":100,"ts":...}
    BINARY,      // raw TickMessage bytes (64 bytes, little-endian)
    JSON_HUMAN,  // {"symbol":"AAPL","price":150.25,"volume":100,"ts":...}
};

// ============================================================================
// CommitMode: Kafka offset commit strategy
// ============================================================================
enum class CommitMode {
    // librdkafka auto-commits offsets on a timer (default every 5s).
    // Fast but if the process crashes after commit and before ingest completes,
    // those messages are permanently lost.
    AUTO,

    // Offset is committed only after ingest_decoded() returns true.
    // Guarantees at-least-once delivery: on restart, un-ingested messages
    // are re-delivered from the last committed offset.
    // Requires enable.auto.commit=false (set automatically by start()).
    AFTER_INGEST,
};

// ============================================================================
// KafkaConfig: consumer configuration
// ============================================================================
struct KafkaConfig {
    std::string brokers          = "localhost:9092";
    std::string topic;
    std::string group_id         = "zepto-consumer";
    std::string auto_offset_reset = "latest";  // "latest" | "earliest"

    int  poll_timeout_ms = 100;   // ms to block in each poll() call
    int  batch_size      = 1024;  // max messages to drain per wake-up

    MessageFormat format      = MessageFormat::JSON;
    double        price_scale = 10000.0;  // float → int64 (JSON_HUMAN only)

    // Symbol name → SymbolId mapping (required for JSON_HUMAN format)
    std::unordered_map<std::string, SymbolId> symbol_map;

    // ------------------------------------------------------------------
    // Offset commit strategy
    // ------------------------------------------------------------------
    // AFTER_INGEST is the safe default: offsets are committed only after
    // a tick is successfully ingested. AUTO may lose messages on crash.
    CommitMode commit_mode = CommitMode::AFTER_INGEST;

    // ------------------------------------------------------------------
    // Backpressure: retry when the ingestion ring buffer is full
    // ------------------------------------------------------------------
    // If ingest_tick() returns false (ring buffer full), retry up to
    // backpressure_retries times, sleeping backpressure_sleep_us
    // microseconds between attempts.  After all retries fail the message
    // is dropped (ingest_failures++) and the offset is NOT committed,
    // so the message will be re-delivered on the next consumer restart.
    int backpressure_retries   = 3;    // 0 = no retry, fail immediately
    int backpressure_sleep_us  = 100;  // sleep between retries (μs)
};

// ============================================================================
// KafkaStats: per-consumer statistics (snapshot, not live atomics)
// ============================================================================
struct KafkaStats {
    uint64_t messages_consumed = 0;  // successfully decoded + dispatched
    uint64_t bytes_consumed    = 0;  // total payload bytes received
    uint64_t decode_errors     = 0;  // messages that failed to decode
    uint64_t route_local       = 0;  // ticks dispatched to local pipeline
    uint64_t route_remote      = 0;  // ticks forwarded to remote node via RPC
    uint64_t ingest_failures   = 0;  // pipeline/RPC ingest returned false
};

// ============================================================================
// KafkaConsumer
// ============================================================================
class KafkaConsumer {
public:
    explicit KafkaConsumer(KafkaConfig config);
    ~KafkaConsumer();

    KafkaConsumer(const KafkaConsumer&)            = delete;
    KafkaConsumer& operator=(const KafkaConsumer&) = delete;

    // ------------------------------------------------------------------
    // Routing setup — call before start()
    // ------------------------------------------------------------------

    /// Single-node mode: all decoded ticks go to this pipeline.
    void set_pipeline(zeptodb::core::ZeptoPipeline* pipeline);

    /// Multi-node mode: route via consistent-hash ring.
    /// @param local_id   NodeId of this process.
    /// @param router     Shared PartitionRouter (thread-safe).
    /// @param remotes    Map of NodeId → TcpRpcClient for remote nodes.
    void set_routing(
        zeptodb::cluster::NodeId local_id,
        std::shared_ptr<zeptodb::cluster::PartitionRouter> router,
        std::unordered_map<zeptodb::cluster::NodeId,
            std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes);

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    /// Start background polling thread.
    /// Returns false if Kafka support is not compiled in (APEX_KAFKA_AVAILABLE)
    /// or if the broker / topic subscription fails.
    bool start();

    /// Stop polling and join the background thread.
    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // ------------------------------------------------------------------
    // Statistics
    // ------------------------------------------------------------------
    KafkaStats stats() const;

    // ------------------------------------------------------------------
    // Public message handlers — testable without a live Kafka broker
    // ------------------------------------------------------------------

    /// Decode + route a raw payload.  Called by the poll loop; also
    /// callable directly from tests or custom integrations.
    /// @return true if the tick was successfully decoded and ingested.
    bool on_message(const char* data, size_t len);

    /// Route a pre-decoded TickMessage (skips decoding).
    /// Useful when the caller performs its own deserialization.
    bool ingest_decoded(zeptodb::ingestion::TickMessage msg);

    // ------------------------------------------------------------------
    // Static decoders — pure functions, zero side-effects, fully testable
    // ------------------------------------------------------------------

    /// Decode JSON format: {"symbol_id":1,"price":15000,"volume":100,"ts":...}
    /// "ts" is optional; if absent recv_ts is left at 0 (pipeline fills it).
    static std::optional<zeptodb::ingestion::TickMessage>
    decode_json(const char* data, size_t len);

    /// Decode raw binary TickMessage (must be exactly sizeof(TickMessage)).
    static std::optional<zeptodb::ingestion::TickMessage>
    decode_binary(const char* data, size_t len);

    /// Decode human-readable JSON: {"symbol":"AAPL","price":150.25,"volume":100}
    /// @param symbol_map  Name → SymbolId lookup table.
    /// @param price_scale Multiply float price by this to get int64 fixed-point.
    static std::optional<zeptodb::ingestion::TickMessage>
    decode_json_human(const char* data, size_t len,
                      const std::unordered_map<std::string, SymbolId>& symbol_map,
                      double price_scale);

    /// Format KafkaStats as Prometheus/OpenMetrics text.
    /// @param consumer_name  Label value for the `consumer` label (e.g. topic name).
    /// @param stats          Snapshot returned by KafkaConsumer::stats().
    /// Returns a multi-line string ready to append to a /metrics response.
    static std::string format_prometheus(const std::string& consumer_name,
                                         const KafkaStats& stats);

private:
    void poll_loop();

    KafkaConfig config_;

    // Single-node routing target
    zeptodb::core::ZeptoPipeline* pipeline_ = nullptr;

    // Multi-node routing
    zeptodb::cluster::NodeId local_id_ = 0;
    std::shared_ptr<zeptodb::cluster::PartitionRouter> router_;
    std::unordered_map<zeptodb::cluster::NodeId,
        std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes_;

    // Thread control
    std::atomic<bool> running_{false};
    std::thread       poll_thread_;

    // Stats (protected by stats_mu_)
    mutable std::mutex stats_mu_;
    KafkaStats         stats_;

    // Opaque Kafka handle (RdKafka::KafkaConsumer* when APEX_KAFKA_AVAILABLE)
    void* consumer_handle_ = nullptr;
};

} // namespace zeptodb::feeds
