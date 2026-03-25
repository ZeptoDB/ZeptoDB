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

    // ----------------------------------------------------------------
    // Router access (internal or shared)
    // ----------------------------------------------------------------

    PartitionRouter&       router()       { return external_router_ ? *external_router_ : own_router_; }
    const PartitionRouter& router() const { return external_router_ ? *external_router_ : own_router_; }

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

    /// Try to extract a single symbol ID from "WHERE symbol = <literal>".
    std::optional<SymbolId> extract_symbol_filter(const std::string& sql) const;

    mutable std::shared_mutex                        mutex_;
    std::vector<std::shared_ptr<NodeEndpoint>>       endpoints_;

    // Router: either own (standalone) or external (shared with ClusterNode)
    PartitionRouter                                  own_router_;
    mutable std::shared_mutex                        router_mu_;
    PartitionRouter*                                 external_router_ = nullptr;
    std::shared_mutex*                               external_router_mu_ = nullptr;
};

} // namespace zeptodb::cluster
