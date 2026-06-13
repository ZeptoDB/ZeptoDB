#pragma once
// ============================================================================
// ZeptoDB: OPC-UA Consumer / Server (Physical AI / Industry)
// ============================================================================
// OPC-UA (IEC 62541) client that subscribes to one or more server-side nodes
// and routes each DataChange notification as a TickMessage into the ZeptoDB
// ingestion pipeline.  This is the third connector after KafkaConsumer and
// MqttConsumer, and intentionally reuses the same architecture:
//
//     connector layer  ──ingest_tick──►  ZeptoPipeline (engine)
//
// Single-node:   set_pipeline()  → ticks → local ZeptoPipeline
// Multi-node:    set_routing()   → PartitionRouter picks local vs remote;
//                                  remote sent via TcpRpcClient::ingest_tick
//
// Compile-time optional: define ZEPTO_OPCUA_AVAILABLE and link `open62541`
// (MPL-2.0) to enable real server connectivity. Without it, start() returns
// false but all pure logic (NodeId→SymbolId mapping, variant coercion,
// 1601-epoch conversion, routing, backpressure, stats) is fully functional
// and unit-testable — matching the KafkaConsumer / MqttConsumer pattern.
//
// Deliberate deltas vs Kafka / MQTT:
//   1. No MessageFormat — OPC-UA is a typed wire protocol, not JSON/binary
//      blobs.  Variants are coerced to int64 via `value_scale` per node.
//   2. No top-level symbol_map — each `OpcUaNodeMap` entry binds one
//      NodeId to one SymbolId directly (with its own scale).
//   3. Subscription params (publishing_interval_ms / sampling_interval_ms /
//      queue_size / discard_oldest) replace Kafka's poll / commit knobs
//      and MQTT's QoS.
//
// Shipped scope:
//   * Live UA_Client subscriptions, reconnect, Basic256Sha256, quality mapping.
//   * Scalar, array, string, Structured-field, Historical Access, and A&C hooks.
//   * OpcUaServer exposes configured ZeptoDB symbols as OPC-UA variable nodes.
//   * Default builds without open62541 fail closed for live connectivity while
//     keeping all pure decode/replay/snapshot contracts unit-testable.
// ============================================================================

#include "zeptodb/common/types.h"
#include "zeptodb/ingestion/tick_plant.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/rpc_client_base.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeptodb::feeds {

// ============================================================================
// OpcUaNodeMap: bind one OPC-UA NodeId to one engine SymbolId
// ============================================================================
// `node_id` is the OPC-UA string-form identifier (e.g. "ns=2;s=Temperature").
// `value_scale` multiplies Float/Double variants to encode them as int64.
struct OpcUaNodeMap {
    std::string node_id;
    SymbolId    symbol_id   = 0;
    double      value_scale = 10000.0;
    uint32_t    array_symbol_stride = 1;  ///< array[i] maps to symbol_id + i * stride
};

// ============================================================================
// OpcUaConfig: consumer configuration
// ============================================================================
struct OpcUaConfig {
    // opc.tcp://host:4840 (binary) is by far the most common endpoint.
    std::string endpoint    = "opc.tcp://localhost:4840";
    std::string client_name = "zepto-opcua-client";

    enum class SecurityMode   : uint8_t { None = 0, Sign, SignAndEncrypt };
    enum class SecurityPolicy : uint8_t { None = 0, Basic256Sha256 };

    SecurityMode   security_mode   = SecurityMode::None;
    SecurityPolicy security_policy = SecurityPolicy::None;

    std::string username;
    std::string password;
    std::string client_cert_path;   // required when Sign / SignAndEncrypt
    std::string client_key_path;
    std::string server_cert_path;   // trusted server cert (optional)

    // Subscription parameters — mapped 1:1 onto UA_CreateSubscriptionRequest.
    double   publishing_interval_ms = 100.0;
    double   sampling_interval_ms   = 50.0;
    uint32_t queue_size             = 10;
    bool     discard_oldest         = true;

    // NodeId → SymbolId mapping (REQUIRED; empty = nothing to subscribe to).
    std::vector<OpcUaNodeMap> nodes;

    // Optional destination table (devlog 084 pattern). Empty = legacy path
    // (msg.table_id = 0).  Resolved once in set_pipeline() via SchemaRegistry.
    std::string table_name;

    // Backpressure: retry when ingestion ring buffer is full.
    int backpressure_retries  = 3;
    int backpressure_sleep_us = 100;

    // Reconnect / timeout knobs honored by the real UA_Client integration.
    uint32_t connect_timeout_ms    = 5000;   ///< initial UA_Client_connect timeout
    uint32_t session_timeout_ms    = 60000;  ///< server session timeout
    uint32_t reconnect_interval_ms = 2000;   ///< base reconnect backoff (BACKLOG P9 #2i)

