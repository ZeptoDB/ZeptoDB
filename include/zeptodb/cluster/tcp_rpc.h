#pragma once
// ============================================================================
// Phase C-3: TCP RPC — inter-node SQL query transport
// ============================================================================
// TcpRpcServer: listens on a TCP port, executes incoming SQL_QUERY messages
//               via a registered callback, sends back SQL_RESULT.
//
// TcpRpcClient: connects to a TcpRpcServer, sends a SQL_QUERY, returns the
//               QueryResultSet from the response.
//
// Protocol: RpcHeader (16 bytes) + payload — see rpc_protocol.h
// One connection per request (Phase 1 simplicity; Phase 2 can add pooling).
// ============================================================================

#include "zeptodb/cluster/rpc_protocol.h"
#include "zeptodb/cluster/rpc_security.h"
#include "zeptodb/cluster/k8s_lease.h"
#include "zeptodb/ingestion/tick_plant.h"
#include "zeptodb/sql/executor.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace zeptodb::cluster {

using SqlQueryCallback   = std::function<zeptodb::sql::QueryResultSet(const std::string&)>;
using TickIngestCallback = std::function<bool(const zeptodb::ingestion::TickMessage&)>;
using WalReplayCallback  = std::function<size_t(const std::vector<zeptodb::ingestion::TickMessage>&)>;
using StatsCallback      = std::function<std::string()>;  // returns JSON
using MetricsCallback    = std::function<std::string(int64_t /*since_ms*/, uint32_t /*limit*/)>;
using RingUpdateCallback = std::function<bool(const uint8_t* data, size_t len)>;

// ============================================================================
// TcpRpcServer
// ============================================================================
class TcpRpcServer {
public:
    TcpRpcServer()  = default;
    ~TcpRpcServer() { stop(); }

    TcpRpcServer(const TcpRpcServer&)            = delete;
    TcpRpcServer& operator=(const TcpRpcServer&) = delete;

    /// Start listening.
    /// sql_cb   — invoked for each SQL_QUERY message.
    /// tick_cb  — invoked for each TICK_INGEST message (optional).
    /// wal_cb   — invoked for each WAL_REPLICATE message (optional).
    void start(uint16_t port, SqlQueryCallback   sql_cb,
               TickIngestCallback tick_cb = nullptr,
               WalReplayCallback  wal_cb  = nullptr);

    /// Graceful stop: close listen socket, join accept thread.
    /// Active connection threads are detached and finish independently.
    void stop();

    /// Set fencing token for write validation.
    /// When set, TICK_INGEST and WAL_REPLICATE with epoch < last_seen are rejected.
    /// epoch=0 in the header bypasses fencing (backward compatible).
    void set_fencing_token(FencingToken* token) { fencing_token_ = token; }

    /// Set stats callback — invoked for STATS_REQUEST messages.
    void set_stats_callback(StatsCallback cb) { stats_callback_ = std::move(cb); }

    /// Set metrics callback — invoked for METRICS_REQUEST messages.
    /// Returns JSON array of MetricsSnapshot for the given time range.
    void set_metrics_callback(MetricsCallback cb) { metrics_callback_ = std::move(cb); }

    /// Set ring update callback — invoked for RING_UPDATE messages.
    /// Returns true if the update was applied (epoch valid).
    void set_ring_update_callback(RingUpdateCallback cb) { ring_update_callback_ = std::move(cb); }

    /// Set security config for internal RPC authentication.
    /// When enabled, clients must send AUTH_HANDSHAKE before any other message.
    void set_security(RpcSecurityConfig cfg) { security_ = std::move(cfg); }

    /// Set maximum allowed payload size in bytes (default 64MB).
    /// Messages exceeding this limit are rejected and the connection is closed.
    void set_max_payload_size(uint32_t bytes) { max_payload_size_ = bytes; }
    uint32_t max_payload_size() const { return max_payload_size_; }

    /// Set maximum concurrent connections (default 1024).
    /// New connections beyond this limit are immediately closed.
    void set_max_connections(int max_conn) { max_connections_ = max_conn; }
    int  max_connections() const { return max_connections_; }

    /// Set thread pool size (default: hardware_concurrency).
    /// Must be called before start(). 0 = hardware_concurrency.
    void set_thread_pool_size(size_t n) { pool_size_ = n; }
    size_t thread_pool_size() const { return pool_size_; }

