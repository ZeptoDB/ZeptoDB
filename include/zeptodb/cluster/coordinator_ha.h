#pragma once
// ============================================================================
// CoordinatorHA — Active-Standby coordinator failover
// ============================================================================
// Two QueryCoordinator instances: one ACTIVE, one STANDBY.
// Standby monitors active via periodic ping. On failure → promote.
//
// Node registry is replicated: add/remove on active are forwarded to standby.
// Queries always go through the active coordinator.
// ============================================================================

#include "zeptodb/cluster/query_coordinator.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/cluster/k8s_lease.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace zeptodb::cluster {

enum class CoordinatorRole : uint8_t { ACTIVE, STANDBY };

struct CoordinatorHAConfig {
    uint32_t ping_interval_ms = 500;   // how often standby pings active
    uint32_t failover_after_ms = 2000; // promote after this many ms without pong
    bool     require_lease = false;    // true = K8sLease 획득 필수 (프로덕션)
    LeaseConfig lease_config;          // K8sLease 설정
};

class CoordinatorHA {
public:
    explicit CoordinatorHA(CoordinatorHAConfig cfg = {}) : config_(cfg) {}
    ~CoordinatorHA() { stop(); }

    CoordinatorHA(const CoordinatorHA&) = delete;
    CoordinatorHA& operator=(const CoordinatorHA&) = delete;

    /// Set this instance's role and the peer coordinator's RPC address.
    /// For STANDBY: peer is the active coordinator's RPC endpoint.
    /// For ACTIVE: peer is the standby (used for node-list replication).
    void init(CoordinatorRole role, const std::string& peer_host,
              uint16_t peer_port);

    /// Start monitoring (standby pings active periodically).
    void start();
    void stop();

    CoordinatorRole role() const { return role_.load(); }
    bool is_active() const { return role_.load() == CoordinatorRole::ACTIVE; }

    /// Access the local coordinator (for query routing).
    QueryCoordinator& coordinator() { return coordinator_; }

    /// Register a node — replicated to peer if we are ACTIVE.
    void add_remote_node(NodeAddress addr);
    void add_local_node(NodeAddress addr, zeptodb::core::ZeptoPipeline& pipeline);
    void remove_node(NodeId id);

    /// Execute SQL via the active coordinator.
    zeptodb::sql::QueryResultSet execute_sql(const std::string& sql);

    /// Number of promotions (standby → active).
    size_t promotion_count() const { return promotions_.load(); }

    using PromotionCallback = std::function<void()>;
    void on_promotion(PromotionCallback cb) { promotion_cb_ = std::move(cb); }

    /// Access fencing token (for RPC epoch propagation)
    FencingToken& fencing_token() { return fencing_token_; }
    const FencingToken& fencing_token() const { return fencing_token_; }

    /// Current epoch (from fencing token)
    uint64_t epoch() const { return fencing_token_.current(); }

    /// Access K8sLease (nullptr if require_lease=false)
    K8sLease* lease() { return lease_.get(); }

private:
    void monitor_loop();
    bool try_promote();

    CoordinatorHAConfig config_;
    std::atomic<CoordinatorRole> role_{CoordinatorRole::STANDBY};

    QueryCoordinator coordinator_;
    FencingToken     fencing_token_;
    std::unique_ptr<K8sLease> lease_;

    std::string peer_host_;
    uint16_t    peer_port_ = 0;
    std::unique_ptr<TcpRpcClient> peer_rpc_;

    // Tracked node addresses for standby replay on promotion
    std::mutex node_mu_;
    std::vector<NodeAddress> registered_nodes_;

    std::atomic<bool> running_{false};
    std::thread       monitor_thread_;
    std::atomic<size_t> promotions_{0};
    PromotionCallback promotion_cb_;
};

} // namespace zeptodb::cluster
