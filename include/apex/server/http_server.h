#pragma once
// ============================================================================
// APEX-DB: HTTP API Server
// ============================================================================
// cpp-httplib based lightweight HTTP/HTTPS server.
// ClickHouse compatible port 8123 — works with Grafana/CH clients.
//
// Endpoints:
//   POST /        — Execute SQL query (body: SQL string)
//   GET  /ping    — Health check (ClickHouse compatible)
//   GET  /health  — Kubernetes liveness probe
//   GET  /ready   — Kubernetes readiness probe
//   GET  /stats   — Pipeline statistics
//   GET  /metrics — Prometheus metrics (OpenMetrics format)
//
// Security:
//   TLS/HTTPS     — Enabled via TlsConfig (cert + key PEM paths)
//   Authentication — API Key (Bearer) or JWT/SSO (OIDC)
//   Authorization  — RBAC: admin/writer/reader/analyst/metrics roles
// ============================================================================

#include "apex/sql/executor.h"
#include "apex/auth/auth_manager.h"
#include "apex/auth/query_tracker.h"
#include <cstdint>
#include <string>
#include <memory>
#include <thread>
#include <atomic>

// Forward declaration — httplib is included only in the .cpp (compile speed)
namespace httplib { class Server; }

namespace apex::server {

// ============================================================================
// HttpServer
// ============================================================================
class HttpServer {
public:
    /// Construct without authentication (development / trusted network mode).
    explicit HttpServer(apex::sql::QueryExecutor& executor,
                        uint16_t port = 8123);

    /// Construct with TLS and/or authentication.
    explicit HttpServer(apex::sql::QueryExecutor& executor,
                        uint16_t port,
                        apex::auth::TlsConfig tls,
                        std::shared_ptr<apex::auth::AuthManager> auth = nullptr);

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

private:
    void setup_routes();
    void setup_auth_middleware();
    void setup_admin_routes();

    // Execute a query with optional timeout and QueryTracker registration.
    // subject is the identity string (remote_addr when auth is off).
    apex::sql::QueryResultSet run_query_with_tracking(
        const std::string& sql, const std::string& subject);

    static std::string build_json_response(
        const apex::sql::QueryResultSet& result);
    static std::string build_error_json(const std::string& msg);
    static std::string build_stats_json(
        const apex::core::PipelineStats& stats);
    std::string build_prometheus_metrics() const;

    apex::sql::QueryExecutor&                executor_;
    uint16_t                                 port_;
    apex::auth::TlsConfig                    tls_;
    std::shared_ptr<apex::auth::AuthManager> auth_;
    std::unique_ptr<httplib::Server>         svr_;
    std::thread                              thread_;
    std::atomic<bool>                        running_{false};
    std::atomic<bool>                        ready_{false};
    uint32_t                                 query_timeout_ms_{0};
    apex::auth::QueryTracker                 query_tracker_;
};

} // namespace apex::server
