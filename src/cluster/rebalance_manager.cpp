#include "zeptodb/cluster/rebalance_manager.h"
#include "zeptodb/cluster/ring_consensus.h"
#include "zeptodb/common/logger.h"

namespace zeptodb::cluster {

RebalanceManager::RebalanceManager(PartitionRouter& router,
                                   PartitionMigrator& migrator,
                                   RebalanceConfig cfg)
    : router_(router), migrator_(migrator), cfg_(std::move(cfg))
    , throttler_(cfg_.max_bandwidth_mbps) {}

RebalanceManager::~RebalanceManager() {
    stop_policy();
    cancel();
    if (worker_.joinable()) worker_.join();
}

bool RebalanceManager::start_add_node(NodeId new_node) {
    auto plan = router_.plan_add(new_node);
    if (plan.moves.empty()) return false;
    {
        std::lock_guard lk(mu_);
        action_ = RebalanceAction::ADD_NODE;
        action_node_ = new_node;
    }
    ZEPTO_INFO("RebalanceManager: start_add_node id={}, {} moves",
               new_node, plan.moves.size());
    return start_plan(std::move(plan));
}

bool RebalanceManager::start_remove_node(NodeId leaving) {
    auto plan = router_.plan_remove(leaving);
    if (plan.moves.empty()) return false;
    {
        std::lock_guard lk(mu_);
        action_ = RebalanceAction::REMOVE_NODE;
        action_node_ = leaving;
    }
    ZEPTO_INFO("RebalanceManager: start_remove_node id={}, {} moves",
               leaving, plan.moves.size());
    return start_plan(std::move(plan));
}

void RebalanceManager::set_max_bandwidth_mbps(uint32_t mbps) {
    cfg_.max_bandwidth_mbps = mbps;
    throttler_.set_limit_mbps(mbps);
    ZEPTO_INFO("RebalanceManager: bandwidth limit set to {} MB/s (0=unlimited)", mbps);
}

bool RebalanceManager::start_move_partitions(std::vector<PartitionRouter::Move> moves) {
    if (moves.empty()) return false;
    PartitionRouter::MigrationPlan plan;
    plan.moves = std::move(moves);
    {
        std::lock_guard lk(mu_);
        action_ = RebalanceAction::NONE;
        action_node_ = 0;
    }
    ZEPTO_INFO("RebalanceManager: start_move_partitions, {} moves", plan.moves.size());
    return start_plan(std::move(plan));
}

bool RebalanceManager::start_plan(PartitionRouter::MigrationPlan plan) {
    auto expected = RebalanceState::IDLE;
    if (!state_.compare_exchange_strong(expected, RebalanceState::RUNNING))
        return false;  // already running

    if (worker_.joinable()) worker_.join();

    {
        std::lock_guard lk(mu_);
        plan_ = std::move(plan);
        status_ = {};
        status_.state = RebalanceState::RUNNING;
        status_.total_moves = plan_.moves.size();
        run_start_ = std::chrono::steady_clock::now();
    }

    if (!cfg_.checkpoint_dir.empty())
        migrator_.set_checkpoint_path(cfg_.checkpoint_dir + "/rebalance.json");

    if (cfg_.move_timeout_sec > 0)
        migrator_.set_move_timeout(cfg_.move_timeout_sec);

    migrator_.set_throttler(cfg_.max_bandwidth_mbps > 0 ? &throttler_ : nullptr);

    worker_ = std::thread([this] { run_loop(); });
    return true;
}

void RebalanceManager::run_loop() {
    for (size_t i = 0; i < plan_.moves.size(); ++i) {
        auto cur = state_.load(std::memory_order_acquire);
        if (cur == RebalanceState::CANCELLING) break;

        // Handle pause
        if (cur == RebalanceState::PAUSED) {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [this] {
                auto s = state_.load(std::memory_order_acquire);
                return s == RebalanceState::RUNNING ||
                       s == RebalanceState::CANCELLING;
            });
            if (state_.load(std::memory_order_acquire) == RebalanceState::CANCELLING)
                break;
        }

        auto& move = plan_.moves[i];
        {
            std::lock_guard lk(mu_);
            status_.current_symbol = std::to_string(move.symbol);
        }

        // Execute via migrator (handles begin_migration → copy → end_migration)
        PartitionRouter::MigrationPlan single;
        single.moves.push_back(move);
        auto result = migrator_.execute_plan(single, router_);

        {
            std::lock_guard lk(mu_);
            if (!result.moves.empty() &&
                result.moves[0].state == MoveState::COMMITTED) {
                status_.completed_moves++;
            } else {
                status_.failed_moves++;
            }
        }
    }

