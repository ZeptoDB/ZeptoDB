#pragma once
// ============================================================================
// APEX-DB: K8s Lease-based Leader Election + Fencing Token
// ============================================================================
// Solves split-brain:
//   1. K8sLease: only one node holds the lease at a time (leader)
//   2. FencingToken: monotonically increasing token attached to writes
//      — stale leader's writes are rejected by data nodes
//
// K8s Lease API:
//   PUT /apis/coordination.k8s.io/v1/namespaces/{ns}/leases/{name}
//   Fields: holderIdentity, leaseDurationSeconds, acquireTime, renewTime
//
// Without real K8s: FencingToken alone prevents split-brain via epoch numbers.
// ============================================================================

#include "apex/cluster/transport.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace apex::cluster {

// ============================================================================
// FencingToken: monotonically increasing epoch for write ownership
// ============================================================================
// Every coordinator promotion increments the global epoch.
// Data nodes reject writes with epoch < their last seen epoch.
// This prevents a stale (partitioned) coordinator from corrupting data.
//
//   Coordinator A (epoch=5) ──X── network partition
//   Coordinator B promoted (epoch=6)
//   A recovers, tries write with epoch=5 → REJECTED (6 > 5)
// ============================================================================
class FencingToken {
public:
    FencingToken() = default;
    explicit FencingToken(uint64_t initial) : epoch_(initial) {}

    /// Generate next token (called on promotion)
    uint64_t advance() { return ++epoch_; }

    /// Current token value
    uint64_t current() const { return epoch_.load(); }

    /// Validate incoming token: returns true if token >= last seen
    /// Updates last_seen on success (monotonic gate)
    bool validate(uint64_t token) {
        uint64_t expected = last_seen_.load();
        while (token >= expected) {
            if (last_seen_.compare_exchange_weak(expected, token))
                return true;
        }
        return false;  // stale token
    }

    /// Current last_seen value (for monitoring)
    uint64_t last_seen() const { return last_seen_.load(); }

private:
    std::atomic<uint64_t> epoch_{0};
    std::atomic<uint64_t> last_seen_{0};
};

// ============================================================================
// K8sLease: leader election via Kubernetes Lease API
// ============================================================================
// In production: uses K8s coordination.k8s.io/v1 Lease resource.
// This implementation provides the contract + a local simulation for testing.
//
// Usage:
//   K8sLease lease(config);
//   lease.on_elected([&]{ coordinator.promote(); });
//   lease.on_lost([&]{ coordinator.demote(); });
//   lease.start(self_id);
// ============================================================================

struct LeaseConfig {
    std::string namespace_name = "default";
    std::string lease_name     = "apex-coordinator";
    uint32_t    lease_duration_ms = 15000;  // lease TTL
    uint32_t    renew_interval_ms = 5000;   // renewal frequency
    uint32_t    retry_interval_ms = 2000;   // acquire retry
};

class K8sLease {
public:
    using Callback = std::function<void()>;

    explicit K8sLease(LeaseConfig cfg = LeaseConfig{}) : cfg_(std::move(cfg)) {}
    ~K8sLease() { stop(); }

    K8sLease(const K8sLease&) = delete;
    K8sLease& operator=(const K8sLease&) = delete;

    void on_elected(Callback cb) { on_elected_ = std::move(cb); }
    void on_lost(Callback cb)    { on_lost_ = std::move(cb); }

    void start(const std::string& holder_id) {
        if (running_.exchange(true)) return;
        holder_id_ = holder_id;
        thread_ = std::thread([this] { lease_loop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    bool is_leader() const { return is_leader_.load(); }
    std::string current_holder() const {
        std::lock_guard<std::mutex> lock(mu_);
        return current_holder_;
    }
    uint64_t epoch() const { return token_.current(); }

    /// Force release lease (graceful step-down)
    void release() {
        std::lock_guard<std::mutex> lock(mu_);
        if (is_leader_.load()) {
            is_leader_.store(false);
            current_holder_.clear();
            if (on_lost_) on_lost_();
        }
    }

    /// Access fencing token (data nodes use this to validate writes)
    FencingToken& fencing_token() { return token_; }
    const FencingToken& fencing_token() const { return token_; }

    // -- For testing: simulate K8s Lease behavior locally ----------------

    /// Try to acquire lease (returns true if acquired)
    bool try_acquire() {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = steady_now_ms();

        // Lease expired or not held?
        if (current_holder_.empty() || now >= lease_expiry_ms_) {
            current_holder_ = holder_id_;
            lease_expiry_ms_ = now + cfg_.lease_duration_ms;
            if (!is_leader_.load()) {
                is_leader_.store(true);
                token_.advance();
                if (on_elected_) on_elected_();
            }
            return true;
        }
        // Already held by us?
        if (current_holder_ == holder_id_) {
            lease_expiry_ms_ = now + cfg_.lease_duration_ms;
            return true;
        }
        return false;  // held by someone else
    }

    /// Simulate another node acquiring the lease (for testing split-brain)
    void force_holder(const std::string& other_id) {
        std::lock_guard<std::mutex> lock(mu_);
        bool was_leader = is_leader_.load();
        current_holder_ = other_id;
        lease_expiry_ms_ = steady_now_ms() + cfg_.lease_duration_ms;
        if (holder_id_ != other_id && was_leader) {
            is_leader_.store(false);
            if (on_lost_) on_lost_();
        }
    }

private:
    void lease_loop() {
        while (running_.load()) {
            if (is_leader_.load()) {
                // Renew
                bool renewed = try_acquire();
                if (!renewed) {
                    // Lost lease
                    is_leader_.store(false);
                    if (on_lost_) on_lost_();
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(cfg_.renew_interval_ms));
            } else {
                // Try acquire
                try_acquire();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(cfg_.retry_interval_ms));
            }
        }
    }

    static uint64_t steady_now_ms() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    LeaseConfig cfg_;
    std::string holder_id_;
    FencingToken token_;

    std::atomic<bool> is_leader_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;

    mutable std::mutex mu_;
    std::string current_holder_;
    uint64_t    lease_expiry_ms_ = 0;

    Callback on_elected_;
    Callback on_lost_;
};

} // namespace apex::cluster
