#include "apex/cluster/coordinator_ha.h"
#include <chrono>

namespace apex::cluster {

void CoordinatorHA::init(CoordinatorRole role, const std::string& peer_host,
                          uint16_t peer_port) {
    role_.store(role);
    peer_host_ = peer_host;
    peer_port_ = peer_port;
    peer_rpc_ = std::make_unique<TcpRpcClient>(peer_host, peer_port, 1000);
}

void CoordinatorHA::start() {
    if (running_.exchange(true)) return;
    if (role_.load() == CoordinatorRole::STANDBY) {
        monitor_thread_ = std::thread([this]() { monitor_loop(); });
    }
}

void CoordinatorHA::stop() {
    if (!running_.exchange(false)) return;
    if (monitor_thread_.joinable()) monitor_thread_.join();
}

void CoordinatorHA::add_remote_node(NodeAddress addr) {
    coordinator_.add_remote_node(addr);
    std::lock_guard<std::mutex> lock(node_mu_);
    registered_nodes_.push_back(addr);
}

void CoordinatorHA::add_local_node(NodeAddress addr,
                                    apex::core::ApexPipeline& pipeline) {
    coordinator_.add_local_node(addr, pipeline);
    std::lock_guard<std::mutex> lock(node_mu_);
    registered_nodes_.push_back(addr);
}

void CoordinatorHA::remove_node(NodeId id) {
    coordinator_.remove_node(id);
    std::lock_guard<std::mutex> lock(node_mu_);
    registered_nodes_.erase(
        std::remove_if(registered_nodes_.begin(), registered_nodes_.end(),
                       [id](const NodeAddress& a) { return a.id == id; }),
        registered_nodes_.end());
}

apex::sql::QueryResultSet CoordinatorHA::execute_sql(const std::string& sql) {
    if (is_active()) {
        return coordinator_.execute_sql(sql);
    }
    // Standby forwards to active via RPC
    if (peer_rpc_) {
        return peer_rpc_->execute_sql(sql);
    }
    apex::sql::QueryResultSet err;
    err.error = "CoordinatorHA: standby has no peer connection";
    return err;
}

void CoordinatorHA::monitor_loop() {
    using Clock = std::chrono::steady_clock;
    auto last_pong = Clock::now();

    while (running_.load()) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.ping_interval_ms));

        if (role_.load() != CoordinatorRole::STANDBY) continue;

        bool alive = peer_rpc_ && peer_rpc_->ping();
        if (alive) {
            last_pong = Clock::now();
            continue;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - last_pong).count();

        if (elapsed >= config_.failover_after_ms) {
            // Promote self to ACTIVE
            role_.store(CoordinatorRole::ACTIVE);
            promotions_.fetch_add(1);

            // Re-register all known nodes as remote into the coordinator
            // so it can immediately route queries after promotion.
            {
                std::lock_guard<std::mutex> lock(node_mu_);
                for (const auto& addr : registered_nodes_) {
                    coordinator_.add_remote_node(addr);
                }
            }

            if (promotion_cb_) promotion_cb_();
            break;  // stop monitoring — we are now active
        }
    }
}

} // namespace apex::cluster
