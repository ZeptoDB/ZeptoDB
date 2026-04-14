#pragma once
// ============================================================================
// BandwidthThrottler — rate-limits data transfer during partition migration
// ============================================================================
// Tracks bytes transferred per window and sleeps when rate exceeds the
// configured limit. Thread-safe. Setting limit to 0 disables throttling.
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace zeptodb::cluster {

class BandwidthThrottler {
public:
    /// @param max_mbps  Maximum bandwidth in MB/s (0 = unlimited)
    explicit BandwidthThrottler(uint32_t max_mbps = 0)
        : max_bytes_per_sec_(static_cast<uint64_t>(max_mbps) * 1024ULL * 1024ULL)
        , window_start_(std::chrono::steady_clock::now()) {}

    /// Update the bandwidth limit at runtime (thread-safe).
    void set_limit_mbps(uint32_t mbps) {
        max_bytes_per_sec_.store(static_cast<uint64_t>(mbps) * 1024ULL * 1024ULL,
                                 std::memory_order_release);
        // Reset window to avoid stale accounting
        window_start_ = std::chrono::steady_clock::now();
        bytes_in_window_.store(0, std::memory_order_release);
    }

    uint32_t limit_mbps() const {
        return static_cast<uint32_t>(max_bytes_per_sec_.load(std::memory_order_acquire)
                                     / (1024ULL * 1024ULL));
    }

    /// Record bytes transferred. Blocks (sleeps) if rate exceeds limit.
    void record(size_t bytes) {
        uint64_t limit = max_bytes_per_sec_.load(std::memory_order_acquire);
        if (limit == 0 || bytes == 0) return;  // unlimited

        uint64_t prev = bytes_in_window_.fetch_add(bytes, std::memory_order_acq_rel);
        uint64_t total = prev + bytes;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            now - window_start_.load(std::memory_order_acquire));

        if (elapsed.count() <= 0) elapsed = std::chrono::microseconds(1);

        // bytes_per_sec = total / (elapsed_us / 1e6) = total * 1e6 / elapsed_us
        double current_rate = static_cast<double>(total) * 1'000'000.0
                            / static_cast<double>(elapsed.count());

        if (current_rate > static_cast<double>(limit)) {
            // Sleep until we're back under the limit:
            // required_time = total / limit (in seconds)
            // sleep = required_time - elapsed
            auto required_us = static_cast<int64_t>(
                static_cast<double>(total) * 1'000'000.0 / static_cast<double>(limit));
            auto sleep_us = required_us - elapsed.count();
            if (sleep_us > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
            }
        }

        // Reset window periodically (every ~1 second) to avoid drift
        auto now2 = std::chrono::steady_clock::now();
        auto window_age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now2 - window_start_.load(std::memory_order_acquire));
        if (window_age.count() >= 1000) {
            window_start_.store(now2, std::memory_order_release);
            bytes_in_window_.store(0, std::memory_order_release);
        }
    }

    /// Reset counters (e.g. between rebalance runs).
    void reset() {
        window_start_.store(std::chrono::steady_clock::now(), std::memory_order_release);
        bytes_in_window_.store(0, std::memory_order_release);
    }

    /// Total bytes recorded (for status reporting).
    uint64_t total_bytes() const {
        return bytes_in_window_.load(std::memory_order_acquire);
    }

private:
    std::atomic<uint64_t> max_bytes_per_sec_;
    std::atomic<std::chrono::steady_clock::time_point> window_start_;
    std::atomic<uint64_t> bytes_in_window_{0};
};

} // namespace zeptodb::cluster
