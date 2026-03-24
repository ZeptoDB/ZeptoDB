// ============================================================================
// ZeptoDB Scheduler Tests
// Tests kdb+ .z.ts equivalent: interval, daily-at, once-at
// ============================================================================
#include <gtest/gtest.h>
#include "zeptodb/scheduler/scheduler.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace zeptodb::scheduler;
using namespace std::chrono_literals;

// ============================================================================
// Basic lifecycle
// ============================================================================

TEST(Scheduler, StartStop) {
    Scheduler s;
    EXPECT_FALSE(s.is_running());
    s.start();
    EXPECT_TRUE(s.is_running());
    s.stop();
    EXPECT_FALSE(s.is_running());
}

TEST(Scheduler, DoubleStartIsHarmless) {
    Scheduler s;
    s.start();
    s.start();  // second call is a no-op
    EXPECT_TRUE(s.is_running());
    s.stop();
}

TEST(Scheduler, DoubleStopIsHarmless) {
    Scheduler s;
    s.start();
    s.stop();
    s.stop();  // second call is a no-op
    EXPECT_FALSE(s.is_running());
}

// ============================================================================
// Interval jobs
// ============================================================================

TEST(Scheduler, IntervalJobFires) {
    Scheduler s;
    std::atomic<int> count{0};

    s.add_job("tick", 20, [&]() { ++count; });
    s.start();
    std::this_thread::sleep_for(120ms);
    s.stop();

    // 20ms interval over 120ms → should fire at least 3 times, at most 8
    EXPECT_GE(count.load(), 3);
    EXPECT_LE(count.load(), 8);
}

TEST(Scheduler, IntervalJobCanBeCancelled) {
    Scheduler s;
    s.start();

    std::atomic<int> count{0};
    s.add_job("job", 20, [&]() { ++count; });
    std::this_thread::sleep_for(30ms);
    s.cancel("job");
    int snap = count.load();
    std::this_thread::sleep_for(60ms);

    // No additional fires after cancel
    EXPECT_EQ(count.load(), snap);
    s.stop();
}

TEST(Scheduler, MultipleIntervalJobs) {
    Scheduler s;
    s.start();

    std::atomic<int> a{0}, b{0};
    s.add_job("fast", 15, [&]() { ++a; });
    s.add_job("slow", 50, [&]() { ++b; });
    std::this_thread::sleep_for(200ms);
    s.stop();

    // fast fires more than slow
    EXPECT_GT(a.load(), b.load());
    EXPECT_GE(b.load(), 2);
}

TEST(Scheduler, InvalidIntervalThrows) {
    Scheduler s;
    EXPECT_THROW(s.add_job("bad", 0, []() {}), std::invalid_argument);
    EXPECT_THROW(s.add_job("bad", -1, []() {}), std::invalid_argument);
}

// ============================================================================
// One-shot jobs
// ============================================================================

TEST(Scheduler, OnceJobFiresOnce) {
    Scheduler s;
    s.start();

    std::atomic<int> count{0};
    int64_t fire_at = Scheduler::now_ns() + 30'000'000LL;  // 30ms from now
    s.add_once("one", fire_at, [&]() { ++count; });

    std::this_thread::sleep_for(100ms);
    s.stop();

    EXPECT_EQ(count.load(), 1);
}

TEST(Scheduler, OnceJobRemovedAfterFiring) {
    Scheduler s;
    s.start();

    int64_t fire_at = Scheduler::now_ns() + 20'000'000LL;
    s.add_once("one", fire_at, []() {});
    std::this_thread::sleep_for(60ms);

    // Job should no longer be in the list after firing
    auto jobs = s.list_jobs();
    bool found = false;
    for (const auto& j : jobs) {
        if (j == "one") { found = true; break; }
    }
    EXPECT_FALSE(found);
    s.stop();
}

