#pragma once
// ============================================================================
// ZeptoDB: Arrow Flight Server
// ============================================================================
// Arrow Flight RPC server for zero-copy-grade data streaming.
// Python/Polars/Pandas clients connect via flight:// protocol.
//
// Supported RPCs:
//   GetFlightInfo  — schema + row count for a SQL query or table
//   DoGet          — execute SQL, stream results as Arrow RecordBatches
//   DoPut          — ingest Arrow RecordBatches into a table
//   ListFlights    — list available tables
//   DoAction       — "ping", "healthcheck"
//   ListActions    — list supported actions
//
// Usage:
//   FlightServer srv(executor);
//   srv.start(8815);          // blocking
//   srv.start_async(8815);    // background thread
//   srv.stop();
// ============================================================================

#include "zeptodb/sql/executor.h"
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <atomic>

namespace zeptodb::server {

class FlightServer {
public:
    explicit FlightServer(zeptodb::sql::QueryExecutor& executor);
    ~FlightServer();

    FlightServer(const FlightServer&) = delete;
    FlightServer& operator=(const FlightServer&) = delete;

    /// Start blocking on the calling thread.
    void start(uint16_t port = 8815);

    /// Start on a background thread.
    void start_async(uint16_t port = 8815);

    /// Stop the server.
    void stop();

    bool running() const { return running_.load(); }
    int  port()    const { return port_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    zeptodb::sql::QueryExecutor& executor_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int port_{0};
};

} // namespace zeptodb::server
