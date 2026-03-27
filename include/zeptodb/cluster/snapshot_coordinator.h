#pragma once
// ============================================================================
// SnapshotCoordinator — consistent distributed snapshot (2PC)
// ============================================================================
// Two-phase commit for consistent point-in-time snapshots:
//   Phase 1 (PREPARE): all nodes pause ingest + report LSN
//   Phase 2 (COMMIT):  all nodes flush at agreed LSN cutoff
//   ABORT:             if any node fails prepare, all resume ingest
//
// SQL commands sent via TcpRpcClient:
//   "SNAPSHOT PREPARE <snapshot_id>"  → node pauses ingest, returns LSN
//   "SNAPSHOT COMMIT <snapshot_id>"   → node flushes + resumes ingest
//   "SNAPSHOT ABORT <snapshot_id>"    → node resumes ingest (no flush)
//   "SNAPSHOT"                        → legacy single-phase (backward compat)
// ============================================================================

#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/cluster/transport.h"
#include "zeptodb/sql/executor.h"

#include <chrono>
#include <memory>
#include <string>
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
    uint64_t    snapshot_id;  // unique snapshot identifier
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

    /// Two-phase consistent snapshot across all nodes.
    /// Phase 1: PREPARE (pause ingest on all nodes)
    /// Phase 2: COMMIT (flush) or ABORT (resume without flush)
    SnapshotResult take_snapshot();

    /// Legacy single-phase snapshot (backward compatible, no consistency guarantee).
    SnapshotResult take_snapshot_legacy();

    /// Set prepare timeout in milliseconds (default 10s).
    /// If any node doesn't respond within this time, the snapshot is aborted.
    void set_prepare_timeout_ms(int ms) { prepare_timeout_ms_ = ms; }

    size_t node_count() const { return nodes_.size(); }

private:
    struct NodeEntry {
        NodeId id;
        std::unique_ptr<TcpRpcClient> rpc;
    };

    uint64_t next_snapshot_id();
    void send_abort(uint64_t snapshot_id);

    std::vector<NodeEntry> nodes_;
    int prepare_timeout_ms_ = 10000;
    uint64_t snapshot_seq_ = 0;
};

} // namespace zeptodb::cluster