    // Sector-aware default profiles (BACKLOG P9 #2q).
    enum class Profile {
        Generic,   ///< current defaults (queue_size=10, retries=3)
        Fab,       ///< semiconductor fab: 10 KHz sampling, 99% burst survival
        Auto,      ///< auto factory: 1 KHz sampling, ~200 K/s
        Steel,     ///< steel mill: 5 KHz vibration, ~250 K/s
    };

    // ------------------------------------------------------------------
    // UA StatusCode → TickMessage.volume mapping (BACKLOG P9 #2j, devlog 107).
    // ------------------------------------------------------------------
    enum class QualityHandling : uint8_t {
        IgnoreBad,        ///< drop Bad-quality samples at decode time (decode_errors++)
        AcceptAll,        ///< forward everything, set TickMessage.volume = status as-is
        AcceptAllGoodAs1, ///< forward everything; volume=1 if GOOD, 0 otherwise (default)
    };

    /// How to handle UA_DataValue.status on each notification. Default
    /// AcceptAllGoodAs1 gives query-friendly boolean quality without
    /// dropping samples. IgnoreBad is useful when downstream aggregates
    /// cannot tolerate uncertain values. AcceptAll preserves the raw
    /// 32-bit status code.
    QualityHandling quality_handling = QualityHandling::AcceptAllGoodAs1;

    /// Call BEFORE start(). Overwrites queue_size, sampling_interval_ms,
    /// publishing_interval_ms, backpressure_retries with sector defaults.
    /// User-set values after this call win.
    void apply_profile(Profile p);
};

// ============================================================================
// OpcUaStats: per-consumer statistics (snapshot) — mirrors MqttStats exactly.
// ============================================================================
struct OpcUaStats {
    uint64_t messages_consumed = 0;
    uint64_t bytes_consumed    = 0;
    uint64_t decode_errors     = 0;
    uint64_t route_local       = 0;
    uint64_t route_remote      = 0;
    uint64_t ingest_failures   = 0;
    uint64_t reconnects        = 0;  ///< successful reconnects (BACKLOG P9 #2i, devlog 109)
};

// ============================================================================
// OpcUaConsumer
// ============================================================================
class OpcUaConsumer {
public:
    explicit OpcUaConsumer(OpcUaConfig config);
    ~OpcUaConsumer();

    OpcUaConsumer(const OpcUaConsumer&)            = delete;
    OpcUaConsumer& operator=(const OpcUaConsumer&) = delete;

    // ------------------------------------------------------------------
    // Routing setup — call before start()
    // ------------------------------------------------------------------
    void set_pipeline(zeptodb::core::ZeptoPipeline* pipeline);
    void set_routing(
        zeptodb::cluster::NodeId local_id,
        std::shared_ptr<zeptodb::cluster::PartitionRouter> router,
        std::unordered_map<zeptodb::cluster::NodeId,
            std::shared_ptr<zeptodb::cluster::RpcClientBase>> remotes);

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------
    /// Connect to the OPC-UA server, create subscription, monitor all nodes.
    /// Returns false if:
    ///   - config_.nodes is empty
    ///   - security config is invalid (see is_valid_security)
    ///   - ZEPTO_OPCUA_AVAILABLE is not defined
    ///   - license does not grant Feature::IOT_CONNECTORS
    bool start();
    void stop();
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // ------------------------------------------------------------------
    // Statistics
    // ------------------------------------------------------------------
    OpcUaStats stats() const;

    // ------------------------------------------------------------------
    // Public handlers — testable without a live OPC-UA server
    // ------------------------------------------------------------------

    /// Called (from a real UA_Client callback in production, or directly
    /// from tests) for each DataChange notification.  Looks up `node_id`
    /// in the pre-built map, builds a TickMessage, and dispatches.
    ///
    /// `status` is the UA_StatusCode reported on the notification
    /// (0 = UA_STATUSCODE_GOOD).  It is consumed by
    /// `config_.quality_handling` to either drop the sample
    /// (`IgnoreBad` + non-GOOD → `decode_errors++`) or encode it into
    /// `TickMessage.volume` (`AcceptAll` = raw status, `AcceptAllGoodAs1`
    /// = 1 if GOOD else 0).  Default 0 keeps every existing caller —
    /// including the unit tests — on the GOOD/`volume=1` path with
    /// no source change.  BACKLOG P9 #2j, devlog 107.
    bool on_data_change(const std::string& node_id,
                        int64_t value,
                        uint64_t source_ts_ns,
                        int64_t status = 0);

    /// Route a pre-decoded TickMessage (skips NodeId lookup).  Shares the
    /// single-vs-multi-node dispatch + backpressure retry path with
    /// MqttConsumer::ingest_decoded.
    bool ingest_decoded(zeptodb::ingestion::TickMessage msg);

