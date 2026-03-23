#pragma once
// ============================================================================
// ComputeNode — stateless query executor that reads from remote Data Nodes
// ============================================================================
// Separates compute from storage:
//   - Data Nodes: own partitions, serve raw data via SQL RPC
//   - Compute Node: fetches data from Data Nodes, executes JOINs/aggregations locally
//
// Zero impact on Data Node CPU for heavy compute (JOIN, window functions).
// Data transfer via TcpRpc (future: RDMA remote_read for zero-copy).
// ============================================================================

#include "apex/cluster/query_coordinator.h"
#include "apex/cluster/tcp_rpc.h"
#include "apex/cluster/transport.h"
#include "apex/core/pipeline.h"
#include "apex/sql/executor.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace apex::cluster {

class ComputeNode {
public:
    ComputeNode() = default;

    /// Register a data node endpoint.
    void add_data_node(NodeId id, const std::string& host, uint16_t port);

    /// Execute SQL that may span multiple data nodes.
    /// Uses QueryCoordinator's merge logic (partial agg, GROUP BY merge, etc.)
    apex::sql::QueryResultSet execute(const std::string& sql);

    /// Execute a query against a specific data node (direct RPC, no merge).
    apex::sql::QueryResultSet execute_on(NodeId id, const std::string& sql);

    /// Fetch data from a data node and ingest into local pipeline for local JOIN.
    size_t fetch_and_ingest(NodeId source, const std::string& sql,
                            apex::core::ApexPipeline& local);

    size_t data_node_count() const { return nodes_.size(); }

private:
    std::unordered_map<NodeId, std::unique_ptr<TcpRpcClient>> nodes_;
    QueryCoordinator coordinator_;
};

} // namespace apex::cluster
