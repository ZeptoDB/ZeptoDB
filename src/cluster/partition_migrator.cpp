#include "zeptodb/cluster/partition_migrator.h"
#include "zeptodb/common/logger.h"

#include <future>

namespace zeptodb::cluster {

void PartitionMigrator::add_node(NodeId id, const std::string& host,
                                  uint16_t port) {
    nodes_[id] = std::make_unique<TcpRpcClient>(host, port);
}

bool PartitionMigrator::migrate_symbol(SymbolId symbol, NodeId from,
                                        NodeId to, uint64_t& rows_out) {
    rows_out = 0;
    auto src_it = nodes_.find(from);
    auto dst_it = nodes_.find(to);
    if (src_it == nodes_.end() || dst_it == nodes_.end()) return false;

    std::string sql = "SELECT * FROM trades WHERE symbol = "
                    + std::to_string(symbol);
    auto result = src_it->second->execute_sql(sql);
    if (!result.ok()) { stats_.moves_failed.fetch_add(1); return false; }
    if (result.rows.empty()) {
        rows_out = 0;
        stats_.moves_completed.fetch_add(1);
        return true;
    }

    int col_price = -1, col_volume = -1, col_ts = -1;
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        if (result.column_names[i] == "price")     col_price  = static_cast<int>(i);
        if (result.column_names[i] == "volume")    col_volume = static_cast<int>(i);
        if (result.column_names[i] == "timestamp") col_ts     = static_cast<int>(i);
    }

    std::vector<zeptodb::ingestion::TickMessage> batch;
    batch.reserve(result.rows.size());
    for (auto& row : result.rows) {
        zeptodb::ingestion::TickMessage msg{};
        msg.symbol_id = static_cast<uint32_t>(symbol);
        if (col_price >= 0)  msg.price   = row[col_price];
        if (col_volume >= 0) msg.volume  = row[col_volume];
        if (col_ts >= 0)     msg.recv_ts = row[col_ts];
        batch.push_back(msg);
    }

    bool ok = dst_it->second->replicate_wal(batch);
    if (ok) {
        rows_out = batch.size();
        stats_.moves_completed.fetch_add(1);
        stats_.rows_migrated.fetch_add(batch.size());
        // Throttle: estimate ~64 bytes per tick (3 int64 fields + overhead)
        if (throttler_) throttler_->record(batch.size() * 64);
    } else {
        stats_.moves_failed.fetch_add(1);
    }
    return ok;
}

void PartitionMigrator::execute_move(MoveStatus& ms, PartitionRouter& router) {
    ms.attempts++;

    // PENDING → DUAL_WRITE
    ms.state = MoveState::DUAL_WRITE;
    router.begin_migration(ms.move.symbol, ms.move.from, ms.move.to);

    // DUAL_WRITE → COPYING (with timeout enforcement)
    ms.state = MoveState::COPYING;
    uint64_t rows = 0;
    bool ok = false;

    if (move_timeout_sec_ > 0) {
        auto fut = std::async(std::launch::async, [&]() {
            return migrate_symbol(ms.move.symbol, ms.move.from, ms.move.to, rows);
        });
        auto status = fut.wait_for(std::chrono::seconds(move_timeout_sec_));
        if (status == std::future_status::ready) {
            ok = fut.get();
        } else {
            // Timeout — treat as failure
            ms.state = MoveState::FAILED;
            ms.error = "move timed out after " + std::to_string(move_timeout_sec_) + "s";
            router.end_migration(ms.move.symbol);
            ZEPTO_WARN("PartitionMigrator: move symbol={} {}→{} timed out ({}s)",
                       ms.move.symbol, ms.move.from, ms.move.to, move_timeout_sec_);
            return;
        }
    } else {
        ok = migrate_symbol(ms.move.symbol, ms.move.from, ms.move.to, rows);
    }

    if (ok) {
        ms.state = MoveState::COMMITTED;
        ms.rows_migrated = rows;
        ms.error.clear();
        router.end_migration(ms.move.symbol);
    } else {
        // Rollback: delete partial data from dest
        rollback_move(ms.move.symbol, ms.move.to);
        ms.state = MoveState::FAILED;
        ms.error = "migrate_symbol failed (attempt " + std::to_string(ms.attempts) + ")";
        router.end_migration(ms.move.symbol);
        ZEPTO_WARN("PartitionMigrator: move symbol={} {}→{} failed, rolled back (attempt {})",
                   ms.move.symbol, ms.move.from, ms.move.to, ms.attempts);
    }
}

void PartitionMigrator::rollback_move(SymbolId symbol, NodeId dest) {
    auto it = nodes_.find(dest);
    if (it == nodes_.end()) return;
    std::string sql = "DELETE FROM trades WHERE symbol = " + std::to_string(symbol);
    auto r = it->second->execute_sql(sql);
    if (!r.ok()) {
        ZEPTO_WARN("PartitionMigrator: rollback DELETE on node {} failed: {}",
                   dest, r.error);
    }
}

MigrationCheckpoint PartitionMigrator::execute_plan(
    const PartitionRouter::MigrationPlan& plan,
    PartitionRouter& router)
{
    MigrationCheckpoint cp;
    cp.moves.reserve(plan.moves.size());
    for (auto& m : plan.moves)
        cp.moves.push_back({m, MoveState::PENDING, {}, 0, 0});

    return resume_plan(std::move(cp), router);
}

MigrationCheckpoint PartitionMigrator::resume_plan(
    MigrationCheckpoint checkpoint,
    PartitionRouter& router)
{
    for (auto& ms : checkpoint.moves) {
        if (ms.state == MoveState::COMMITTED) continue;

        // Reset FAILED to retry
        if (ms.state == MoveState::FAILED) ms.state = MoveState::PENDING;

        while (ms.state == MoveState::PENDING && ms.attempts < max_retries_) {
            execute_move(ms, router);
            // Save checkpoint after each move attempt
            if (!checkpoint_path_.empty())
                checkpoint.save_to_file(checkpoint_path_);
            if (ms.state == MoveState::COMMITTED) break;
            if (ms.state == MoveState::FAILED && ms.attempts < max_retries_)
                ms.state = MoveState::PENDING;
        }
    }
    return checkpoint;
}

} // namespace zeptodb::cluster
