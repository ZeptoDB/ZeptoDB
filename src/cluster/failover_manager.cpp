#include "zeptodb/cluster/failover_manager.h"
#include <set>

namespace zeptodb::cluster {

void FailoverManager::connect(HealthMonitor& hm) {
    hm.on_state_change([this](NodeId id, NodeState /*old_s*/, NodeState new_s) {
        if (new_s == NodeState::DEAD) {
            trigger_failover(id);
        }
    });
}

FailoverEvent FailoverManager::trigger_failover(NodeId dead_node) {
    // Before removing: find symbols whose primary was dead_node
    // After removal, their new primary = old replica, and we need a new replica
    auto plan = router_.plan_remove(dead_node);

    // Remove from router (ring rehash) and coordinator (endpoint list)
    router_.remove_node(dead_node);
    coordinator_.remove_node(dead_node);

    FailoverEvent event;
    event.dead_node = dead_node;
    event.affected_vnodes = plan.total_moves();

    // Compute re-replication targets:
    // For each move (from=dead_node, to=new_primary), find new_primary's replica
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

    for (auto& cb : callbacks_)
        cb(event);

    return event;
}

} // namespace zeptodb::cluster
