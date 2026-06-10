#pragma once
// ============================================================================
// ZeptoDB: HTTP API Server
// ============================================================================
// cpp-httplib based lightweight HTTP/HTTPS server.
// ClickHouse compatible port 8123 — works with Grafana/CH clients.
//
// Endpoints:
//   POST /        — Execute SQL query (body: SQL string)
//                    Response is JSON by default; returns Arrow IPC stream
//                    (`application/vnd.apache.arrow.stream`) when the client
//                    requests it via `Accept: application/vnd.apache.arrow.stream`,
//                    `?default_format=Arrow`, or `?format=arrow`. Errors
//                    always stay JSON regardless of Accept (devlog 119).
//   POST /insert/arrow
//                 — Ingest an Arrow IPC RecordBatchStream into ZeptoDB ticks.
//                    Query params map columns (`table`, `sym_col`, `price_col`,
//                    `vol_col`, `ts_col`) and numeric scales.
//   GET  /        — Execute SQL via `?query=` param (always JSON)
//   GET  /ping    — Health check (ClickHouse compatible)
//   GET  /health  — Kubernetes liveness probe
//   GET  /ready   — Kubernetes readiness probe
//   GET  /whoami  — Return authenticated role and subject
//   GET  /stats   — Pipeline statistics
//   GET  /metrics — Prometheus metrics (OpenMetrics format)
//
// Observability:
//   - Structured JSON access log (every request): request_id, method, path,
//     status, duration_us, request/response bytes, remote_addr, subject
//   - Slow query log (>100ms): query_id, SQL (truncated), duration, rows, error
//   - X-Request-Id response header for client-side correlation
//   - Prometheus: zepto_http_requests_total, zepto_http_active_sessions
//   - Server lifecycle events: start, stop (with port, TLS, auth config)
//
// Security:
//   TLS/HTTPS     — Enabled via TlsConfig (cert + key PEM paths)
//   Authentication — API Key (Bearer) or JWT/SSO (OIDC)
//   Authorization  — RBAC: admin/writer/reader/analyst/metrics roles
// ============================================================================

#include "zeptodb/sql/executor.h"
#include "zeptodb/auth/auth_manager.h"
#include "zeptodb/auth/query_tracker.h"
#include "zeptodb/auth/tenant_manager.h"
#include "zeptodb/server/metrics_collector.h"
#include "zeptodb/ai/agent_memory_router.h"
#include "zeptodb/cluster/query_coordinator.h"
#include "zeptodb/cluster/rebalance_manager.h"
#include "zeptodb/cluster/ptp_clock_detector.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <vector>

// Forward declaration — httplib is included only in the .cpp (compile speed)
namespace httplib { class Server; }

namespace zeptodb::ai {
class AgentMemoryStore;
struct CacheEntry;
struct CacheLookup;
struct CacheLookupResult;
struct AgentMemoryEvictionEvent;
struct MemoryGetResult;
struct MemoryQuery;
struct MemoryRecord;
struct MemorySearchResult;
struct StoreResult;
}
namespace zeptodb::cluster { class TcpRpcClient; }

namespace zeptodb::server {

enum class AgentMemoryReplicationMode {
    Routed,
    Quorum,
    Sync,
};

/// Result for an Agent Memory owner failover transition.
struct AgentMemoryOwnerFailoverResult {
    bool ok = false;
    bool adopted = false;
    bool replica_promoted = false;
    bool degraded = false;
    bool replay_source_missing = false;
    zeptodb::ai::AgentMemoryNodeId source_node_id = 0;
    zeptodb::ai::AgentMemoryNodeId replacement_node_id = 0;
    uint64_t source_ring_epoch = 0;
    uint64_t new_ring_epoch = 0;
    std::string degraded_reason;
    std::string error;
};

// ============================================================================
// ConnectionInfo — active client session tracking (.z.po/.z.pc equivalent)
// ============================================================================
struct ConnectionInfo {
    std::string remote_addr;        // IP:port of the client
    std::string user;               // subject from auth (or remote_addr if no auth)
    int64_t     connected_at_ns = 0; // epoch-ns of first request
    int64_t     last_active_ns  = 0; // epoch-ns of most recent request
    uint64_t    query_count     = 0; // number of requests from this session
};

// ============================================================================
// HttpServer
// ============================================================================
class HttpServer {
public:
    /// Construct without authentication (development / trusted network mode).
    explicit HttpServer(zeptodb::sql::QueryExecutor& executor,
                        uint16_t port = 8123);

