#pragma once
// ============================================================================
// SnapshotCoordinator — consistent distributed snapshot
// ============================================================================
// Triggers a coordinated flush across all cluster nodes:
//   1. Send SNAPSHOT command to each node (via SQL RPC)
//   2. Each node seals active partitions and flushes to HDB
//   3. Collect ack from all nodes
//   4. Return snapshot metadata (timestamp, node results)
//
// Uses existing TcpRpcClient — sends a special SQL command that the
// server-side callback can intercept.
// ============================================================================

#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/cluster/transport.h"
#include "zeptodb/sql/executor.h"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeptodb::cluster {

struct SnapshotNodeResult {
    NodeId      node_id;
    bool        success;
    std::string error;
    int64_t     rows_flushed;
};

struct SnapshotResult {
    int64_t     snapshot_ts;  // nanosecond timestamp of snapshot
    std::vector<SnapshotNodeResult> nodes;

    bool all_ok() const {
        for (auto& n : nodes)
            if (!n.success) return false;
        return !nodes.empty();
    }

    size_t total_rows() const {
        size_t t = 0;
        for (auto& n : nodes) t += n.rows_flushed;
        return t;
    }
};

class SnapshotCoordinator {
public:
    SnapshotCoordinator() = default;

    void add_node(NodeId id, const std::string& host, uint16_t port);

    /// Trigger a coordinated snapshot across all nodes.
    /// Sends "SNAPSHOT" SQL command to each node.
    /// The node's SQL callback should handle this command.
    SnapshotResult take_snapshot();

    size_t node_count() const { return nodes_.size(); }

private:
    struct NodeEntry {
        NodeId id;
        std::unique_ptr<TcpRpcClient> rpc;
    };
    std::vector<NodeEntry> nodes_;
};

} // namespace zeptodb::cluster
