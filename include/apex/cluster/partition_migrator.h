#pragma once
// ============================================================================
// Partition Migrator — execute MigrationPlan moves between nodes
// ============================================================================
// For each Move(symbol, from_node, to_node):
//   1. Query source node: SELECT * FROM trades WHERE symbol = X
//   2. Send rows as WAL batch to destination node
//   3. Update routing table
//
// Uses existing TcpRpc infrastructure — no new wire protocol needed.
// ============================================================================

#include "apex/cluster/partition_router.h"
#include "apex/cluster/tcp_rpc.h"
#include "apex/ingestion/tick_plant.h"

#include <atomic>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace apex::cluster {

struct MigrationStats {
    std::atomic<uint64_t> moves_completed{0};
    std::atomic<uint64_t> moves_failed{0};
    std::atomic<uint64_t> rows_migrated{0};
};

class PartitionMigrator {
public:
    PartitionMigrator() = default;

    /// Register a node endpoint for migration.
    /// The RPC client is used to query source and send WAL to destination.
    void add_node(NodeId id, const std::string& host, uint16_t port);

    /// Execute all moves in a migration plan.
    /// Returns number of successful moves.
    size_t execute_plan(const PartitionRouter::MigrationPlan& plan,
                        PartitionRouter& router);

    /// Execute a single symbol migration: from_node → to_node.
    /// Queries source for all rows of the symbol, sends as WAL batch to dest.
    bool migrate_symbol(SymbolId symbol, NodeId from, NodeId to);

    const MigrationStats& stats() const { return stats_; }

private:
    std::unordered_map<NodeId, std::unique_ptr<TcpRpcClient>> nodes_;
    MigrationStats stats_;
};

} // namespace apex::cluster
