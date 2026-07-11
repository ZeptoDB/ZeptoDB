#pragma once
// ============================================================================
// Phase C-3: QueryCoordinator — scatter-gather query execution
// ============================================================================
// Routes SQL queries to the correct data node(s) and merges partial results.
//
// Tier A (direct routing): query contains WHERE symbol = X
//   → parse symbol ID → PartitionRouter.route(sym) → execute on that node
//   → no merge needed (single-node result)
//
// Tier B (scatter-gather): query has no single-symbol filter
//   → fan out to ALL registered nodes in parallel (std::async)
//   → collect QueryResultSet from each node
//   → merge via partial_agg.h
//
// Node types:
//   Local  — in-process ZeptoPipeline; SQL runs inside the coordinator process
//   Remote — TcpRpcClient; SQL sent over TCP to a TcpRpcServer
// ============================================================================

#include "zeptodb/cluster/health_monitor.h"
#include "zeptodb/cluster/partial_agg.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"
#include <atomic>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace zeptodb::cluster {

// ============================================================================
// QueryCoordinator
// ============================================================================
class QueryCoordinator {
public:
    /// Policy for the bounded coordinator-local small-table JOIN path.
    /// `Disabled` rejects simple cross-node hash JOIN candidates explicitly;
    /// `BoundedBroadcast` fetches both sides under configured row/byte/latency
    /// caps before delegating semantics to a temporary local executor.
    enum class SmallTableJoinPolicy {
        Disabled,
        BoundedBroadcast,
    };

    /// Runtime guardrails for bounded small-table JOIN materialization.
    /// Thread-safe to update through `set_small_table_join_config()`; values
    /// are snapshotted per query. `row_limit` is rows per JOIN side,
    /// `max_materialized_bytes` is an estimated byte cap across both sides,
    /// and `max_latency_us=0` disables latency rejection.
    struct SmallTableJoinConfig {
        SmallTableJoinPolicy policy = SmallTableJoinPolicy::BoundedBroadcast;
        size_t row_limit = 4096;
        size_t max_materialized_bytes = 8 * 1024 * 1024;
        uint64_t max_latency_us = 0;
    };

    /// Telemetry counters and last-attempt gauges for bounded small-table JOIN.
    /// All fields are monotonically increasing counters except `last_*`, which
    /// describe the most recent candidate attempt. Units are rows, bytes, and
    /// microseconds as named.
    struct SmallTableJoinTelemetrySnapshot {
        uint64_t candidates = 0;
        uint64_t accepted = 0;
        uint64_t rejected_row_cap = 0;
        uint64_t rejected_byte_cap = 0;
        uint64_t rejected_latency_cap = 0;
        uint64_t errors = 0;
        uint64_t rows_materialized = 0;
        uint64_t bytes_materialized = 0;
        uint64_t last_left_rows = 0;
        uint64_t last_right_rows = 0;
        uint64_t last_materialized_bytes = 0;
        uint64_t last_latency_us = 0;
    };

    /// Policy for coordinator-local full-data window materialization.
    /// `Disabled` rejects distributed queries that require all rows on the
    /// coordinator; `BoundedCoordinatorLocal` allows the path under configured
    /// row/byte/latency caps.
    enum class WindowMaterializationPolicy {
        Disabled,
        BoundedCoordinatorLocal,
    };

    /// Runtime guardrails for cluster-mode full-data window materialization.
    /// Thread-safe to update through `set_window_materialization_config()`;
    /// values are snapshotted per query. `row_limit` bounds fetched base rows,
    /// `max_materialized_bytes` is an estimated byte cap, and
    /// `max_latency_us=0` disables latency rejection.
    struct WindowMaterializationConfig {
        WindowMaterializationPolicy policy =
            WindowMaterializationPolicy::BoundedCoordinatorLocal;
        size_t row_limit = 65536;
        size_t max_materialized_bytes = 64 * 1024 * 1024;
        uint64_t max_latency_us = 0;
    };