    /// Construct with TLS and/or authentication.
    explicit HttpServer(zeptodb::sql::QueryExecutor& executor,
                        uint16_t port,
                        zeptodb::auth::TlsConfig tls,
                        std::shared_ptr<zeptodb::auth::AuthManager> auth = nullptr);

    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// Start blocking (uses calling thread).
    void start();

    /// Start on a background thread.
    void start_async();

    /// Stop the server.
    void stop();

    uint16_t port()    const { return port_; }
    bool     running() const { return running_.load(); }

    /// Mark the server as ready (called after pipeline initialization).
    void set_ready(bool ready) { ready_.store(ready); }

    /// Set query execution timeout (0 = disabled).
    void set_query_timeout_ms(uint32_t ms) { query_timeout_ms_ = ms; }

    /// Serve static files from directory (Web UI).
    /// Must be called before start(). Mounts at "/".
    void set_web_dir(const std::string& dir);

    /// Set tenant manager for multi-tenancy quota enforcement.
    void set_tenant_manager(std::shared_ptr<zeptodb::auth::TenantManager> mgr) {
        tenant_mgr_ = std::move(mgr);
    }

    // ---- Connection hooks (.z.po / .z.pc equivalent) ----------------------

    /// Called when a new client session is detected (first request from addr).
    void set_on_connect(std::function<void(const ConnectionInfo&)> fn) {
        on_connect_ = std::move(fn);
    }

    /// Called when a session ends (Connection: close detected from client).
    void set_on_disconnect(std::function<void(const ConnectionInfo&)> fn) {
        on_disconnect_ = std::move(fn);
    }

    /// Return snapshot of all currently tracked sessions.
    std::vector<ConnectionInfo> list_sessions() const;

    /// Evict sessions that have been inactive for longer than timeout_ms.
    /// Returns the number of sessions evicted.
    size_t evict_idle_sessions(int64_t timeout_ms);

    // ---- Extensible /metrics providers ------------------------------------

    /// Register a callback that contributes additional Prometheus/OpenMetrics
    /// text to the GET /metrics response.  The function must return a
    /// newline-terminated string in OpenMetrics text format.
    ///
    /// Example (Kafka consumer):
    ///   server.add_metrics_provider([&consumer]() {
    ///       return zeptodb::feeds::KafkaConsumer::format_prometheus(
    ///           "market-data", consumer.stats());
    ///   });
    ///
    /// Thread-safe: providers are called while holding an internal mutex.
    void add_metrics_provider(std::function<std::string()> provider);

    /// Access the internal metrics collector (for testing or custom queries).
    MetricsCollector* metrics_collector() { return metrics_collector_.get(); }

    /// Access the agent memory store backing /api/ai/* endpoints.
    /// The store lives with the HTTP server and is in-memory for v0.
    zeptodb::ai::AgentMemoryStore& agent_memory_store();

    /// Enable Agent Memory sidecar persistence and load any existing snapshot.
    /// Standalone mode uses the directory directly; routed mode uses
    /// node-{node_id}/shard-0 under it and validates the shard manifest. The
    /// server saves after flush_every_mutations memory/cache mutations and again
    /// during stop(). Set flush_every_mutations=0 to flush only on stop().
    bool set_agent_memory_persistence(const std::string& directory,
                                      std::string* error = nullptr,
                                      size_t flush_every_mutations = 100);

