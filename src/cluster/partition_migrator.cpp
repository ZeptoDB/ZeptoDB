#include "apex/cluster/partition_migrator.h"
#include "apex/cluster/rpc_protocol.h"

namespace apex::cluster {

void PartitionMigrator::add_node(NodeId id, const std::string& host,
                                  uint16_t port) {
    nodes_[id] = std::make_unique<TcpRpcClient>(host, port);
}

bool PartitionMigrator::migrate_symbol(SymbolId symbol, NodeId from,
                                        NodeId to) {
    auto src_it = nodes_.find(from);
    auto dst_it = nodes_.find(to);
    if (src_it == nodes_.end() || dst_it == nodes_.end()) {
        stats_.moves_failed.fetch_add(1);
        return false;
    }

    // Query all rows for this symbol from source node
    std::string sql = "SELECT * FROM trades WHERE symbol = "
                    + std::to_string(symbol);
    auto result = src_it->second->execute_sql(sql);
    if (!result.ok() || result.rows.empty()) {
        // No data or error — still count as completed (nothing to move)
        if (result.ok()) {
            stats_.moves_completed.fetch_add(1);
            return true;
        }
        stats_.moves_failed.fetch_add(1);
        return false;
    }

    // Convert result rows back to TickMessages for WAL replay on dest
    // Result columns: symbol, price, volume, timestamp (standard trades schema)
    // Find column indices
    int col_price = -1, col_volume = -1, col_ts = -1;
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        if (result.column_names[i] == "price")     col_price  = static_cast<int>(i);
        if (result.column_names[i] == "volume")    col_volume = static_cast<int>(i);
        if (result.column_names[i] == "timestamp") col_ts     = static_cast<int>(i);
    }

    std::vector<apex::ingestion::TickMessage> batch;
    batch.reserve(result.rows.size());
    for (auto& row : result.rows) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = static_cast<uint32_t>(symbol);
        if (col_price >= 0)  msg.price   = row[col_price];
        if (col_volume >= 0) msg.volume  = row[col_volume];
        if (col_ts >= 0)     msg.recv_ts = row[col_ts];
        batch.push_back(msg);
    }

    // Send to destination as WAL batch
    bool ok = dst_it->second->replicate_wal(batch);
    if (ok) {
        stats_.moves_completed.fetch_add(1);
        stats_.rows_migrated.fetch_add(batch.size());
    } else {
        stats_.moves_failed.fetch_add(1);
    }
    return ok;
}

size_t PartitionMigrator::execute_plan(
    const PartitionRouter::MigrationPlan& plan,
    PartitionRouter& router)
{
    size_t success = 0;
    for (auto& move : plan.moves) {
        if (migrate_symbol(move.symbol, move.from, move.to))
            ++success;
    }
    // Apply routing changes after all moves complete
    // (caller is responsible for add_node/remove_node on the router)
    return success;
}

} // namespace apex::cluster
