// ============================================================================
// ZeptoDB: MetricsCollector Tests
// ============================================================================
// Tests verify:
//   - O(1) circular buffer (no erase, no realloc)
//   - Memory hard limit enforcement
//   - Response limit (bounded API output)
//   - Since filter
//   - Background thread collection
//   - All stats fields captured
//   - JSON serialization
// ============================================================================

#include <gtest/gtest.h>
#include "zeptodb/server/metrics_collector.h"
#include <thread>
#include <chrono>

using namespace zeptodb::server;
using namespace zeptodb::core;

class MetricsCollectorTest : public ::testing::Test {
protected:
    PipelineStats stats;
};

// ── Basic capture ───────────────────────────────────────────────────────────

TEST_F(MetricsCollectorTest, CaptureNow_RecordsSnapshot) {
    stats.ticks_ingested.store(1000);
    stats.ticks_stored.store(990);
    stats.queries_executed.store(50);

    MetricsCollector mc(stats, std::chrono::milliseconds(1000), 100);
    mc.capture_now();

    EXPECT_EQ(mc.size(), 1);
    auto history = mc.get_history();
    ASSERT_EQ(history.size(), 1);
    EXPECT_EQ(history[0].ticks_ingested, 1000);
    EXPECT_EQ(history[0].ticks_stored, 990);
    EXPECT_EQ(history[0].queries_executed, 50);
    EXPECT_GT(history[0].timestamp_ms, 0);
}

TEST_F(MetricsCollectorTest, MultipleCaptures_AccumulateHistory) {
    stats.ticks_ingested.store(100);
    MetricsCollector mc(stats, std::chrono::milliseconds(1000), 100);

    mc.capture_now();
    stats.ticks_ingested.store(200);
    mc.capture_now();
    stats.ticks_ingested.store(300);
    mc.capture_now();

    auto history = mc.get_history();
    ASSERT_EQ(history.size(), 3);
    EXPECT_EQ(history[0].ticks_ingested, 100);
    EXPECT_EQ(history[1].ticks_ingested, 200);
    EXPECT_EQ(history[2].ticks_ingested, 300);
}

// ── Circular buffer eviction (O(1), no erase) ──────────────────────────────

TEST_F(MetricsCollectorTest, CircularBuffer_EvictsOldest) {
    MetricsCollector mc(stats, std::chrono::milliseconds(1000), 3);

    for (int i = 1; i <= 5; ++i) {
        stats.ticks_ingested.store(i * 100);
        mc.capture_now();
    }

    EXPECT_EQ(mc.size(), 3);
    auto history = mc.get_history();
    ASSERT_EQ(history.size(), 3);
    // Oldest surviving = 300, newest = 500
    EXPECT_EQ(history[0].ticks_ingested, 300);
    EXPECT_EQ(history[1].ticks_ingested, 400);
    EXPECT_EQ(history[2].ticks_ingested, 500);
}

TEST_F(MetricsCollectorTest, CircularBuffer_WrapAround_MultipleRounds) {
    MetricsCollector mc(stats, std::chrono::milliseconds(1000), 3);

    // Write 10 entries into capacity-3 buffer (3+ full wraps)
    for (int i = 1; i <= 10; ++i) {
        stats.ticks_ingested.store(i);
        mc.capture_now();
    }

    EXPECT_EQ(mc.size(), 3);
    auto history = mc.get_history();
    EXPECT_EQ(history[0].ticks_ingested, 8);
    EXPECT_EQ(history[2].ticks_ingested, 10);
}

// ── Memory hard limit ───────────────────────────────────────────────────────

TEST_F(MetricsCollectorTest, MemoryLimit_CapsCapacity) {
    MetricsConfig cfg;
    cfg.capacity = 10000;  // request 10K
    cfg.max_memory_bytes = sizeof(MetricsSnapshot) * 5;  // but only allow 5

    MetricsCollector mc(stats, cfg);
    EXPECT_EQ(mc.capacity(), 5);
    EXPECT_LE(mc.memory_bytes(), cfg.max_memory_bytes);
}

TEST_F(MetricsCollectorTest, MemoryLimit_DefaultIs256KB) {
    MetricsConfig cfg;
    cfg.capacity = 100000;  // absurdly large
    // default max_memory_bytes = 256KB

    MetricsCollector mc(stats, cfg);
    EXPECT_LE(mc.memory_bytes(), 256 * 1024);
}

// ── Response limit ──────────────────────────────────────────────────────────

TEST_F(MetricsCollectorTest, ResponseLimit_CapsOutput) {
    MetricsConfig cfg;
    cfg.capacity = 100;
    cfg.response_limit = 5;
    cfg.max_memory_bytes = 100 * sizeof(MetricsSnapshot);

    MetricsCollector mc(stats, cfg);
    for (int i = 0; i < 20; ++i) mc.capture_now();

    // Default limit = 5
    auto history = mc.get_history(0, 0);
    EXPECT_EQ(history.size(), 5);
}