    /// Enable routed Agent Memory HTTP operations. The default remains
    /// node-local until this is called. Remote writes, point memory lookup,
    /// search fan-out, context fan-out, and exact cache lookup use TcpRpc opaque
    /// payloads. Remote write clients inherit config.ring_epoch so data nodes
    /// with fencing enabled can reject stale owner mutations. Returns false
    /// when shard-local persistence validation or load fails for this node.
    bool set_agent_memory_routing(
        zeptodb::ai::AgentMemoryRouterConfig config,
        const std::vector<zeptodb::ai::AgentMemoryNodeId>& nodes,
        std::unordered_map<zeptodb::ai::AgentMemoryNodeId,
                           std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes,
        std::string* error = nullptr);

    /// Configure owner-side Agent Memory WAL replica acknowledgement policy.
    /// Routed is the default single-owner mode. Quorum requires enough replica
    /// WAL acknowledgements to form a majority with the owner commit marker.
    /// Sync requires every configured remote replica before owner commit/ACK.
    void set_agent_memory_replication_mode(AgentMemoryReplicationMode mode) {
        std::lock_guard<std::mutex> lock(agent_memory_routing_mu_);
        agent_memory_replication_mode_ = mode;
    }
    AgentMemoryReplicationMode agent_memory_replication_mode() const {
        std::lock_guard<std::mutex> lock(agent_memory_routing_mu_);
        return agent_memory_replication_mode_;
    }

    /// Load a failed owner's shard-local snapshot/WAL into this node's current
    /// Agent Memory store, then publish it into this node's configured shard
    /// snapshot path. Intended for explicit failover orchestration after the
    /// routing ring has reassigned the old owner away from source_node_id.
    bool adopt_agent_memory_owner_shard(
        zeptodb::ai::AgentMemoryNodeId source_node_id,
        uint64_t source_ring_epoch,
        std::string* error = nullptr);

    /// Apply a failed Agent Memory owner transition to this HTTP server. The
    /// server switches its local Agent Memory ring to live_nodes/new_ring_epoch.
    /// The deterministic replacement is the first live node whose id is greater
    /// than source_node_id, wrapping to the lowest live id; only that node
    /// adopts the failed owner's persisted shard.
    AgentMemoryOwnerFailoverResult handle_agent_memory_owner_failover(
        zeptodb::ai::AgentMemoryNodeId source_node_id,
        uint64_t source_ring_epoch,
        uint64_t new_ring_epoch,
        const std::vector<zeptodb::ai::AgentMemoryNodeId>& live_nodes);

    /// TcpRpc callbacks for remote Agent Memory operations. Write callbacks
    /// return serialized StoreResult payloads and mark local persistence dirty
    /// on successful writes. Read callbacks return serialized lookup/stats
    /// payloads.
    std::vector<uint8_t> handle_agent_memory_put_rpc(const uint8_t* data, size_t len);
    std::vector<uint8_t> handle_agent_cache_store_rpc(const uint8_t* data, size_t len);
    std::vector<uint8_t> handle_agent_memory_delete_rpc(const uint8_t* data, size_t len);
    std::vector<uint8_t> handle_agent_cache_delete_rpc(const uint8_t* data, size_t len);
    std::vector<uint8_t> handle_agent_memory_get_rpc(const uint8_t* data, size_t len);
    std::vector<uint8_t> handle_agent_memory_search_rpc(const uint8_t* data, size_t len);
    std::vector<uint8_t> handle_agent_cache_lookup_rpc(const uint8_t* data, size_t len);
    std::vector<uint8_t> handle_agent_memory_stats_rpc(const uint8_t* data, size_t len);
    std::vector<uint8_t> handle_agent_memory_replica_append_rpc(const uint8_t* data,
                                                                size_t len);

