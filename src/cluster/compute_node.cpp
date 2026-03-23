#include "apex/cluster/compute_node.h"

namespace apex::cluster {

void ComputeNode::add_data_node(NodeId id, const std::string& host,
                                 uint16_t port) {
    nodes_[id] = std::make_unique<TcpRpcClient>(host, port);
    coordinator_.add_remote_node({host, port, id});
}

apex::sql::QueryResultSet ComputeNode::execute_on(NodeId id,
                                                    const std::string& sql) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        apex::sql::QueryResultSet err;
        err.error = "ComputeNode: unknown data node " + std::to_string(id);
        return err;
    }
    return it->second->execute_sql(sql);
}

apex::sql::QueryResultSet ComputeNode::execute(const std::string& sql) {
    return coordinator_.execute_sql(sql);
}

size_t ComputeNode::fetch_and_ingest(NodeId source, const std::string& sql,
                                      apex::core::ApexPipeline& local) {
    auto result = execute_on(source, sql);
    if (!result.ok() || result.rows.empty()) return 0;

    // Find column indices
    int col_sym = -1, col_price = -1, col_vol = -1, col_ts = -1;
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        if (result.column_names[i] == "symbol")    col_sym   = static_cast<int>(i);
        if (result.column_names[i] == "price")     col_price = static_cast<int>(i);
        if (result.column_names[i] == "volume")    col_vol   = static_cast<int>(i);
        if (result.column_names[i] == "timestamp") col_ts    = static_cast<int>(i);
    }

    size_t count = 0;
    for (auto& row : result.rows) {
        apex::ingestion::TickMessage msg{};
        if (col_sym >= 0)   msg.symbol_id = static_cast<uint32_t>(row[col_sym]);
        if (col_price >= 0) msg.price     = row[col_price];
        if (col_vol >= 0)   msg.volume    = row[col_vol];
        if (col_ts >= 0)    msg.recv_ts   = row[col_ts];
        if (local.ingest_tick(msg)) ++count;
    }
    local.drain_sync(result.rows.size() + 100);
    return count;
}

} // namespace apex::cluster