    /// Telemetry counters and last-attempt gauges for bounded full-data
    /// materialization. All fields are monotonically increasing counters except
    /// `last_*`, which describe the most recent candidate attempt. Units are
    /// rows, bytes, and microseconds as named.
    struct WindowMaterializationTelemetrySnapshot {
        uint64_t candidates = 0;
        uint64_t accepted = 0;
        uint64_t rejected_row_cap = 0;
        uint64_t rejected_byte_cap = 0;
        uint64_t rejected_latency_cap = 0;
        uint64_t errors = 0;
        uint64_t rows_materialized = 0;
        uint64_t bytes_materialized = 0;
        uint64_t last_rows = 0;
        uint64_t last_materialized_bytes = 0;
        uint64_t last_latency_us = 0;
    };

    QueryCoordinator()  = default;
    ~QueryCoordinator() = default;

    QueryCoordinator(const QueryCoordinator&)            = delete;
    QueryCoordinator& operator=(const QueryCoordinator&) = delete;

    // ----------------------------------------------------------------
    // Shared router injection
    // ----------------------------------------------------------------

    /// Use an external PartitionRouter (owned by caller, e.g. ClusterNode).
    /// Must be called before add_remote_node / add_local_node.
    /// The caller's router_mutex must protect the shared router externally
    /// when ClusterNode and QueryCoordinator are used together.
    void set_shared_router(PartitionRouter* shared, std::shared_mutex* shared_mu) {
        external_router_ = shared;
        external_router_mu_ = shared_mu;
    }

    // ----------------------------------------------------------------
    // Node registration
    // ----------------------------------------------------------------

    /// Register a remote data node.  Queries are sent via TCP.
    void add_remote_node(NodeAddress addr);

    /// Register the local pipeline as a data node (in-process execution).
    void add_local_node(NodeAddress addr, zeptodb::core::ZeptoPipeline& pipeline);

    /// Remove a node (e.g., after failure detection).
    void remove_node(NodeId id);

    size_t node_count() const {
        std::shared_lock lock(mutex_);
        return endpoints_.size();
    }

    /// Collect stats JSON from all remote nodes (parallel fan-out).
    /// Returns a vector of JSON strings, one per remote node.
    /// Local node is NOT included — caller adds it separately.
    std::vector<std::string> collect_remote_stats() {
        std::shared_lock lock(mutex_);
        std::vector<std::shared_ptr<NodeEndpoint>> remotes;
        for (auto& ep : endpoints_) {
            if (!ep->is_local && ep->rpc) remotes.push_back(ep);
        }
        lock.unlock();

        // Fan out in parallel
        std::vector<std::future<std::string>> futures;
        futures.reserve(remotes.size());
        for (auto& ep : remotes) {
            futures.push_back(std::async(std::launch::async, [&ep]() {
                return ep->rpc->request_stats();
            }));
        }

        std::vector<std::string> results;
        results.reserve(futures.size());
        for (auto& f : futures) {
            auto json = f.get();
            if (!json.empty()) results.push_back(json);
        }
        return results;
    }

    /// Collect metrics history from all remote nodes (parallel fan-out).
    /// Returns a vector of JSON array strings, one per remote node.
    std::vector<std::string> collect_remote_metrics(int64_t since_ms = 0,
                                                     uint32_t limit = 0) {
        std::shared_lock lock(mutex_);
        std::vector<std::shared_ptr<NodeEndpoint>> remotes;
        for (auto& ep : endpoints_) {
            if (!ep->is_local && ep->rpc) remotes.push_back(ep);
        }
        lock.unlock();

        std::vector<std::future<std::string>> futures;
        futures.reserve(remotes.size());
        for (auto& ep : remotes) {
            futures.push_back(std::async(std::launch::async,
                [&ep, since_ms, limit]() {
                    return ep->rpc->request_metrics(since_ms, limit);
                }));
        }

        std::vector<std::string> results;
        results.reserve(futures.size());
        for (auto& f : futures) {
            auto json = f.get();
            if (!json.empty()) results.push_back(json);
        }
        return results;
    }

    /// Wire this coordinator into a HealthMonitor so that DEAD nodes are
    /// automatically removed from the routing table without manual calls.
    void connect_health_monitor(HealthMonitor& hm);