    /// Explicitly bump `decode_errors` for an unsupported UA variant type
    /// (invoked by the open62541 bridge in `handle_data_change` when
    /// `variant_to_int64` fails, and directly from tests). No dispatch;
    /// this is a pure stats transition. BACKLOG P9 Sprint-2 polish 2,
    /// devlog 110.
    void on_unsupported_variant();

    // ------------------------------------------------------------------
    // Static helpers — pure, testable
    // ------------------------------------------------------------------

    /// Minimal variant representation for unit testing the coerce path
    /// without pulling in `open62541.h`.  Mirrors the subset of UA_Variant
    /// types supported by the scalar coercion path.
    enum class VariantType : uint8_t {
        Int16, Int32, Int64,
        Float, Double,
        Boolean,
        Unsupported
    };
    struct Variant {
        VariantType type = VariantType::Unsupported;
        // Tagged-union payload kept out of the struct for simplicity:
        // callers pass the concrete field they need via `from_*` helpers.
        int64_t i64 = 0;
        double  f64 = 0.0;
        bool    b   = false;
    };

    /// One field decoded from an OPC-UA Structured variant. `symbol_id` is
    /// explicit because server field order and field names are not stable
    /// enough to infer engine partition identity.
    struct StructuredField {
        std::string field_name;
        SymbolId    symbol_id = 0;
        Variant     value;
        double      value_scale = 1.0;
        std::string engineering_unit;
    };

    /// One historical value read from a server historian. This shares the
    /// normal NodeId map and quality policy so HA backfills land identically
    /// to live subscription samples.
    struct HistoricalSample {
        std::string node_id;
        Variant     value;
        uint64_t    source_ts_ns = 0;
        int64_t     status = 0;
    };

    /// Live Historical Access read parameters. Timestamps are Unix
    /// nanoseconds and are converted to OPC-UA DateTime internally.
    struct HistoryReadOptions {
        uint64_t start_ts_ns = 0;
        uint64_t end_ts_ns = 0;
        uint32_t values_per_node = 0;  ///< 0 lets the server choose the batch size
        bool return_bounds = false;
    };

    /// OPC-UA Alarms & Conditions event mapped into a tick stream. Active
    /// alarms store positive severity; cleared alarms store negative severity.
    struct AlarmEvent {
        SymbolId symbol_id = 0;
        int64_t  severity = 0;
        bool     active = true;
        uint64_t source_ts_ns = 0;
        int64_t  status = 0;
    };

    /// Dispatch fields from one Structured variant. Returns true only if every
    /// field is supported and successfully routed.
    bool on_structured_change(const std::vector<StructuredField>& fields,
                              uint64_t source_ts_ns,
                              int64_t status = 0);

    /// Expand one OPC-UA array DataValue into one TickMessage per element.
    /// The first element uses the node's configured symbol_id; later elements
    /// use `symbol_id + index * array_symbol_stride`. Unsupported elements
    /// bump decode_errors and make the aggregate return value false.
    bool on_array_change(const std::string& node_id,
                         const std::vector<Variant>& values,
                         uint64_t source_ts_ns,
                         int64_t status = 0);

    /// Map a UA String value into a deterministic dictionary/symbol code and
    /// dispatch it through the same scalar route. When a local pipeline is
    /// configured, the pipeline symbol dictionary owns the code; otherwise a
    /// stable FNV-1a fallback code is used so remote-only tests and consumers
    /// can still route the value without a local storage target.
    bool on_string_change(const std::string& node_id,
                          const std::string& value,
                          uint64_t source_ts_ns,
                          int64_t status = 0);

    /// Replay Historical Access samples through the normal live-ingest path.
    /// Returns the number of samples successfully routed.
    size_t ingest_history(const std::vector<HistoricalSample>& samples);

    /// Read Historical Access samples from the live open62541 client session
    /// and route them through the same path as subscription samples. Returns
    /// the number of historical data values successfully routed. Default
    /// builds without open62541 return 0 and bump ingest_failures once.
    size_t read_history(const HistoryReadOptions& options);

    /// Dispatch an Alarms & Conditions event as a dedicated tick stream.
    bool on_alarm_event(const AlarmEvent& event);

    /// Coerce a Variant to int64 using `value_scale` for float/double.
    /// Returns false on Unsupported types (caller bumps decode_errors).
    static bool coerce_variant_to_int64(const Variant& v,
                                        double value_scale,
                                        int64_t& out);

    /// Convert UA DateTime (100 ns ticks since 1601-01-01 UTC) to ns since
    /// the Unix epoch.  Returns 0 for values before 1970 (clamp to epoch).
    static uint64_t ua_datetime_to_ns(int64_t ua_datetime_100ns_since_1601);

