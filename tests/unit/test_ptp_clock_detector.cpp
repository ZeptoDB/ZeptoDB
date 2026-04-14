#include <gtest/gtest.h>
#include "zeptodb/cluster/ptp_clock_detector.h"

#include <thread>
#include <vector>

using namespace zeptodb::cluster;

// ============================================================================
// PtpClockDetector unit tests
// ============================================================================

// --- Status classification ---

TEST(PtpClockDetector, DefaultStatusIsUnavailable) {
    PtpClockDetector det;
    EXPECT_EQ(det.status(), PtpSyncStatus::UNAVAILABLE);
    EXPECT_EQ(det.offset_ns(), 0);
    EXPECT_FALSE(det.ptp_available());
}

TEST(PtpClockDetector, InjectSyncedOffset) {
    PtpClockDetector det({.max_offset_ns = 1000});
    det.inject_offset(500, true);
    EXPECT_EQ(det.status(), PtpSyncStatus::SYNCED);
    EXPECT_EQ(det.offset_ns(), 500);
    EXPECT_TRUE(det.ptp_available());
}

TEST(PtpClockDetector, InjectExactThreshold) {
    PtpClockDetector det({.max_offset_ns = 1000});
    det.inject_offset(1000, true);
    EXPECT_EQ(det.status(), PtpSyncStatus::SYNCED);
}

TEST(PtpClockDetector, InjectDegradedOffset) {
    PtpClockDetector det({.max_offset_ns = 1000});
    det.inject_offset(5000, true);  // 5x threshold → DEGRADED
    EXPECT_EQ(det.status(), PtpSyncStatus::DEGRADED);
    EXPECT_EQ(det.offset_ns(), 5000);
}

TEST(PtpClockDetector, InjectDegradedBoundary) {
    PtpClockDetector det({.max_offset_ns = 1000});
    det.inject_offset(10000, true);  // exactly 10x → DEGRADED
    EXPECT_EQ(det.status(), PtpSyncStatus::DEGRADED);
}

TEST(PtpClockDetector, InjectUnsyncOffset) {
    PtpClockDetector det({.max_offset_ns = 1000});
    det.inject_offset(10001, true);  // >10x threshold → UNSYNC
    EXPECT_EQ(det.status(), PtpSyncStatus::UNSYNC);
}

TEST(PtpClockDetector, InjectUnavailable) {
    PtpClockDetector det({.max_offset_ns = 1000});
    det.inject_offset(0, false);
    EXPECT_EQ(det.status(), PtpSyncStatus::UNAVAILABLE);
    EXPECT_FALSE(det.ptp_available());
}

TEST(PtpClockDetector, NegativeOffsetUsesAbsValue) {
    PtpClockDetector det({.max_offset_ns = 1000});
    det.inject_offset(-500, true);
    EXPECT_EQ(det.status(), PtpSyncStatus::SYNCED);
    EXPECT_EQ(det.offset_ns(), 500);
}

TEST(PtpClockDetector, NegativeOffsetDegraded) {
    PtpClockDetector det({.max_offset_ns = 100});
    det.inject_offset(-800, true);
    EXPECT_EQ(det.status(), PtpSyncStatus::DEGRADED);
    EXPECT_EQ(det.offset_ns(), 800);
}

// --- Strict mode ---

TEST(PtpClockDetector, StrictModeDisabledAlwaysAllows) {
    PtpClockDetector det({.max_offset_ns = 1000, .strict_mode = false});
    det.inject_offset(999999, true);  // huge offset
    EXPECT_TRUE(det.allow_distributed_asof());
}

TEST(PtpClockDetector, StrictModeEnabledAllowsSynced) {
    PtpClockDetector det({.max_offset_ns = 1000, .strict_mode = true});
    det.inject_offset(500, true);
    EXPECT_TRUE(det.allow_distributed_asof());
}

TEST(PtpClockDetector, StrictModeBlocksDegraded) {
    PtpClockDetector det({.max_offset_ns = 1000, .strict_mode = true});
    det.inject_offset(5000, true);
    EXPECT_FALSE(det.allow_distributed_asof());
}

TEST(PtpClockDetector, StrictModeBlocksUnsync) {
    PtpClockDetector det({.max_offset_ns = 1000, .strict_mode = true});
    det.inject_offset(50000, true);
    EXPECT_FALSE(det.allow_distributed_asof());
}