TEST_F(MetricsCollectorTest, ResponseLimit_ExplicitOverride) {
    MetricsConfig cfg;
    cfg.capacity = 100;
    cfg.response_limit = 5;
    cfg.max_memory_bytes = 100 * sizeof(MetricsSnapshot);

    MetricsCollector mc(stats, cfg);
    for (int i = 0; i < 20; ++i) mc.capture_now();

    // Explicit limit = 3
    auto history = mc.get_history(0, 3);
    EXPECT_EQ(history.size(), 3);
}

// ── Since filter ────────────────────────────────────────────────────────────

TEST_F(MetricsCollectorTest, GetHistory_SinceFilter) {
    MetricsCollector mc(stats, std::chrono::milliseconds(1000), 100);

    mc.capture_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto ts_after_first = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    mc.capture_now();

    auto filtered = mc.get_history(ts_after_first);
    EXPECT_EQ(filtered.size(), 1);
}

// ── JSON serialization ──────────────────────────────────────────────────────

TEST_F(MetricsCollectorTest, ToJson_ValidFormat) {
    stats.ticks_ingested.store(42);
    stats.queries_executed.store(7);

    MetricsCollector mc(stats, std::chrono::milliseconds(1000), 100);
    mc.capture_now();

    auto json = MetricsCollector::to_json(mc.get_history());
    EXPECT_EQ(json.front(), '[');
    EXPECT_EQ(json.back(), ']');
    EXPECT_NE(json.find("\"ticks_ingested\":42"), std::string::npos);
    EXPECT_NE(json.find("\"queries_executed\":7"), std::string::npos);
}

TEST_F(MetricsCollectorTest, ToJson_EmptyArray) {
    EXPECT_EQ(MetricsCollector::to_json({}), "[]");
}

// ── Node ID ─────────────────────────────────────────────────────────────────

TEST_F(MetricsCollectorTest, NodeId_Preserved) {
    MetricsCollector mc(stats, std::chrono::milliseconds(1000), 100, 5);
    mc.capture_now();
    EXPECT_EQ(mc.get_history()[0].node_id, 5);
}

TEST_F(MetricsCollectorTest, SetNodeId_UpdatesFutureCaptures) {
    MetricsCollector mc(stats, std::chrono::milliseconds(1000), 100);
    mc.capture_now();
    EXPECT_EQ(mc.get_history()[0].node_id, 0);

    mc.set_node_id(7);
    mc.capture_now();
    auto history = mc.get_history();
    EXPECT_EQ(history[0].node_id, 0);
    EXPECT_EQ(history[1].node_id, 7);
}

// ── Background thread ───────────────────────────────────────────────────────

TEST_F(MetricsCollectorTest, BackgroundThread_CollectsAutomatically) {
    stats.ticks_ingested.store(999);

    MetricsCollector mc(stats, std::chrono::milliseconds(20), 100);
    mc.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    mc.stop();

    EXPECT_GE(mc.size(), 2);
    EXPECT_EQ(mc.get_history().back().ticks_ingested, 999);
}

// ── All fields ──────────────────────────────────────────────────────────────

TEST_F(MetricsCollectorTest, AllStatsFields_Captured) {
    stats.ticks_ingested.store(1);
    stats.ticks_stored.store(2);
    stats.ticks_dropped.store(3);
    stats.queries_executed.store(4);
    stats.total_rows_scanned.store(5);
    stats.partitions_created.store(6);
    stats.last_ingest_latency_ns.store(7);

    MetricsCollector mc(stats, std::chrono::milliseconds(1000), 100);
    mc.capture_now();

    auto s = mc.get_history()[0];
    EXPECT_EQ(s.ticks_ingested, 1);
    EXPECT_EQ(s.ticks_stored, 2);
    EXPECT_EQ(s.ticks_dropped, 3);
    EXPECT_EQ(s.queries_executed, 4);
    EXPECT_EQ(s.total_rows_scanned, 5);
    EXPECT_EQ(s.partitions_created, 6);
    EXPECT_EQ(s.last_ingest_latency_ns, 7);
}

// ── Zero allocation after init ──────────────────────────────────────────────

TEST_F(MetricsCollectorTest, NoReallocation_AfterInit) {
    MetricsCollector mc(stats, std::chrono::milliseconds(1000), 10);

    // capacity() is fixed at construction
    EXPECT_EQ(mc.capacity(), 10);

    // Fill and overflow — capacity stays the same
    for (int i = 0; i < 100; ++i) mc.capture_now();
    EXPECT_EQ(mc.capacity(), 10);
    EXPECT_EQ(mc.size(), 10);
}

// ── Long-running safety ─────────────────────────────────────────────────────

TEST_F(MetricsCollectorTest, LongRunning_MemoryStable) {
    MetricsCollector mc(stats, std::chrono::milliseconds(1000), 5);
    size_t mem_before = mc.memory_bytes();

    // Simulate 10,000 captures (would be ~8 hours at 3s interval)
    for (int i = 0; i < 10000; ++i) {
        stats.ticks_ingested.store(i);
        mc.capture_now();
    }

    // Memory unchanged, size capped
    EXPECT_EQ(mc.memory_bytes(), mem_before);
    EXPECT_EQ(mc.size(), 5);
    EXPECT_EQ(mc.capacity(), 5);

    // Latest data is correct
    auto history = mc.get_history();
    EXPECT_EQ(history.back().ticks_ingested, 9999);
}
