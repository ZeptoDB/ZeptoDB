// ============================================================================
// BandwidthThrottler unit tests
// ============================================================================

#include "zeptodb/cluster/bandwidth_throttler.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace zeptodb::cluster;

// ============================================================================
// Basic functionality
// ============================================================================

TEST(BandwidthThrottlerTest, UnlimitedDoesNotBlock) {
    BandwidthThrottler t(0);  // unlimited
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i)
        t.record(1024 * 1024);  // 100 MB
    auto elapsed = std::chrono::steady_clock::now() - start;
    // Should complete nearly instantly (< 50ms)
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 50);
}

TEST(BandwidthThrottlerTest, ZeroBytesDoesNotBlock) {
    BandwidthThrottler t(1);  // 1 MB/s
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i)
        t.record(0);
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 50);
}

TEST(BandwidthThrottlerTest, ThrottlesWhenOverLimit) {
    BandwidthThrottler t(1);  // 1 MB/s
    auto start = std::chrono::steady_clock::now();
    // Send 2 MB at once — should sleep ~1 second
    t.record(2 * 1024 * 1024);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    // Expect at least 500ms of throttling (conservative to avoid flaky tests)
    EXPECT_GE(elapsed_ms, 500);
}

TEST(BandwidthThrottlerTest, LimitMbpsAccessor) {
    BandwidthThrottler t(42);
    EXPECT_EQ(t.limit_mbps(), 42u);
    t.set_limit_mbps(100);
    EXPECT_EQ(t.limit_mbps(), 100u);
    t.set_limit_mbps(0);
    EXPECT_EQ(t.limit_mbps(), 0u);
}

TEST(BandwidthThrottlerTest, SetLimitResetsWindow) {
    BandwidthThrottler t(1);  // 1 MB/s
    t.record(512 * 1024);     // 0.5 MB recorded
    t.set_limit_mbps(1);      // reset window
    // After reset, total_bytes should be 0
    EXPECT_EQ(t.total_bytes(), 0u);
}

TEST(BandwidthThrottlerTest, ResetClearsCounters) {
    BandwidthThrottler t(10);
    t.record(1024 * 1024);
    EXPECT_GT(t.total_bytes(), 0u);
    t.reset();
    EXPECT_EQ(t.total_bytes(), 0u);
}

TEST(BandwidthThrottlerTest, RuntimeLimitChange) {
    BandwidthThrottler t(0);  // start unlimited
    // Record lots of data — should not block
    auto start = std::chrono::steady_clock::now();
    t.record(100 * 1024 * 1024);
    auto elapsed1 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(elapsed1, 50);

    // Switch to 1 MB/s — next record should throttle
    t.set_limit_mbps(1);
    start = std::chrono::steady_clock::now();
    t.record(2 * 1024 * 1024);  // 2 MB at 1 MB/s → ~1s sleep
    auto elapsed2 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_GE(elapsed2, 500);
}

// ============================================================================
// Thread safety
// ============================================================================

TEST(BandwidthThrottlerTest, ConcurrentRecordNoRace) {
    BandwidthThrottler t(0);  // unlimited — just test no crash/race
    std::atomic<uint64_t> total_bytes{0};
    constexpr int N_THREADS = 4;
    constexpr int N_ITERS = 1000;
    constexpr size_t CHUNK = 1024;

    std::vector<std::thread> threads;
    for (int i = 0; i < N_THREADS; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < N_ITERS; ++j) {
                t.record(CHUNK);
                total_bytes.fetch_add(CHUNK);
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(total_bytes.load(), N_THREADS * N_ITERS * CHUNK);
}

TEST(BandwidthThrottlerTest, ConcurrentRecordWithThrottle) {
    BandwidthThrottler t(10);  // 10 MB/s
    constexpr int N_THREADS = 4;
    constexpr int N_ITERS = 50;
    constexpr size_t CHUNK = 64;

    std::vector<std::thread> threads;
    for (int i = 0; i < N_THREADS; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < N_ITERS; ++j)
                t.record(CHUNK);
        });
    }
    for (auto& th : threads) th.join();
    // Just verify no crash — throttle amount is small enough to finish quickly
}

TEST(BandwidthThrottlerTest, DefaultConstructorIsUnlimited) {
    BandwidthThrottler t;
    EXPECT_EQ(t.limit_mbps(), 0u);
}
