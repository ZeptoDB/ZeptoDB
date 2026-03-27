#include "zeptodb/cluster/snapshot_coordinator.h"
#include "zeptodb/common/logger.h"
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

uint64_t SnapshotCoordinator::next_snapshot_id() {
    return ++snapshot_seq_;
}

void SnapshotCoordinator::send_abort(uint64_t snapshot_id) {
    std::string cmd = "SNAPSHOT ABORT " + std::to_string(snapshot_id);
    std::vector<std::future<void>> futs;
    for (auto& node : nodes_) {
        futs.push_back(std::async(std::launch::async,
            [&node, &cmd]() { node.rpc->execute_sql(cmd); }));
    }
    for (auto& f : futs) {
        try { f.get(); } catch (...) {}
    }
}

SnapshotResult SnapshotCoordinator::take_snapshot() {
    SnapshotResult result;
    uint64_t sid = next_snapshot_id();
    result.snapshot_id = sid;
    result.snapshot_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // ================================================================
    // Phase 1: PREPARE — pause ingest on all nodes
    // ================================================================
    std::string prepare_cmd = "SNAPSHOT PREPARE " + std::to_string(sid);

    std::vector<std::future<SnapshotNodeResult>> prep_futures;
    for (auto& node : nodes_) {
        prep_futures.push_back(std::async(std::launch::async,
            [&node, &prepare_cmd]() -> SnapshotNodeResult {
                SnapshotNodeResult nr;
                nr.node_id = node.id;
                nr.rows_flushed = 0;
                auto qr = node.rpc->execute_sql(prepare_cmd);
                nr.success = qr.ok();
                if (!nr.success) nr.error = qr.error;
                return nr;
            }));
    }

    bool all_prepared = true;
    std::vector<SnapshotNodeResult> prep_results;
    for (auto& f : prep_futures) {
        auto nr = f.get();
        if (!nr.success) {
            ZEPTO_WARN("SnapshotCoordinator: PREPARE failed on node {} — {}",
                       nr.node_id, nr.error);
            all_prepared = false;
        }
        prep_results.push_back(std::move(nr));
    }

    // ================================================================
    // ABORT if any node failed prepare
    // ================================================================
    if (!all_prepared) {
        ZEPTO_WARN("SnapshotCoordinator: aborting snapshot {}", sid);
        send_abort(sid);
        result.nodes = std::move(prep_results);
        return result;
    }

    // ================================================================
    // Phase 2: COMMIT — flush at consistent point
    // ================================================================
    std::string commit_cmd = "SNAPSHOT COMMIT " + std::to_string(sid);

    std::vector<std::future<SnapshotNodeResult>> commit_futures;
    for (auto& node : nodes_) {
        commit_futures.push_back(std::async(std::launch::async,
            [&node, &commit_cmd]() -> SnapshotNodeResult {
                SnapshotNodeResult nr;
                nr.node_id = node.id;
                auto qr = node.rpc->execute_sql(commit_cmd);
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

    for (auto& f : commit_futures)
        result.nodes.push_back(f.get());

    return result;
}

SnapshotResult SnapshotCoordinator::take_snapshot_legacy() {
    SnapshotResult result;
    result.snapshot_id = next_snapshot_id();
    result.snapshot_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

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
