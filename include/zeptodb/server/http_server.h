#pragma once
// ============================================================================
// ZeptoDB: HTTP API Server
// ============================================================================
// cpp-httplib based lightweight HTTP/HTTPS server.
// ClickHouse compatible port 8123 — works with Grafana/CH clients.
//
// Endpoints:
//   POST /        — Execute SQL query (body: SQL string)
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

namespace zeptodb::server {

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

    // Execute a query with optional timeout and QueryTracker registration.
    // subject is the identity string (remote_addr when auth is off).
    zeptodb::sql::QueryResultSet run_query_with_tracking(
        const std::string& sql, const std::string& subject);

    static std::string build_json_response(
        const zeptodb::sql::QueryResultSet& result);
    static std::string build_error_json(const std::string& msg);
    static std::string build_stats_json(
        const zeptodb::core::PipelineStats& stats);
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

    // Cluster coordinator (null = standalone mode)
    zeptodb::cluster::QueryCoordinator*                     coordinator_ = nullptr;

    // Rebalance manager (null = rebalance not available)
    zeptodb::cluster::RebalanceManager*                     rebalance_mgr_ = nullptr;

    // PTP clock detector (null = not configured)
    zeptodb::cluster::PtpClockDetector*                     ptp_detector_ = nullptr;
};

} // namespace zeptodb::server
