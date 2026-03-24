#include "zeptodb/cluster/snapshot_coordinator.h"
#include <chrono>
#include <future>

namespace zeptodb::cluster {

void SnapshotCoordinator::add_node(NodeId id, const std::string& host,
                                    uint16_t port) {
    NodeEntry e;
    e.id  = id;
    e.rpc = std::make_unique<TcpRpcClient>(host, port);
    nodes_.push_back(std::move(e));
}

SnapshotResult SnapshotCoordinator::take_snapshot() {
    SnapshotResult result;
    result.snapshot_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Fan out SNAPSHOT command to all nodes in parallel
    std::vector<std::future<SnapshotNodeResult>> futures;
    for (auto& node : nodes_) {
        futures.push_back(std::async(std::launch::async,
            [&node]() -> SnapshotNodeResult {
                SnapshotNodeResult nr;
                nr.node_id = node.id;
                auto qr = node.rpc->execute_sql("SNAPSHOT");
                if (qr.ok()) {
                    nr.success = true;
                    nr.rows_flushed = (!qr.rows.empty() && !qr.rows[0].empty())
                                    ? qr.rows[0][0] : 0;
                } else {
                    nr.success = false;
                    nr.error = qr.error;
                    nr.rows_flushed = 0;
                }
                return nr;
            }));
    }

    for (auto& f : futures)
        result.nodes.push_back(f.get());

    return result;
}

} // namespace zeptodb::cluster