    /// Set graceful drain timeout in milliseconds (default 30000 = 30s).
    /// During stop(), in-flight requests are given this long to complete
    /// before connections are forcibly closed.
    void set_drain_timeout_ms(int ms) { drain_timeout_ms_ = ms; }
    int  drain_timeout_ms() const { return drain_timeout_ms_; }

    bool     is_running() const { return running_.load(); }
    uint16_t port()       const { return port_; }
    int      active_connections() const { return active_conns_.load(); }

private:
    void accept_loop();
    void handle_connection(int client_fd);
    void worker_loop();

    int                listen_fd_ = -1;
    uint16_t           port_      = 0;
    std::atomic<bool>  running_{false};
    std::atomic<int>   active_conns_{0};
    std::thread        accept_thread_;
    SqlQueryCallback   sql_callback_;
    TickIngestCallback tick_callback_;
    WalReplayCallback  wal_callback_;
    StatsCallback      stats_callback_;
    MetricsCallback    metrics_callback_;
    RingUpdateCallback ring_update_callback_;
    FencingToken*      fencing_token_ = nullptr;
    RpcSecurityConfig  security_;
    uint32_t           max_payload_size_ = 64u * 1024u * 1024u;  // 64MB default
    int                max_connections_ = 1024;
    size_t             pool_size_ = 0;  // 0 = hardware_concurrency
    int                drain_timeout_ms_ = 30000;  // 30s default

    // Thread pool for connection handling
    std::vector<std::thread>    workers_;
    std::mutex                  queue_mu_;
    std::condition_variable     queue_cv_;
    std::queue<int>             conn_queue_;  // pending client fds

    // Track active connection fds for clean shutdown
    std::mutex              conn_fds_mu_;
    std::vector<int>        conn_fds_;
};

// ============================================================================
// TcpRpcClient — with connection pooling
// ============================================================================
class TcpRpcClient {
public:
    /// @param connect_timeout_ms  Non-blocking connect timeout (default 2s).
    /// @param pool_size           Max idle connections to keep (default 4).
    /// @param query_timeout_ms    Per-query read timeout (default 30s, 0=no timeout).
    explicit TcpRpcClient(std::string host, uint16_t port,
                           int connect_timeout_ms = 2000,
                           size_t pool_size = 4,
                           int query_timeout_ms = 30000)
        : host_(std::move(host)), port_(port),
          connect_timeout_ms_(connect_timeout_ms),
          max_pool_size_(pool_size),
          query_timeout_ms_(query_timeout_ms) {}

    ~TcpRpcClient();

    TcpRpcClient(const TcpRpcClient&)            = delete;
    TcpRpcClient& operator=(const TcpRpcClient&) = delete;

    /// Execute SQL on the remote node (blocking, respects connect timeout).
    zeptodb::sql::QueryResultSet execute_sql(const std::string& sql);

    /// Send a TickMessage to the remote node's local pipeline.
    bool ingest_tick(const zeptodb::ingestion::TickMessage& msg);

    /// Ping the remote server.  true = reachable within connect_timeout_ms.
    bool ping();

    /// Send a batch of WAL entries to the replica node.
    bool replicate_wal(const std::vector<zeptodb::ingestion::TickMessage>& batch);

    /// Request stats + metrics history from the remote node.
    /// Returns raw JSON string, or empty string on failure.
    std::string request_stats();

    /// Request metrics history from the remote node.
    /// Returns JSON array string, or empty string on failure.
    std::string request_metrics(int64_t since_ms = 0, uint32_t limit = 0);

    const std::string& host() const { return host_; }
    uint16_t           port() const { return port_; }

    /// Set fencing epoch to include in write messages (TICK_INGEST, WAL_REPLICATE).
    void set_epoch(uint64_t e) { epoch_ = e; }
    uint64_t epoch() const { return epoch_; }

    /// Set security config for internal RPC authentication.
    void set_security(RpcSecurityConfig cfg) { security_ = std::move(cfg); }

    /// Number of idle connections currently in the pool.
    size_t pool_idle_count() const;

private:
    int connect_to_server() const;

    /// Acquire a connected fd from pool or create new.
    int acquire();
    /// Return fd to pool (or close if pool full / fd bad).
    void release(int fd, bool healthy);

    std::string host_;
    uint16_t    port_;
    int         connect_timeout_ms_;
    size_t      max_pool_size_;
    int         query_timeout_ms_;
    uint64_t    epoch_ = 0;
    RpcSecurityConfig security_;

    mutable std::mutex pool_mu_;
    std::vector<int>   pool_;  // idle fds
};

} // namespace zeptodb::cluster
