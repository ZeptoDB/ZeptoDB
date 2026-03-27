#include "zeptodb/cluster/failover_manager.h"
#include "zeptodb/common/logger.h"
#include <set>

namespace zeptodb::cluster {

void FailoverManager::connect(HealthMonitor& hm) {
    hm.on_state_change([this](NodeId id, NodeState /*old_s*/, NodeState new_s) {
        if (new_s == NodeState::DEAD) {
            trigger_failover(id);
        }
    });
}

void FailoverManager::register_node(NodeId id, const std::string& host,
                                     uint16_t port) {
    migrator_.add_node(id, host, port);
}

FailoverEvent FailoverManager::trigger_failover(NodeId dead_node) {
    ZEPTO_INFO("FailoverManager: node {} declared DEAD, starting failover",
               dead_node);

    // Before removing: plan what moves are needed
    auto plan = router_.plan_remove(dead_node);

    // Remove from router and coordinator
    router_.remove_node(dead_node);
    coordinator_.remove_node(dead_node);

    FailoverEvent event;
    event.dead_node = dead_node;
    event.affected_vnodes = plan.total_moves();

    // Compute re-replication targets
    if (router_.node_count() >= 2) {
        std::set<NodeId> seen;
        for (auto& m : plan.moves) {
            if (m.from == dead_node && !seen.count(m.to)) {
                seen.insert(m.to);
                NodeId new_replica = router_.route_replica(m.symbol);
                if (new_replica != INVALID_NODE_ID && new_replica != m.to) {
                    event.re_replication.push_back({m.to, new_replica});
                }
            }
        }
    }

    failover_count_.fetch_add(1);

    // Auto re-replication: mandatory step (only if migrator has registered nodes)
    bool can_re_replicate = config_.auto_re_replicate &&
                            !event.re_replication.empty();
    if (can_re_replicate) {
        // Check that migrator knows about the involved nodes
        bool nodes_known = true;
        for (auto& t : event.re_replication) {
            if (!migrator_.has_node(t.new_primary) ||
                !migrator_.has_node(t.new_replica)) {
                nodes_known = false;
                break;
            }
        }
        can_re_replicate = nodes_known;
    }

    if (can_re_replicate) {
        if (config_.async_re_replicate) {
            auto event_copy = event;
            std::lock_guard<std::mutex> lock(async_mu_);
            async_threads_.emplace_back([this, ev = std::move(event_copy)]() mutable {
                run_re_replication(ev);
                for (auto& cb : callbacks_) cb(ev);
            });
        } else {
            run_re_replication(event);
            for (auto& cb : callbacks_) cb(event);
        }
    } else {
        for (auto& cb : callbacks_) cb(event);
    }

    return event;
}

void FailoverManager::run_re_replication(FailoverEvent& event) {
    ZEPTO_INFO("FailoverManager: starting re-replication for {} targets",
               event.re_replication.size());

    for (auto& target : event.re_replication) {
        event.re_replication_attempted++;

        // new_primary has the data (was replica), copy to new_replica
        // Use a representative symbol to find what data to migrate
        // In practice, we migrate all symbols that new_primary now owns
        // For each symbol routed to new_primary, replicate to new_replica
        uint64_t rows_moved = 0;
        bool ok = migrator_.migrate_symbol(
            0,  // representative symbol — migrator queries by node
            target.new_primary,
            target.new_replica,
            rows_moved);

        if (ok) {
            event.re_replication_succeeded++;
            re_replication_count_.fetch_add(1);
            ZEPTO_INFO("FailoverManager: re-replicated node {} → node {}",
                       target.new_primary, target.new_replica);
        } else {
            ZEPTO_WARN("FailoverManager: re-replication failed node {} → node {}",
                       target.new_primary, target.new_replica);
        }
    }

    ZEPTO_INFO("FailoverManager: re-replication complete ({}/{} succeeded)",
               event.re_replication_succeeded, event.re_replication_attempted);
}

} // namespace zeptodb::cluster