TEST(PtpClockDetector, StrictModeBlocksUnavailable) {
    PtpClockDetector det({.max_offset_ns = 1000, .strict_mode = true});
    // default status is UNAVAILABLE
    EXPECT_FALSE(det.allow_distributed_asof());
}

// --- Config mutation ---

TEST(PtpClockDetector, SetStrictMode) {
    PtpClockDetector det;
    EXPECT_FALSE(det.strict_mode());
    det.set_strict_mode(true);
    EXPECT_TRUE(det.strict_mode());
}

TEST(PtpClockDetector, SetMaxOffset) {
    PtpClockDetector det({.max_offset_ns = 1000});
    det.inject_offset(1500, true);
    EXPECT_EQ(det.status(), PtpSyncStatus::DEGRADED);

    det.set_max_offset_ns(2000);
    det.inject_offset(1500, true);  // re-inject to reclassify
    EXPECT_EQ(det.status(), PtpSyncStatus::SYNCED);
}

// --- JSON output ---

TEST(PtpClockDetector, ToJsonContainsFields) {
    PtpClockDetector det({.max_offset_ns = 1000, .strict_mode = true});
    det.inject_offset(500, true);
    auto json = det.to_json();
    EXPECT_NE(json.find("\"ptp_available\":true"), std::string::npos);
    EXPECT_NE(json.find("\"status\":\"SYNCED\""), std::string::npos);
    EXPECT_NE(json.find("\"offset_ns\":500"), std::string::npos);
    EXPECT_NE(json.find("\"strict_mode\":true"), std::string::npos);
    EXPECT_NE(json.find("\"max_offset_ns\":1000"), std::string::npos);
}

TEST(PtpClockDetector, ToJsonUnavailable) {
    PtpClockDetector det;
    auto json = det.to_json();
    EXPECT_NE(json.find("\"ptp_available\":false"), std::string::npos);
    EXPECT_NE(json.find("\"status\":\"UNAVAILABLE\""), std::string::npos);
}

// --- Status string ---

TEST(PtpClockDetector, StatusStrings) {
    EXPECT_STREQ(ptp_status_str(PtpSyncStatus::SYNCED), "SYNCED");
    EXPECT_STREQ(ptp_status_str(PtpSyncStatus::DEGRADED), "DEGRADED");
    EXPECT_STREQ(ptp_status_str(PtpSyncStatus::UNSYNC), "UNSYNC");
    EXPECT_STREQ(ptp_status_str(PtpSyncStatus::UNAVAILABLE), "UNAVAILABLE");
}

// --- Concurrent access ---

TEST(PtpClockDetector, ConcurrentInjectAndRead) {
    PtpClockDetector det({.max_offset_ns = 1000, .strict_mode = true});

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;

    // Writer threads
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&det, &stop, i]() {
            int64_t offset = (i + 1) * 200;
            while (!stop.load()) {
                det.inject_offset(offset, true);
            }
        });
    }

    // Reader threads
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&det, &stop]() {
            while (!stop.load()) {
                auto s = det.status();
                auto o = det.offset_ns();
                auto a = det.allow_distributed_asof();
                (void)s; (void)o; (void)a;
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true);
    for (auto& t : threads) t.join();

    // No crash = pass
    EXPECT_TRUE(det.ptp_available());
}

// --- Refresh on system without PTP (expected: UNAVAILABLE on CI) ---

TEST(PtpClockDetector, RefreshOnSystemWithoutPtp) {
    PtpClockDetector det({.max_offset_ns = 1000});
    det.refresh();
    // On most CI/dev machines, PTP hardware is not available
    // Just verify it doesn't crash and returns a valid status
    auto s = det.status();
    EXPECT_TRUE(s == PtpSyncStatus::SYNCED ||
                s == PtpSyncStatus::DEGRADED ||
                s == PtpSyncStatus::UNSYNC ||
                s == PtpSyncStatus::UNAVAILABLE);
}

// --- Zero threshold edge case ---

TEST(PtpClockDetector, ZeroThreshold) {
    PtpClockDetector det({.max_offset_ns = 0});
    det.inject_offset(0, true);
    EXPECT_EQ(det.status(), PtpSyncStatus::SYNCED);

    det.inject_offset(1, true);
    // 1 > 0 threshold, 1 <= 0*10=0 is false → UNSYNC (since 10x of 0 is 0)
    EXPECT_EQ(det.status(), PtpSyncStatus::UNSYNC);
}