TEST(Scheduler, OnceJobInPastFiresImmediately) {
    Scheduler s;
    s.start();

    std::atomic<int> count{0};
    int64_t past = Scheduler::now_ns() - 1'000'000'000LL;  // 1s in the past
    s.add_once("past", past, [&]() { ++count; });
    std::this_thread::sleep_for(50ms);
    s.stop();

    EXPECT_EQ(count.load(), 1);
}

// ============================================================================
// Cancel and list
// ============================================================================

TEST(Scheduler, CancelNonExistentReturnsFalse) {
    Scheduler s;
    EXPECT_FALSE(s.cancel("nonexistent"));
}

TEST(Scheduler, CancelAll) {
    Scheduler s;
    s.start();

    std::atomic<int> count{0};
    s.add_job("a", 20, [&]() { ++count; });
    s.add_job("b", 20, [&]() { ++count; });
    s.add_job("c", 20, [&]() { ++count; });
    std::this_thread::sleep_for(10ms);  // let at most one fire
    s.cancel_all();
    int snap = count.load();
    std::this_thread::sleep_for(80ms);

    EXPECT_EQ(count.load(), snap);
    EXPECT_TRUE(s.list_jobs().empty());
    s.stop();
}

TEST(Scheduler, ListJobs) {
    Scheduler s;
    s.add_job("alpha", 1000, []() {});
    s.add_job("beta",  2000, []() {});
    s.add_once("gamma", Scheduler::now_ns() + 10'000'000'000LL, []() {});

    auto jobs = s.list_jobs();
    ASSERT_EQ(jobs.size(), 3u);

    bool has_alpha = false, has_beta = false, has_gamma = false;
    for (const auto& j : jobs) {
        if (j == "alpha") has_alpha = true;
        if (j == "beta")  has_beta  = true;
        if (j == "gamma") has_gamma = true;
    }
    EXPECT_TRUE(has_alpha);
    EXPECT_TRUE(has_beta);
    EXPECT_TRUE(has_gamma);
    s.cancel_all();
}

// ============================================================================
// Job callback exception safety
// ============================================================================

TEST(Scheduler, ExceptionInJobDoesNotCrash) {
    Scheduler s;
    s.start();

    std::atomic<int> count{0};
    s.add_job("thrower", 20, [&]() {
        ++count;
        throw std::runtime_error("simulated error");
    });
    std::this_thread::sleep_for(100ms);
    s.stop();

    // Scheduler should survive the exceptions and keep re-firing
    EXPECT_GE(count.load(), 2);
}

// ============================================================================
// Now_ns utility
// ============================================================================

TEST(Scheduler, NowNsIsMonotonic) {
    int64_t t1 = Scheduler::now_ns();
    std::this_thread::sleep_for(1ms);
    int64_t t2 = Scheduler::now_ns();
    EXPECT_GT(t2, t1);
    // ~1ms gap in nanoseconds
    EXPECT_GE(t2 - t1, 500'000LL);
}

TEST(Scheduler, NowNsReasonableEpoch) {
    int64_t ns = Scheduler::now_ns();
    // Sanity: 2024-01-01 in ns
    EXPECT_GT(ns, 1'700'000'000'000'000'000LL);
}

// ============================================================================
// Add job while running
// ============================================================================

TEST(Scheduler, AddJobWhileRunning) {
    Scheduler s;
    s.start();
    std::this_thread::sleep_for(20ms);  // let thread start

    std::atomic<int> count{0};
    s.add_job("late", 20, [&]() { ++count; });
    std::this_thread::sleep_for(100ms);
    s.stop();

    EXPECT_GE(count.load(), 2);
}

// ============================================================================
// Scheduler: stop with no jobs (should not hang)
// ============================================================================

TEST(Scheduler, StopWithNoJobs) {
    Scheduler s;
    s.start();
    std::this_thread::sleep_for(10ms);
    s.stop();  // must not hang
    EXPECT_FALSE(s.is_running());
}