    // ----------------------------------------------------------------
    // Query execution
    // ----------------------------------------------------------------

    /// Execute SQL.  Automatically selects Tier A or Tier B routing.
    zeptodb::sql::QueryResultSet execute_sql(const std::string& sql);

    /// Force Tier A: execute on the node that owns symbol_id.
    zeptodb::sql::QueryResultSet execute_sql_for_symbol(const std::string& sql,
                                                     SymbolId           symbol_id);

    /// Fire-and-forget: forward a DDL SQL string to every *remote* node's
    /// TcpRpcClient so all pods converge on the same schema (devlog 112).
    /// Called by HttpServer after a successful *local* DDL execution.
    /// Local endpoints and endpoints without an rpc client are skipped.
    /// Per-node failures are logged as warnings and never thrown.
    void forward_ddl_to_remotes(const std::string& sql);

    /// Set table-level placement for a declared table. This is a cold-path
    /// operational control used by symbol-less Action-Outcome/control tables
    /// so placement is explicit instead of an accidental stable table-id hash.
    bool set_table_placement(const std::string& table_name,
                             TablePlacementPolicy policy,
                             NodeId node_id = INVALID_NODE_ID,
                             std::string* error = nullptr);

    /// Remove a table-level placement override for a declared table.
    bool clear_table_placement(const std::string& table_name,
                               std::string* error = nullptr);

    /// Re-apply persisted table placement metadata from local schema catalogs.
    /// Intended for restart/reload paths after schemas have been restored.
    bool apply_catalog_table_placements(std::string* error = nullptr);

    /// Snapshot bounded small-table JOIN telemetry.
    [[nodiscard]] SmallTableJoinTelemetrySnapshot small_table_join_stats() const;

    /// Reset bounded small-table JOIN telemetry. Intended for tests and
    /// benchmark/replay harnesses that measure deltas explicitly.
    void reset_small_table_join_stats();

    /// Configure the bounded coordinator-local JOIN path. This is an
    /// explicit feature policy, not a general optimizer rule.
    bool set_small_table_join_config(SmallTableJoinConfig config,
                                     std::string* error = nullptr);
    [[nodiscard]] SmallTableJoinConfig small_table_join_config() const;

    /// Snapshot bounded full-data window materialization telemetry.
    [[nodiscard]] WindowMaterializationTelemetrySnapshot
    window_materialization_stats() const;

    /// Reset bounded full-data window materialization telemetry. Intended for
    /// focused tests and replay harnesses that measure deltas explicitly.
    void reset_window_materialization_stats();

    /// Configure the bounded coordinator-local window/full-data path. This
    /// path fails closed when disabled or when a cap is exceeded; it does not
    /// fall back to partial scatter semantics.
    bool set_window_materialization_config(WindowMaterializationConfig config,
                                           std::string* error = nullptr);
    [[nodiscard]] WindowMaterializationConfig
    window_materialization_config() const;

    // ----------------------------------------------------------------
    // Router access (internal or shared)
    // ----------------------------------------------------------------

    PartitionRouter&       router()       { return external_router_ ? *external_router_ : own_router_; }
    const PartitionRouter& router() const { return external_router_ ? *external_router_ : own_router_; }

    /// Access the router mutex directly (external if shared, else internal).
    /// Used by CoordinatorRoutingAdapter (devlog 111) to share the same
    /// reader/writer lock the coordinator itself uses.
    std::shared_mutex& router_mutex() const {
        return external_router_mu_ ? *external_router_mu_ : router_mu_;
    }

    /// Lock the router for reading (uses external mutex if shared, else internal).
    std::shared_lock<std::shared_mutex> router_read_lock() const {
        return std::shared_lock<std::shared_mutex>(
            external_router_mu_ ? *external_router_mu_ : router_mu_);
    }

    /// Lock the router for writing.
    std::unique_lock<std::shared_mutex> router_write_lock() {
        return std::unique_lock<std::shared_mutex>(
            external_router_mu_ ? *external_router_mu_ : router_mu_);
    }

private:
    // ----------------------------------------------------------------
    // Internal
    // ----------------------------------------------------------------

