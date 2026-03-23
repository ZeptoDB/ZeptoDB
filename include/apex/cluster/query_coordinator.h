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
//   Local  — in-process ApexPipeline; SQL runs inside the coordinator process
//   Remote — TcpRpcClient; SQL sent over TCP to a TcpRpcServer
// ============================================================================

#include "apex/cluster/health_monitor.h"
#include "apex/cluster/partial_agg.h"
#include "apex/cluster/partition_router.h"
#include "apex/cluster/tcp_rpc.h"
#include "apex/core/pipeline.h"
#include "apex/sql/executor.h"
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace apex::cluster {

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
    void add_local_node(NodeAddress addr, apex::core::ApexPipeline& pipeline);

    /// Remove a node (e.g., after failure detection).
    void remove_node(NodeId id);

    size_t node_count() const {
        std::shared_lock lock(mutex_);
        return endpoints_.size();
    }

    /// Wire this coordinator into a HealthMonitor so that DEAD nodes are
    /// automatically removed from the routing table without manual calls.
    void connect_health_monitor(HealthMonitor& hm);

    // ----------------------------------------------------------------
    // Query execution
    // ----------------------------------------------------------------

    /// Execute SQL.  Automatically selects Tier A or Tier B routing.
    apex::sql::QueryResultSet execute_sql(const std::string& sql);

    /// Force Tier A: execute on the node that owns symbol_id.
    apex::sql::QueryResultSet execute_sql_for_symbol(const std::string& sql,
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
        apex::core::ApexPipeline*     pipeline;  // non-null for local node
        bool                          is_local;
    };

    apex::sql::QueryResultSet exec_on(NodeEndpoint& ep, const std::string& sql);

    std::vector<apex::sql::QueryResultSet> scatter(const std::string& sql);

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

} // namespace apex::cluster
