#pragma once
// ============================================================================
// ZeptoDB: Arrow Flight Server
// ============================================================================
// Arrow Flight RPC server for zero-copy-grade data streaming.
// Python/Polars/Pandas clients connect via flight:// protocol.
//
// Supported RPCs:
//   GetFlightInfo  — schema + row count for a read-only SQL query
//   DoGet          — execute SQL, stream results as Arrow RecordBatches
//   DoPut          — ingest Arrow RecordBatches into a table
//   ListFlights    — list available tables
//   DoAction       — "ping", "healthcheck"
//   ListActions    — list supported actions
//
// Usage:
//   FlightServer srv(executor, auth);
//   srv.start(8815);          // blocking, loopback-only development default
//   srv.start_async(8815);    // background thread
//   srv.stop();
// ============================================================================

#include "zeptodb/auth/auth_manager.h"
#include "zeptodb/auth/tenant_manager.h"
#include "zeptodb/sql/executor.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace zeptodb::server {

/// Network and TLS settings for Arrow Flight.
/// Plaintext non-loopback listeners are rejected unless the explicit
/// development-only override is set.
struct FlightServerConfig {
    std::string host = "127.0.0.1";  ///< Bind host or address.
    uint16_t port = 8815;            ///< TCP port; zero auto-assigns one.
    std::string tls_cert_path;       ///< PEM certificate path; paired with key.
    std::string tls_key_path;        ///< PEM private-key path; paired with cert.
    bool allow_insecure_non_loopback = false;  ///< Development-only override.
    size_t max_query_rows = 100000;  ///< Materialized rows per read RPC.
    size_t max_query_bytes = 64U * 1024U * 1024U;  ///< Arrow result bytes.
    uint32_t query_timeout_ms = 30000;  ///< Cooperative read timeout; zero disables.
    size_t max_put_rows = 100000;  ///< Cumulative rows per DoPut RPC.
    size_t max_put_bytes = 64U * 1024U * 1024U;  ///< Cumulative Arrow bytes.
    /// DoPut currently commits rows one at a time and cannot roll back a
    /// partially applied stream. Keep it disabled for production unless this
    /// experimental, non-atomic behavior is explicitly accepted.
    bool allow_non_atomic_put = false;
};

/// Arrow Flight SQL and ingestion service.
/// Thread-safe for concurrent Flight RPCs after startup; lifecycle calls must
/// be serialized. QueryExecutor must outlive in-flight requests. Shared
/// ownership is retained for auth and tenant policy. Query tickets are
/// intentionally read-only.
class FlightServer {
public:
    explicit FlightServer(
        zeptodb::sql::QueryExecutor& executor,
        std::shared_ptr<zeptodb::auth::AuthManager> auth = nullptr,
        std::shared_ptr<zeptodb::auth::TenantManager> tenant_manager = nullptr);
    ~FlightServer();

    FlightServer(const FlightServer&) = delete;
    FlightServer& operator=(const FlightServer&) = delete;

    /// Start blocking on loopback using plaintext transport.
    /// This compatibility overload is intended for local development/tests.
    void start(uint16_t port = 8815);

    /// Start blocking with explicit network/TLS settings.
    /// Returns false on invalid configuration or listener initialization error.
    bool start(const FlightServerConfig& config);

    /// Start on a loopback background listener for local development/tests.
    void start_async(uint16_t port = 8815);

    /// Start on a background thread with explicit network/TLS settings.
    /// Returns false before spawning the thread when validation or listener
    /// initialization fails.
    bool start_async(const FlightServerConfig& config);

    /// Stop the server.
    void stop();

    /// Return whether the server is currently serving RPCs.
    bool running() const { return running_.load(); }

    /// Return the bound TCP port after successful initialization.
    int port() const { return port_; }

    /// Return the most recent startup or serving error.
    const std::string& last_error() const { return last_error_; }

private:
    struct Impl;
    bool initialize(const FlightServerConfig& config);
    void serve();

    std::unique_ptr<Impl> impl_;

    zeptodb::sql::QueryExecutor& executor_;
    std::shared_ptr<zeptodb::auth::AuthManager> auth_;
    std::shared_ptr<zeptodb::auth::TenantManager> tenant_manager_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int port_{0};
    std::string last_error_;
};

} // namespace zeptodb::server