    /// Convert Unix ns to UA DateTime (100 ns ticks since 1601-01-01 UTC).
    static int64_t ns_to_ua_datetime(uint64_t unix_ns);

    /// Minimal security-config sanity check.  Returns false on obviously
    /// inconsistent combos (e.g. Sign with Policy::None).  Does NOT attempt
    /// to validate certificate files on disk — that is production work.
    static bool is_valid_security(OpcUaConfig::SecurityMode mode,
                                  OpcUaConfig::SecurityPolicy policy);

private:
    OpcUaConfig config_;

    // Single-node routing target
    zeptodb::core::ZeptoPipeline* pipeline_ = nullptr;

    // Multi-node routing
    zeptodb::cluster::NodeId local_id_ = 0;
    std::shared_ptr<zeptodb::cluster::PartitionRouter> router_;
    std::unordered_map<zeptodb::cluster::NodeId,
        std::shared_ptr<zeptodb::cluster::RpcClientBase>> remotes_;

    std::atomic<bool> running_{false};

    mutable std::mutex stats_mu_;
    OpcUaStats         stats_;

    // NodeId → (SymbolId, value_scale) lookup, built in start() from
    // config_.nodes so on_data_change() is O(1) per notification.
    struct NodeBinding {
        SymbolId symbol_id;
        double   value_scale;
        uint32_t array_symbol_stride;
    };
    std::unordered_map<std::string, NodeBinding> node_map_;

    // Opaque handles reserved for the production UA_Client integration.
    void*    client_handle_   = nullptr;  // UA_Client* when ZEPTO_OPCUA_AVAILABLE
    uint32_t subscription_id_ = 0;

    // (Re)create subscription + monitored items on the current UA_Client.
    // Shared between initial connect in start() and reconnect in
    // run_iterate_loop().  No-op stub when ZEPTO_OPCUA_AVAILABLE is
    // undefined.  BACKLOG P9 #2i, devlog 109.
    bool setup_subscription();

    // Background UA_Client_run_iterate() loop with disconnect detection
    // and jittered exponential-backoff reconnect.  BACKLOG P9 #2i.
    void run_iterate_loop();

    // Resolved destination table_id (devlog 084 pattern). 0 = legacy path.
    uint16_t table_id_ = 0;

    bool dispatch_value(SymbolId symbol_id,
                        int64_t value,
                        uint64_t source_ts_ns,
                        int64_t status,
                        size_t bytes_consumed);
};

// ============================================================================
// OPC-UA server mode
// ============================================================================

struct OpcUaServerNode {
    std::string node_id;       ///< empty = generated from namespace_index + symbol_id
    SymbolId    symbol_id = 0;
    std::string display_name;
    int64_t     initial_value = 0;
};

struct OpcUaServerConfig {
    std::string endpoint = "opc.tcp://0.0.0.0:4840";
    std::string application_name = "zepto-opcua-server";
    uint16_t namespace_index = 1;
    uint32_t iterate_sleep_ms = 50;
    std::vector<OpcUaServerNode> nodes;
};

struct OpcUaServerStats {
    uint64_t writes = 0;
    uint64_t write_failures = 0;
    uint64_t start_failures = 0;
};

/// Minimal ZeptoDB-as-OPC-UA-server wrapper. The engine can publish current
/// symbol values via publish_value(); open62541 builds expose configured
/// symbols as OPC-UA variable nodes, while default builds keep the same
/// snapshot contract testable and return false from start().
class OpcUaServer {
public:
    explicit OpcUaServer(OpcUaServerConfig config);
    ~OpcUaServer();

    OpcUaServer(const OpcUaServer&) = delete;
    OpcUaServer& operator=(const OpcUaServer&) = delete;

    bool start();
    void stop();
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    bool publish_value(SymbolId symbol_id,
                       int64_t value,
                       uint64_t source_ts_ns = 0,
                       int64_t status = 0);
    std::optional<int64_t> snapshot_value(SymbolId symbol_id) const;
    OpcUaServerStats stats() const;

private:
    struct NodeState {
        std::string node_id;
        std::string display_name;
        int64_t value = 0;
        uint64_t source_ts_ns = 0;
        int64_t status = 0;
    };

    OpcUaServerConfig config_;
    std::atomic<bool> running_{false};

    mutable std::mutex mu_;
    std::unordered_map<SymbolId, NodeState> values_;

    mutable std::mutex stats_mu_;
    OpcUaServerStats stats_;

    void* server_handle_ = nullptr;  // UA_Server* when ZEPTO_OPCUA_AVAILABLE

    bool write_live_value(SymbolId symbol_id, const NodeState& state);
    void bump_start_failure();
    void bump_write_failure();
};

} // namespace zeptodb::feeds