    struct NodeEndpoint {
        NodeAddress                   addr;
        std::unique_ptr<TcpRpcClient> rpc;       // null for local node
        zeptodb::core::ZeptoPipeline*     pipeline;  // non-null for local node
        bool                          is_local;
    };

    zeptodb::sql::QueryResultSet exec_on(NodeEndpoint& ep, const std::string& sql);

    std::vector<zeptodb::sql::QueryResultSet> scatter(const std::string& sql);
    std::vector<zeptodb::sql::QueryResultSet> scatter_to(
        const std::string& sql, const std::unordered_set<size_t>& node_indices);

    /// Execute a bounded coordinator-local hash JOIN by fetching both small
    /// operational tables, materializing them into a temporary typed pipeline,
    /// and delegating the actual JOIN semantics to the SQL executor.
    zeptodb::sql::QueryResultSet execute_small_table_hash_join(
        const std::string& sql,
        const zeptodb::sql::SelectStmt& stmt);

    /// Try to extract a single symbol ID from "WHERE symbol = <literal>".
    std::optional<SymbolId> extract_symbol_filter(const std::string& sql) const;

    /// Try to extract the primary table name for table-aware routing.
    std::optional<std::string> extract_table_name(const std::string& sql) const;

    /// Resolve a table id for routing when a local schema snapshot knows the
    /// table. Returns 0 for legacy/no-CREATE-table paths.
    uint16_t resolve_routing_table_id_locked(
        const std::optional<std::string>& table_name) const;

    /// Snapshot a local schema registry entry while mutex_ is already held.
    std::optional<zeptodb::storage::TableSchema> schema_snapshot_locked(
        const std::string& table_name) const;

    bool apply_catalog_table_placements_locked(std::string* error);
    bool apply_schema_placement_locked(
        const zeptodb::storage::TableSchema& schema,
        std::string* error);

    struct SmallTableJoinTelemetry {
        std::atomic<uint64_t> candidates{0};
        std::atomic<uint64_t> accepted{0};
        std::atomic<uint64_t> rejected_row_cap{0};
        std::atomic<uint64_t> rejected_byte_cap{0};
        std::atomic<uint64_t> rejected_latency_cap{0};
        std::atomic<uint64_t> errors{0};
        std::atomic<uint64_t> rows_materialized{0};
        std::atomic<uint64_t> bytes_materialized{0};
        std::atomic<uint64_t> last_left_rows{0};
        std::atomic<uint64_t> last_right_rows{0};
        std::atomic<uint64_t> last_materialized_bytes{0};
        std::atomic<uint64_t> last_latency_us{0};
    };

    struct WindowMaterializationTelemetry {
        std::atomic<uint64_t> candidates{0};
        std::atomic<uint64_t> accepted{0};
        std::atomic<uint64_t> rejected_row_cap{0};
        std::atomic<uint64_t> rejected_byte_cap{0};
        std::atomic<uint64_t> rejected_latency_cap{0};
        std::atomic<uint64_t> errors{0};
        std::atomic<uint64_t> rows_materialized{0};
        std::atomic<uint64_t> bytes_materialized{0};
        std::atomic<uint64_t> last_rows{0};
        std::atomic<uint64_t> last_materialized_bytes{0};
        std::atomic<uint64_t> last_latency_us{0};
    };

    mutable std::shared_mutex                        mutex_;
    std::vector<std::shared_ptr<NodeEndpoint>>       endpoints_;

    // Router: either own (standalone) or external (shared with ClusterNode)
    PartitionRouter                                  own_router_;
    mutable std::shared_mutex                        router_mu_;
    PartitionRouter*                                 external_router_ = nullptr;
    std::shared_mutex*                               external_router_mu_ = nullptr;
    SmallTableJoinTelemetry                          small_table_join_stats_;
    SmallTableJoinConfig                             small_table_join_config_;
    WindowMaterializationTelemetry                   window_materialization_stats_;
    WindowMaterializationConfig                      window_materialization_config_;
};

} // namespace zeptodb::cluster
