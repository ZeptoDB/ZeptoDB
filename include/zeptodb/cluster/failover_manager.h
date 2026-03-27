#pragma once
// ============================================================================
// FailoverManager — automatic failover + data recovery on node death
// ============================================================================
// Connects HealthMonitor + PartitionRouter + QueryCoordinator:
//   1. DEAD detected → remove node from router & coordinator
//   2. Consistent hash ring auto-promotes replica to primary
//   3. WAL-based re-replication: copy data from promoted primary to new replica
//   4. Fires on_failover callback for additional caller actions
//
// Re-replication is a mandatory failover step (not optional callback).
// ============================================================================

#include "zeptodb/cluster/health_monitor.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/partition_migrator.h"
#include "zeptodb/cluster/query_coordinator.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace zeptodb::cluster {

struct FailoverEvent {
    NodeId dead_node;
    size_t affected_vnodes;

    struct ReReplicationTarget {
        NodeId new_primary;   // was replica, now promoted
        NodeId new_replica;   // next node on ring — needs data copy
    };
    std::vector<ReReplicationTarget> re_replication;

    // Recovery results
    size_t re_replication_attempted = 0;
    size_t re_replication_succeeded = 0;
};

struct FailoverConfig {
    bool   auto_re_replicate = true;   // true = failover 시 자동 re-replication
    bool   async_re_replicate = true;  // true = 백그라운드, false = blocking
};

using FailoverCallback = std::function<void(const FailoverEvent&)>;

class FailoverManager {
public:
    FailoverManager(PartitionRouter& router, QueryCoordinator& coordinator,
                    FailoverConfig cfg = {})
        : router_(router), coordinator_(coordinator), config_(cfg) {}

    ~FailoverManager() {
        // Join any outstanding async re-replication threads
        std::lock_guard<std::mutex> lock(async_mu_);
        for (auto& t : async_threads_) {
            if (t.joinable()) t.join();
        }
    }

    /// Connect to HealthMonitor. Failover triggers automatically on DEAD.
    void connect(HealthMonitor& hm);

    /// Register callback fired after failover completes.
    void on_failover(FailoverCallback cb) { callbacks_.push_back(std::move(cb)); }

    /// Register a node endpoint for re-replication (host:port for RPC).
    /// Must be called for each node so PartitionMigrator can reach them.
    void register_node(NodeId id, const std::string& host, uint16_t port);

    /// Manual failover trigger (for testing or admin-initiated).
    FailoverEvent trigger_failover(NodeId dead_node);

    size_t failover_count() const { return failover_count_.load(); }
    size_t re_replication_count() const { return re_replication_count_.load(); }

private:
    void run_re_replication(FailoverEvent& event);

    PartitionRouter&    router_;
    QueryCoordinator&   coordinator_;
    FailoverConfig      config_;
    PartitionMigrator   migrator_;
    std::vector<FailoverCallback> callbacks_;
    std::atomic<size_t> failover_count_{0};
    std::atomic<size_t> re_replication_count_{0};

    // Async re-replication threads
    std::mutex async_mu_;
    std::vector<std::thread> async_threads_;
};

} // namespace zeptodb::cluster