    /// Set query coordinator for cluster mode.
    /// When set, /admin/nodes and /admin/metrics/history aggregate from all nodes.
    /// @param node_id  This node's ID (used to tag local metrics snapshots).
    void set_coordinator(zeptodb::cluster::QueryCoordinator* coord,
                         uint16_t node_id = 0) {
        coordinator_ = coord;
        if (metrics_collector_ && node_id > 0)
            metrics_collector_->set_node_id(node_id);
    }

    /// Set rebalance manager for /admin/rebalance/* endpoints.
    void set_rebalance_manager(zeptodb::cluster::RebalanceManager* mgr) {
        rebalance_mgr_ = mgr;
    }

    /// Set PTP clock detector for /admin/clock endpoint.
    void set_ptp_detector(zeptodb::cluster::PtpClockDetector* det) {
        ptp_detector_ = det;
    }

private:
    void setup_routes();
    void setup_auth_middleware();
    void setup_admin_routes();
    void setup_session_tracking();

    // Track a request from remote_addr; fires on_connect_ / on_disconnect_.
    // is_closing=true when client sends "Connection: close".
    void track_session(const std::string& remote_addr, bool is_closing);
    bool persist_agent_memory_snapshot_(std::string* error = nullptr,
                                        bool force = false);
    bool mark_agent_memory_dirty_(std::string* error = nullptr);
    bool replay_agent_memory_wal_(const std::string& directory,
                                  std::string* error = nullptr);
    bool append_agent_memory_wal_record_(uint32_t type,
                                         const std::vector<uint8_t>& payload,
                                         std::string* error = nullptr);
    bool replicate_agent_memory_wal_record_(uint32_t type,
                                            const std::vector<uint8_t>& payload,
                                            bool local_record_counts,
                                            std::string* error = nullptr);
    uint64_t next_agent_memory_wal_tx_id_();
    bool truncate_agent_memory_wal_locked_(std::string* error = nullptr);
    bool persist_agent_memory_record_mutation_(
        const std::string& memory_id,
        const std::string& tenant_id,
        std::string* error = nullptr);
    bool persist_agent_cache_entry_mutation_(
        const std::string& tenant_id,
        const std::string& namespace_id,
        const std::string& prompt,
        std::string* error = nullptr);
    bool persist_agent_memory_delete_mutation_(
        const std::string& memory_id,
        const std::string& tenant_id,
        std::string* error = nullptr);
    bool persist_agent_cache_delete_mutation_(
        const std::string& tenant_id,
        const std::string& namespace_id,
        const std::string& prompt,
        std::string* error = nullptr);
    bool persist_agent_memory_eviction_tombstones_(
        const std::vector<zeptodb::ai::AgentMemoryEvictionEvent>& evictions,
        std::string* error = nullptr,
        size_t* failed_index = nullptr);
    zeptodb::ai::StoreResult put_agent_memory_routed_(
        zeptodb::ai::MemoryRecord record,
        bool* local_write);
    zeptodb::ai::StoreResult store_agent_cache_routed_(
        zeptodb::ai::CacheEntry entry,
        bool* local_write);
    zeptodb::ai::StoreResult delete_agent_memory_routed_(
        const std::string& memory_id,
        const std::string& namespace_id,
        const std::string& tenant_id,
        bool* local_write);
    zeptodb::ai::StoreResult delete_agent_cache_routed_(
        const std::string& tenant_id,
        const std::string& namespace_id,
        const std::string& prompt,
        bool* local_write);
    zeptodb::ai::MemoryGetResult get_agent_memory_routed_(
        const std::string& memory_id,
        const std::string& namespace_id,
        const std::string& tenant_id);
    std::vector<zeptodb::ai::MemorySearchResult> search_agent_memory_routed_(
        zeptodb::ai::MemoryQuery query,
        std::string* error);
    zeptodb::ai::CacheLookupResult lookup_agent_cache_routed_(
        zeptodb::ai::CacheLookup lookup);
    void record_agent_memory_failover_status_(
        const AgentMemoryOwnerFailoverResult& result);
    AgentMemoryOwnerFailoverResult agent_memory_failover_status_() const;

