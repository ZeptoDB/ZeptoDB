#pragma once
// ============================================================================
// APEX-DB: CancellationToken
// ============================================================================
// Lightweight header-only cooperative cancellation signal.
// Used by QueryTracker to signal running queries to stop early.
// Checked by QueryExecutor at partition scan boundaries.
// ============================================================================
#include <atomic>

namespace apex::auth {

class CancellationToken {
public:
    void cancel() noexcept {
        cancelled_.store(true, std::memory_order_release);
    }
    bool is_cancelled() const noexcept {
        return cancelled_.load(std::memory_order_acquire);
    }
private:
    std::atomic<bool> cancelled_{false};
};

} // namespace apex::auth
