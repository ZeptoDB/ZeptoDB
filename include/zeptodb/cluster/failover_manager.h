#pragma once
// ============================================================================
// FailoverManager — automatic failover on node death
// ============================================================================
// Connects HealthMonitor + PartitionRouter + QueryCoordinator:
//   1. DEAD detected → remove node from router & coordinator
//   2. Consistent hash ring auto-promotes replica to primary
//   3. Fires on_failover callback so the caller can start new replication
//
// The caller (e.g. ClusterNode) is responsible for:
//   - Setting up WalReplicator to the new replica after failover
//   - Actual data re-replication if needed
// ============================================================================

#include "zeptodb/cluster/health_monitor.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/query_coordinator.h"

#include <atomic>
#include <functional>
#include <vector>

namespace zeptodb::cluster {

struct FailoverEvent {
    NodeId dead_node;
    size_t affected_vnodes;

    // Re-replication targets: for each promoted primary, the new replica node
    // that should receive a copy of the data. Caller uses this to start
    // PartitionMigrator or WalReplicator to the new replica.
    struct ReReplicationTarget {
        NodeId new_primary;   // was replica, now promoted
        NodeId new_replica;   // next node on ring — needs data copy
    };
    std::vector<ReReplicationTarget> re_replication;
};

using FailoverCallback = std::function<void(const FailoverEvent&)>;

class FailoverManager {
public:
    FailoverManager(PartitionRouter& router, QueryCoordinator& coordinator)
        : router_(router), coordinator_(coordinator) {}

    /// Connect to HealthMonitor. Failover triggers automatically on DEAD.
    void connect(HealthMonitor& hm);

    /// Register callback fired after failover completes.
    void on_failover(FailoverCallback cb) { callbacks_.push_back(std::move(cb)); }

    /// Manual failover trigger (for testing or admin-initiated).
    FailoverEvent trigger_failover(NodeId dead_node);

    size_t failover_count() const { return failover_count_.load(); }

private:
    PartitionRouter&    router_;
    QueryCoordinator&   coordinator_;
    std::vector<FailoverCallback> callbacks_;
    std::atomic<size_t> failover_count_{0};
};

} // namespace zeptodb::cluster