    // Execute a query with optional timeout and QueryTracker registration.
    // subject is the identity string (remote_addr when auth is off).
    zeptodb::sql::QueryResultSet run_query_with_tracking(
        const std::string& sql, const std::string& subject);

    static std::string build_json_response(
        const zeptodb::sql::QueryResultSet& result);
    static std::string build_error_json(const std::string& msg);
    static std::string build_stats_json(
        const zeptodb::core::PipelineStats& stats);
    std::string build_agent_memory_stats_json(bool cluster_scope) const;
    std::string build_prometheus_metrics() const;

    // Cluster helpers: scatter stats/metrics requests to all nodes
    std::vector<std::string> coordinator_scatter_stats();
    std::string coordinator_scatter_metrics(int64_t since_ms, size_t limit);

    zeptodb::sql::QueryExecutor&                executor_;
    uint16_t                                 port_;
    zeptodb::auth::TlsConfig                    tls_;
    std::shared_ptr<zeptodb::auth::AuthManager> auth_;
    std::unique_ptr<httplib::Server>         svr_;
    std::thread                              thread_;
    std::atomic<bool>                        running_{false};
    std::atomic<bool>                        ready_{false};
    uint32_t                                 query_timeout_ms_{0};
    zeptodb::auth::QueryTracker                 query_tracker_;

    // Multi-tenancy
    std::shared_ptr<zeptodb::auth::TenantManager> tenant_mgr_;

    // Session registry
    mutable std::mutex                                     sessions_mu_;
    std::unordered_map<std::string, ConnectionInfo>        sessions_;
    std::function<void(const ConnectionInfo&)>             on_connect_;
    std::function<void(const ConnectionInfo&)>             on_disconnect_;

    // Extensible /metrics providers
    mutable std::mutex                                     providers_mu_;
    std::vector<std::function<std::string()>>              metrics_providers_;

    // Self-metrics history collector
    std::unique_ptr<MetricsCollector>                       metrics_collector_;

    // Agent memory/context/cache API store
    std::unique_ptr<zeptodb::ai::AgentMemoryStore>           agent_memory_;
    std::mutex                                               agent_memory_persist_mu_;
    std::string                                              agent_memory_persist_base_dir_;
    std::string                                              agent_memory_persist_dir_;
    bool                                                     agent_memory_persist_routed_ = false;
    zeptodb::ai::AgentMemoryNodeId                           agent_memory_persist_node_id_ = 0;
    uint64_t                                                 agent_memory_persist_ring_epoch_ = 0;
    uint32_t                                                 agent_memory_persist_shard_id_ = 0;
    size_t                                                   agent_memory_dirty_mutations_ = 0;
    size_t                                                   agent_memory_flush_every_mutations_ = 100;
    mutable std::mutex                                       agent_memory_routing_mu_;
    std::unique_ptr<zeptodb::ai::AgentMemoryRouter>          agent_memory_router_;
    std::unordered_map<zeptodb::ai::AgentMemoryNodeId,
                       std::shared_ptr<zeptodb::cluster::TcpRpcClient>>
                                                              agent_memory_remotes_;
    AgentMemoryReplicationMode                                 agent_memory_replication_mode_ =
        AgentMemoryReplicationMode::Routed;
    mutable std::mutex                                          agent_memory_failover_mu_;
    AgentMemoryOwnerFailoverResult                             agent_memory_last_failover_;
    std::atomic<uint64_t>                                      agent_memory_wal_tx_counter_{1};

    // Cluster coordinator (null = standalone mode)
    zeptodb::cluster::QueryCoordinator*                     coordinator_ = nullptr;

    // Rebalance manager (null = rebalance not available)
    zeptodb::cluster::RebalanceManager*                     rebalance_mgr_ = nullptr;

    // PTP clock detector (null = not configured)
    zeptodb::cluster::PtpClockDetector*                     ptp_detector_ = nullptr;
};

} // namespace zeptodb::server