    // Broadcast ring update to all nodes via RingConsensus
    if (consensus_) {
        RebalanceAction act;
        NodeId node;
        size_t completed;
        {
            std::lock_guard lk(mu_);
            act = action_;
            node = action_node_;
            completed = status_.completed_moves;
        }
        if (completed > 0 && act != RebalanceAction::NONE && state_.load(std::memory_order_acquire) != RebalanceState::CANCELLING) {
            bool ok = (act == RebalanceAction::ADD_NODE)
                          ? consensus_->propose_add(node)
                          : consensus_->propose_remove(node);
            if (!ok) {
                ZEPTO_WARN("RebalanceManager: ring broadcast failed for node {}", node);
            }
        }
    }

    // Done — record history and reset
    size_t completed, failed;
    {
        std::lock_guard lk(mu_);
        // Record history entry
        RebalanceHistoryEntry entry;
        entry.action = action_;
        entry.node_id = action_node_;
        entry.total_moves = status_.total_moves;
        entry.completed_moves = status_.completed_moves;
        entry.failed_moves = status_.failed_moves;
        auto now = std::chrono::steady_clock::now();
        entry.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - run_start_).count();
        entry.start_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() - entry.duration_ms;
        entry.cancelled = (state_.load(std::memory_order_acquire) == RebalanceState::CANCELLING);
        if (history_.size() >= MAX_HISTORY) history_.erase(history_.begin());
        history_.push_back(entry);

        status_.state = RebalanceState::IDLE;
        status_.current_symbol.clear();
        completed = status_.completed_moves;
        failed = status_.failed_moves;
        action_ = RebalanceAction::NONE;
        action_node_ = 0;
    }
    state_.store(RebalanceState::IDLE, std::memory_order_release);
    done_cv_.notify_all();
    ZEPTO_INFO("RebalanceManager: finished — completed={}, failed={}",
               completed, failed);
}

void RebalanceManager::pause() {
    auto expected = RebalanceState::RUNNING;
    if (state_.compare_exchange_strong(expected, RebalanceState::PAUSED)) {
        std::lock_guard lk(mu_);
        status_.state = RebalanceState::PAUSED;
        ZEPTO_INFO("RebalanceManager: paused");
    }
}

void RebalanceManager::resume() {
    auto expected = RebalanceState::PAUSED;
    if (state_.compare_exchange_strong(expected, RebalanceState::RUNNING)) {
        {
            std::lock_guard lk(mu_);
            status_.state = RebalanceState::RUNNING;
        }
        cv_.notify_all();
        ZEPTO_INFO("RebalanceManager: resumed");
    }
}

void RebalanceManager::cancel() {
    auto cur = state_.load(std::memory_order_acquire);
    while (cur != RebalanceState::IDLE && cur != RebalanceState::CANCELLING) {
        if (state_.compare_exchange_weak(cur, RebalanceState::CANCELLING,
                                         std::memory_order_acq_rel))
            break;
    }
    if (cur == RebalanceState::IDLE || cur == RebalanceState::CANCELLING) return;

    {
        std::lock_guard lk(mu_);
        status_.state = RebalanceState::CANCELLING;
    }
    cv_.notify_all();  // wake paused thread
    ZEPTO_INFO("RebalanceManager: cancelling");
}

RebalanceStatus RebalanceManager::status() const {
    std::lock_guard lk(mu_);
    return status_;
}

std::vector<RebalanceHistoryEntry> RebalanceManager::history() const {
    std::lock_guard lk(mu_);
    return history_;
}

