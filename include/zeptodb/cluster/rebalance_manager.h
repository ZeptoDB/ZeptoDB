#pragma once
// ============================================================================
// RebalanceManager — orchestrates live partition rebalancing
// ============================================================================
// Coordinates zero-downtime partition migration when nodes are added/removed.
// Uses PartitionRouter for planning and PartitionMigrator for execution.
//
// State machine:  IDLE → RUNNING ⇄ PAUSED → IDLE
//                   ↓       ↓         ↓
//                   └── CANCELLING ──→ IDLE
// ============================================================================

#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/partition_migrator.h"
#include "zeptodb/cluster/bandwidth_throttler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zeptodb::cluster {

class RingConsensus;  // forward declaration — broadcast ring after rebalance

enum class RebalanceAction : uint8_t { NONE, ADD_NODE, REMOVE_NODE };

struct RebalancePolicy {
    bool     enabled = false;
    double   imbalance_ratio = 2.0;    // trigger if max/min partition count > 2x
    uint32_t check_interval_sec = 60;  // how often to check
    uint32_t cooldown_sec = 300;       // min time between auto-rebalances
};

struct RebalanceConfig {
    std::string checkpoint_dir;          // for crash recovery
    uint32_t move_timeout_sec = 300;     // per-move timeout (0 = no timeout)
    uint32_t max_bandwidth_mbps = 0;     // bandwidth throttle (0 = unlimited)
    RebalancePolicy policy;              // load-based auto-trigger
};

enum class RebalanceState : uint8_t {
    IDLE, RUNNING, PAUSED, CANCELLING
};

struct RebalanceStatus {
    RebalanceState state = RebalanceState::IDLE;
    size_t total_moves = 0;
    size_t completed_moves = 0;
    size_t failed_moves = 0;
    std::string current_symbol;
};

struct RebalanceHistoryEntry {
    RebalanceAction action = RebalanceAction::NONE;
    NodeId node_id = 0;
    size_t total_moves = 0;
    size_t completed_moves = 0;
    size_t failed_moves = 0;
    int64_t start_time_ms = 0;   // epoch millis
    int64_t duration_ms = 0;
    bool cancelled = false;
};

class RebalanceManager {
public:
    RebalanceManager(PartitionRouter& router, PartitionMigrator& migrator,
                     RebalanceConfig cfg = {});
    ~RebalanceManager();

    // Non-copyable
    RebalanceManager(const RebalanceManager&) = delete;
    RebalanceManager& operator=(const RebalanceManager&) = delete;

    /// Scale-out: plan and execute migration for a new node.
    bool start_add_node(NodeId new_node);

    /// Scale-in: plan and execute migration away from a leaving node.
    bool start_remove_node(NodeId leaving);

    /// Move specific partitions between existing nodes (no full drain).
    /// Returns false if moves is empty or a rebalance is already running.
    bool start_move_partitions(std::vector<PartitionRouter::Move> moves);

    /// Pause the current rebalance (in-flight move completes first).
    void pause();

    /// Resume a paused rebalance.
    void resume();

    /// Cancel the current rebalance (committed moves stay committed).
    void cancel();

    /// Current status snapshot.
    RebalanceStatus status() const;

    /// Current state.
    RebalanceState state() const { return state_.load(std::memory_order_acquire); }

    /// Block until rebalance completes. Returns true if finished, false on timeout.
    /// timeout_sec=0 means wait indefinitely.
    bool wait(uint32_t timeout_sec = 0);

    /// Returns the history of completed rebalance events (most recent first).
    std::vector<RebalanceHistoryEntry> history() const;

    /// Set a callback that returns per-node partition counts for load checking.
    using LoadProvider = std::function<std::vector<std::pair<NodeId, size_t>>()>;
    void set_load_provider(LoadProvider provider);

    /// Start the periodic policy check loop (requires load_provider).
    void start_policy();

    /// Stop the periodic policy check loop.
    void stop_policy();

    /// Current config (read-only).
    const RebalanceConfig& config() const { return cfg_; }

    /// Update bandwidth limit at runtime (thread-safe, takes effect immediately).
    void set_max_bandwidth_mbps(uint32_t mbps);

    /// Set RingConsensus for broadcasting ring updates after rebalance.
    void set_consensus(RingConsensus* c) { consensus_ = c; }

private:
    bool start_plan(PartitionRouter::MigrationPlan plan);
    void run_loop();
    void policy_loop();

    PartitionRouter&  router_;
    PartitionMigrator& migrator_;
    RebalanceConfig   cfg_;

    std::atomic<RebalanceState> state_{RebalanceState::IDLE};

    mutable std::mutex mu_;
    std::condition_variable cv_;           // pause/resume/cancel signaling
    std::condition_variable done_cv_;      // wait() signaling

    PartitionRouter::MigrationPlan plan_;
    RebalanceStatus status_;               // guarded by mu_

    std::thread worker_;

    // Policy thread members
    LoadProvider load_provider_;
    std::atomic<bool> policy_running_{false};
    std::thread policy_thread_;
    std::mutex policy_mu_;
    std::condition_variable policy_cv_;
    std::chrono::steady_clock::time_point last_auto_rebalance_{};

    // RingConsensus broadcast
    RingConsensus* consensus_ = nullptr;
    RebalanceAction action_ = RebalanceAction::NONE;
    NodeId action_node_ = 0;
    std::vector<RebalanceHistoryEntry> history_;  // guarded by mu_
    std::chrono::steady_clock::time_point run_start_{};
    static constexpr size_t MAX_HISTORY = 50;

    // Bandwidth throttling
    BandwidthThrottler throttler_;
};

} // namespace zeptodb::cluster
