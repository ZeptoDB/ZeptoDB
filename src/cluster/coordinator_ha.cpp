#include "zeptodb/cluster/coordinator_ha.h"
#include "zeptodb/common/logger.h"
#include <chrono>

namespace zeptodb::cluster {

void CoordinatorHA::init(CoordinatorRole role, const std::string& peer_host,
                          uint16_t peer_port) {
    role_.store(role);
    peer_host_ = peer_host;
    peer_port_ = peer_port;
    peer_rpc_ = std::make_unique<TcpRpcClient>(peer_host, peer_port, 1000);

    // K8sLease 초기화 (require_lease=true일 때)
    if (config_.require_lease) {
        lease_ = std::make_unique<K8sLease>(config_.lease_config);
        lease_->on_elected([this]() {
            // Lease 획득 시 promote (monitor_loop 외부에서도 가능)
            if (role_.load() == CoordinatorRole::STANDBY) {
                try_promote();
            }
        });
        lease_->on_lost([this]() {
            // Lease 상실 시 demote
            if (role_.load() == CoordinatorRole::ACTIVE) {
                ZEPTO_WARN("CoordinatorHA: lease lost, demoting to STANDBY");
                role_.store(CoordinatorRole::STANDBY);
            }
        });
    }

    // ACTIVE로 초기화 시 즉시 epoch advance
    if (role == CoordinatorRole::ACTIVE) {
        fencing_token_.advance();
        if (peer_rpc_) peer_rpc_->set_epoch(fencing_token_.current());
    }
}

void CoordinatorHA::start() {
    if (running_.exchange(true)) return;

    // K8sLease 시작 (require_lease=true일 때)
    if (lease_) {
        lease_->start("coordinator-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    }

    if (role_.load() == CoordinatorRole::STANDBY) {
        monitor_thread_ = std::thread([this]() { monitor_loop(); });
    }
}

void CoordinatorHA::stop() {
    if (!running_.exchange(false)) return;
    if (lease_) lease_->stop();
    if (monitor_thread_.joinable()) monitor_thread_.join();
}

void CoordinatorHA::add_remote_node(NodeAddress addr) {
    coordinator_.add_remote_node(addr);
    std::lock_guard<std::mutex> lock(node_mu_);
    registered_nodes_.push_back(addr);
}

void CoordinatorHA::add_local_node(NodeAddress addr,
                                    zeptodb::core::ZeptoPipeline& pipeline) {
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

zeptodb::sql::QueryResultSet CoordinatorHA::execute_sql(const std::string& sql) {
    if (is_active()) {
        return coordinator_.execute_sql(sql);
    }
    // Standby forwards to active via RPC
    if (peer_rpc_) {
        return peer_rpc_->execute_sql(sql);
    }
    zeptodb::sql::QueryResultSet err;
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
            if (try_promote()) break;  // promoted — stop monitoring
            // Lease 획득 실패 시 재시도
            last_pong = Clock::now();
        }
    }
}

bool CoordinatorHA::try_promote() {
    // K8sLease가 필요한 경우 lease 획득 확인
    if (lease_ && !lease_->is_leader()) {
        // Lease 획득 시도
        if (!lease_->try_acquire()) {
            ZEPTO_WARN("CoordinatorHA: promotion blocked — lease not acquired");
            return false;
        }
    }

    // Epoch advance (stale coordinator의 write 거부를 위해)
    uint64_t new_epoch = fencing_token_.advance();
    ZEPTO_INFO("CoordinatorHA: promoting to ACTIVE (epoch={})", new_epoch);

    role_.store(CoordinatorRole::ACTIVE);
    promotions_.fetch_add(1);

    // 전체 RPC client에 epoch 전파
    if (peer_rpc_) peer_rpc_->set_epoch(new_epoch);

    // Re-register all known nodes
    {
        std::lock_guard<std::mutex> lock(node_mu_);
        for (const auto& addr : registered_nodes_) {
            coordinator_.add_remote_node(addr);
        }
    }

    if (promotion_cb_) promotion_cb_();
    return true;
}

} // namespace zeptodb::cluster