bool RebalanceManager::wait(uint32_t timeout_sec) {
    std::unique_lock lk(mu_);
    auto pred = [this] {
        return state_.load(std::memory_order_acquire) == RebalanceState::IDLE;
    };
    if (timeout_sec == 0) {
        done_cv_.wait(lk, pred);
        return true;
    }
    return done_cv_.wait_for(lk, std::chrono::seconds(timeout_sec), pred);
}

void RebalanceManager::set_load_provider(LoadProvider provider) {
    std::lock_guard lk(policy_mu_);
    load_provider_ = std::move(provider);
}

void RebalanceManager::start_policy() {
    bool has_provider;
    {
        std::lock_guard lk(policy_mu_);
        has_provider = !!load_provider_;
    }
    if (!cfg_.policy.enabled || !has_provider) return;
    if (policy_running_.exchange(true)) return;  // already running
    last_auto_rebalance_ = std::chrono::steady_clock::now();  // startup grace period
    policy_thread_ = std::thread([this] { policy_loop(); });
}

void RebalanceManager::stop_policy() {
    if (!policy_running_.exchange(false)) return;
    policy_cv_.notify_all();
    if (policy_thread_.joinable()) policy_thread_.join();
}

void RebalanceManager::policy_loop() {
    ZEPTO_INFO("RebalanceManager: policy loop started (interval={}s, ratio={}, cooldown={}s)",
               cfg_.policy.check_interval_sec, cfg_.policy.imbalance_ratio, cfg_.policy.cooldown_sec);

    while (policy_running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock lk(policy_mu_);
            policy_cv_.wait_for(lk, std::chrono::seconds(cfg_.policy.check_interval_sec),
                                [this] { return !policy_running_.load(std::memory_order_acquire); });
        }
        if (!policy_running_.load(std::memory_order_acquire)) break;
        if (state_.load(std::memory_order_acquire) != RebalanceState::IDLE) continue;

        // Check cooldown
        auto now = std::chrono::steady_clock::now();
        if (now - last_auto_rebalance_ < std::chrono::seconds(cfg_.policy.cooldown_sec)) continue;

        // Get load from provider (copy under lock to avoid data race)
        LoadProvider provider_copy;
        {
            std::lock_guard lk(policy_mu_);
            provider_copy = load_provider_;
        }
        if (!provider_copy) continue;
        auto loads = provider_copy();
        if (loads.size() < 2) continue;

        // Find min/max
        size_t min_count = loads[0].second, max_count = loads[0].second;
        NodeId min_node = loads[0].first, max_node = loads[0].first;
        for (auto& [id, count] : loads) {
            if (count < min_count) { min_count = count; min_node = id; }
            if (count > max_count) { max_count = count; max_node = id; }
        }

        if (min_count == 0) {
            // Empty node exists — redistribute partitions TO it via plan_add
            // (only works if min_node is not yet in the ring)
            auto plan = router_.plan_add(min_node);
            if (!plan.moves.empty()) {
                ZEPTO_INFO("RebalanceManager: auto-trigger — node {} has 0 partitions, redistributing",
                           min_node);
                if (start_plan(std::move(plan))) {
                    last_auto_rebalance_ = now;
                }
            } else {
                // Node already in ring but empty — drain max node to redistribute
                ZEPTO_INFO("RebalanceManager: auto-trigger — node {} has 0 partitions, draining node {}",
                           min_node, max_node);
                if (start_remove_node(max_node)) {
                    last_auto_rebalance_ = now;
                }
            }
            continue;
        }

        double ratio = static_cast<double>(max_count) / static_cast<double>(min_count);
        if (ratio <= cfg_.policy.imbalance_ratio) continue;

        // Redistribute: drain the most loaded node so ring distributes its
        // partitions to the next nodes. This is aggressive (moves all partitions
        // from max_node) but is the only mechanism available with consistent
        // hashing plan_add/plan_remove. A future improvement would add a
        // targeted partial-move API.
        if (start_remove_node(max_node)) {
            ZEPTO_INFO("RebalanceManager: auto-trigger — ratio={:.2f} (node {} has {}, node {} has {})",
                       ratio, max_node, max_count, min_node, min_count);
            last_auto_rebalance_ = now;
        }
    }

    ZEPTO_INFO("RebalanceManager: policy loop stopped");
}

} // namespace zeptodb::cluster
