#include <gtest/gtest.h>
#include "zeptodb/execution/table_statistics.h"

using namespace zeptodb::execution;
using namespace zeptodb::storage;

// ============================================================================
// ColumnStats tests
// ============================================================================

TEST(TableStatistics, ColumnStatsBasic) {
    ColumnStats cs;
    cs.update(10);
    cs.update(20);
    cs.update(30);

    EXPECT_EQ(cs.min_val, 10);
    EXPECT_EQ(cs.max_val, 30);
    EXPECT_EQ(cs.row_count, 3u);
    EXPECT_GT(cs.distinct_approx, 0u);
}

TEST(TableStatistics, ColumnStatsMinMaxSingleValue) {
    ColumnStats cs;
    cs.update(42);
    EXPECT_EQ(cs.min_val, 42);
    EXPECT_EQ(cs.max_val, 42);
    EXPECT_EQ(cs.row_count, 1u);
}

TEST(TableStatistics, ColumnStatsDistinctApprox) {
    ColumnStats cs;
    // Insert 100 distinct values
    for (int64_t i = 0; i < 100; ++i) {
        cs.update(i * 1000);
    }
    EXPECT_EQ(cs.row_count, 100u);
    // HLL with 64 registers: ~2-15% error for 100 distinct values
    // Allow wide range for small cardinality
    EXPECT_GE(cs.distinct_approx, 50u);
    EXPECT_LE(cs.distinct_approx, 200u);
}

TEST(TableStatistics, ColumnStatsDistinctDuplicates) {
    ColumnStats cs;
    // Insert same value 1000 times
    for (int i = 0; i < 1000; ++i) {
        cs.update(42);
    }
    EXPECT_EQ(cs.row_count, 1000u);
    EXPECT_EQ(cs.distinct_approx, 1u);
}

TEST(TableStatistics, ColumnStatsMerge) {
    ColumnStats a, b;
    a.update(10);
    a.update(20);
    b.update(5);
    b.update(30);

    a.merge(b);
    EXPECT_EQ(a.min_val, 5);
    EXPECT_EQ(a.max_val, 30);
    EXPECT_EQ(a.row_count, 4u);
}

TEST(TableStatistics, ColumnStatsMergeEmpty) {
    ColumnStats a, b;
    a.update(10);
    a.merge(b);  // merge empty
    EXPECT_EQ(a.min_val, 10);
    EXPECT_EQ(a.max_val, 10);
    EXPECT_EQ(a.row_count, 1u);
}

TEST(TableStatistics, ColumnStatsMergeDistinct) {
    ColumnStats a, b;
    for (int64_t i = 0; i < 50; ++i) a.update(i);
    for (int64_t i = 50; i < 100; ++i) b.update(i);

    a.merge(b);
    EXPECT_EQ(a.row_count, 100u);
    // Merged distinct should be roughly 100
    EXPECT_GE(a.distinct_approx, 50u);
    EXPECT_LE(a.distinct_approx, 200u);
}

// ============================================================================
// PartitionStats tests
// ============================================================================

TEST(TableStatistics, PartitionStatsMultiColumn) {
    PartitionStats ps;
    // Row 1: price=100, volume=500
    ps.record_row();
    ps.record_append("price", 100);
    ps.record_append("volume", 500);
    // Row 2: price=200, volume=300
    ps.record_row();
    ps.record_append("price", 200);
    ps.record_append("volume", 300);

    EXPECT_EQ(ps.column_stats["price"].min_val, 100);
    EXPECT_EQ(ps.column_stats["price"].max_val, 200);
    EXPECT_EQ(ps.column_stats["volume"].min_val, 300);
    EXPECT_EQ(ps.column_stats["volume"].max_val, 500);
    // row_count increments once per record_row() call
    EXPECT_EQ(ps.row_count, 2u);
}

TEST(TableStatistics, PartitionStatsSealFreezes) {
    PartitionStats ps;
    ps.record_row();
    ps.record_append("price", 100);
    ps.seal();
    EXPECT_TRUE(ps.sealed);

    // After seal, record_row and record_append are no-ops
    ps.record_row();
    ps.record_append("price", 999);
    EXPECT_EQ(ps.column_stats["price"].max_val, 100);
    EXPECT_EQ(ps.row_count, 1u);
}

TEST(TableStatistics, PartitionStatsTimestamp) {
    PartitionStats ps;
    ps.record_row();
    ps.record_append("timestamp", 1000);
    ps.record_row();
    ps.record_append("timestamp", 2000);
    ps.record_row();
    ps.record_append("timestamp", 1500);

    EXPECT_EQ(ps.ts_min, 1000);
    EXPECT_EQ(ps.ts_max, 2000);
}

// ============================================================================
// TableStatistics tests
// ============================================================================

TEST(TableStatistics, UpdateAndAggregate) {
    // Create two partitions with data
    ArenaConfig cfg{1024 * 1024, false, -1};
    auto arena1 = std::make_unique<ArenaAllocator>(cfg);
    auto arena2 = std::make_unique<ArenaAllocator>(cfg);

    PartitionKey key1{0, 1, 0};
    PartitionKey key2{0, 1, 3600000000000LL};

    Partition p1(key1, std::move(arena1));
    Partition p2(key2, std::move(arena2));

    auto& ts1 = p1.add_column("timestamp", ColumnType::TIMESTAMP_NS);
    auto& pr1 = p1.add_column("price", ColumnType::INT64);
    ts1.append<int64_t>(1000);
    ts1.append<int64_t>(2000);
    pr1.append<int64_t>(100);
    pr1.append<int64_t>(200);

    auto& ts2 = p2.add_column("timestamp", ColumnType::TIMESTAMP_NS);
    auto& pr2 = p2.add_column("price", ColumnType::INT64);
    ts2.append<int64_t>(3000);
    pr2.append<int64_t>(300);

    TableStatistics stats;
    stats.update_partition(&p1);
    stats.update_partition(&p2);

    EXPECT_EQ(stats.estimate_rows("trades"), 3u);

    auto agg = stats.aggregate("trades");
    EXPECT_EQ(agg.row_count, 3u);
    EXPECT_EQ(agg.column_stats["price"].min_val, 100);
    EXPECT_EQ(agg.column_stats["price"].max_val, 300);
    EXPECT_EQ(agg.column_stats["price"].row_count, 3u);
}

TEST(TableStatistics, EmptyPartition) {
    ArenaConfig cfg{1024 * 1024, false, -1};
    auto arena = std::make_unique<ArenaAllocator>(cfg);
    PartitionKey key{0, 1, 0};
    Partition p(key, std::move(arena));

    TableStatistics stats;
    stats.update_partition(&p);
    EXPECT_EQ(stats.estimate_rows("trades"), 0u);
}
