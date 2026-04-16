// ============================================================================
// ZeptoDB: Composite Index (Index Intersection) Tests
// ============================================================================
// Tests for composite index intersection that combines s#, g#, p# indexes.
// IndexResult is private, so we test its behavior through SQL queries that
// exercise range-range, range-set, set-set, and empty intersection paths.
// ============================================================================

#include "zeptodb/sql/executor.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/storage/column_store.h"

#include <gtest/gtest.h>
#include <vector>

using namespace zeptodb::sql;
using namespace zeptodb::storage;
using namespace zeptodb::core;

// ============================================================================
// IndexResult behavior tests (via SQL queries)
// ============================================================================
// These test the IndexResult struct operations indirectly by constructing
// partitions with specific index attributes and verifying query results.

class IndexResultTest : public ::testing::Test {
protected:
    void SetUp() override {
        PipelineConfig cfg;
        cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
        pipeline = std::make_unique<ZeptoPipeline>(cfg);
        executor = std::make_unique<QueryExecutor>(*pipeline);

        auto& pm = pipeline->partition_manager();
        auto& part = pm.get_or_create(1, 1000LL);
        auto& ts  = part.add_column("timestamp",  ColumnType::TIMESTAMP_NS);
        auto& pr  = part.add_column("price",      ColumnType::INT64);
        auto& vol = part.add_column("volume",     ColumnType::INT64);
        auto& ex  = part.add_column("exchange",   ColumnType::INT64);
        auto& ot  = part.add_column("order_type", ColumnType::INT64);
        auto& mt  = part.add_column("msg_type",   ColumnType::INT32);

        // 100 rows: ts 1000..1099, price 10000..10099
        // exchange: [0..49]=1, [50..99]=2 (parted)
        // order_type: i%5 (grouped)
        for (int i = 0; i < 100; ++i) {
            ts.append<int64_t>(1000LL + i);
            pr.append<int64_t>(10000LL + i);
            vol.append<int64_t>(100LL + i);
            ex.append<int64_t>(i < 50 ? 1LL : 2LL);
            ot.append<int64_t>(static_cast<int64_t>(i % 5));
            mt.append<int32_t>(0);
        }
        part.set_sorted("timestamp");
        part.set_sorted("price");
        part.set_parted("exchange");
        part.set_grouped("order_type");
    }

    std::unique_ptr<ZeptoPipeline> pipeline;
    std::unique_ptr<QueryExecutor> executor;
};

// Range ∩ Range: two s# sorted ranges
TEST_F(IndexResultTest, RangeIntersect) {
    // timestamp [1010,1030] → rows 10..30 (21 rows)
    // price [10015,10025] → rows 15..25 (11 rows)
    // Intersection: rows 15..25 → 11 rows
    auto r = executor->execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1010 AND 1030 "
        "AND price BETWEEN 10015 AND 10025");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 11);
}

// Range ∩ Set: s# range + g# set
TEST_F(IndexResultTest, RangeSetIntersect) {
    // timestamp [1000,1049] → rows 0..49 (50 rows)
    // order_type = 0 → rows 0,5,10,...,95 (20 total), within 0..49 → 10 rows
    auto r = executor->execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1000 AND 1049 AND order_type = 0");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 10);
}

// Set ∩ Set: two g# lookups (need a second grouped column)
TEST_F(IndexResultTest, SetSetIntersect) {
    // We have exchange as p# and order_type as g#.
    // To test set∩set, add a second grouped column.
    auto& pm = pipeline->partition_manager();
    auto parts = pm.get_partitions_for_symbol(1);
    ASSERT_FALSE(parts.empty());
    auto& part = *parts[0];

    // Add a second grouped column: side = i%3
    auto& side = part.add_column("side", ColumnType::INT64);
    for (int i = 0; i < 100; ++i) {
        side.append<int64_t>(static_cast<int64_t>(i % 3));
    }
    part.set_grouped("side");

    // order_type=0 → rows {0,5,10,...,95} (20 rows)
    // side=0 → rows {0,3,6,...,99} (34 rows)
    // Intersection: rows where i%5==0 AND i%3==0 → i%15==0 → {0,15,30,45,60,75,90} = 7 rows
    auto r = executor->execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND order_type = 0 AND side = 0");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 7);
}

// Non-overlapping ranges → empty result
TEST_F(IndexResultTest, EmptyIntersect) {
    // timestamp [1000,1005] → rows 0..5, exchange=2 → rows 50..99
    // Range intersection is empty
    auto r = executor->execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1000 AND 1005 AND exchange = 2");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 0);
}

// Single predicate — same as old single-index behavior
TEST_F(IndexResultTest, SingleIndex) {
    auto r = executor->execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1010 AND 1019");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 10);
}

// ============================================================================
// End-to-end composite index tests
// ============================================================================

class CompositeIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        PipelineConfig cfg;
        cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
        pipeline = std::make_unique<ZeptoPipeline>(cfg);
        executor = std::make_unique<QueryExecutor>(*pipeline);

        auto& pm = pipeline->partition_manager();
        auto& part = pm.get_or_create(1, 1000LL);
        auto& ts  = part.add_column("timestamp",  ColumnType::TIMESTAMP_NS);
        auto& pr  = part.add_column("price",      ColumnType::INT64);
        auto& vol = part.add_column("volume",     ColumnType::INT64);
        auto& ex  = part.add_column("exchange",   ColumnType::INT64);
        auto& ot  = part.add_column("order_type", ColumnType::INT64);
        auto& mt  = part.add_column("msg_type",   ColumnType::INT32);

        for (int i = 0; i < 100; ++i) {
            ts.append<int64_t>(1000LL + i);
            pr.append<int64_t>(10000LL + i);
            vol.append<int64_t>(100LL + i);
            ex.append<int64_t>(i < 50 ? 1LL : 2LL);
            ot.append<int64_t>(static_cast<int64_t>(i % 5));
            mt.append<int32_t>(0);
        }
        part.set_sorted("timestamp");
        part.set_sorted("price");
        part.set_parted("exchange");
        part.set_grouped("order_type");
    }

    std::unique_ptr<ZeptoPipeline> pipeline;
    std::unique_ptr<QueryExecutor> executor;
};

// s# + p# end-to-end
TEST_F(CompositeIndexTest, TimestampPlusPart) {
    // timestamp [1020,1060] → rows 20..60, exchange=1 → rows 0..49
    // Intersection: rows 20..49 → 30 rows
    auto r = executor->execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1020 AND 1060 AND exchange = 1");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 30);
}

// s# + g# end-to-end
TEST_F(CompositeIndexTest, TimestampPlusGroup) {
    auto r = executor->execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1000 AND 1049 AND order_type = 0");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 10);
}

// s# + s# range intersection
TEST_F(CompositeIndexTest, TwoSorted) {
    auto r = executor->execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1010 AND 1030 "
        "AND price BETWEEN 10015 AND 10025");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 11);
}

// s# + p# + g# triple intersection
TEST_F(CompositeIndexTest, ThreeWay) {
    // timestamp [1000,1049] → rows 0..49
    // exchange=1 → rows 0..49
    // order_type=2 → rows 2,7,12,...,47 → 10 rows
    auto r = executor->execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1000 AND 1049 "
        "AND exchange = 1 AND order_type = 2");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 10);
}

// Single-index queries unchanged
TEST_F(CompositeIndexTest, NoRegression) {
    auto r1 = executor->execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1010 AND 1019");
    ASSERT_TRUE(r1.ok()) << r1.error;
    EXPECT_EQ(r1.rows[0][0], 10);

    auto r2 = executor->execute(
        "SELECT count(*) FROM trades WHERE symbol = 1 AND exchange = 2");
    ASSERT_TRUE(r2.ok()) << r2.error;
    EXPECT_EQ(r2.rows[0][0], 50);

    auto r3 = executor->execute(
        "SELECT count(*) FROM trades WHERE symbol = 1 AND order_type = 3");
    ASSERT_TRUE(r3.ok()) << r3.error;
    EXPECT_EQ(r3.rows[0][0], 20);
}

// GROUP BY with composite index
TEST_F(CompositeIndexTest, GroupByPath) {
    auto r = executor->execute(
        "SELECT order_type, count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1000 AND 1049 AND exchange = 1 "
        "GROUP BY order_type ORDER BY order_type");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 5u);
    for (auto& row : r.rows) {
        EXPECT_EQ(row[1], 10);
    }
}

// Parallel execution with composite index
TEST_F(CompositeIndexTest, ParallelPath) {
    executor->enable_parallel(4, 1);
    auto r = executor->execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1020 AND 1060 AND exchange = 1");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 30);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST(CompositeIndexEdge, EmptyPartition) {
    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    ZeptoPipeline pipeline(cfg);
    QueryExecutor exec(pipeline);

    auto& pm = pipeline.partition_manager();
    auto& part = pm.get_or_create(1, 1000LL);
    part.add_column("timestamp", ColumnType::TIMESTAMP_NS);
    part.add_column("price",     ColumnType::INT64);
    part.add_column("volume",    ColumnType::INT64);
    part.add_column("msg_type",  ColumnType::INT32);

    auto r = exec.execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1000 AND 2000");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 0);
}

TEST(CompositeIndexEdge, NoIndexedColumns) {
    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    ZeptoPipeline pipeline(cfg);
    QueryExecutor exec(pipeline);

    auto& pm = pipeline.partition_manager();
    auto& part = pm.get_or_create(1, 1000LL);
    auto& ts  = part.add_column("timestamp", ColumnType::TIMESTAMP_NS);
    auto& pr  = part.add_column("price",     ColumnType::INT64);
    auto& vol = part.add_column("volume",    ColumnType::INT64);
    auto& mt  = part.add_column("msg_type",  ColumnType::INT32);
    for (int i = 0; i < 10; ++i) {
        ts.append<int64_t>(1000LL + i);
        pr.append<int64_t>(100LL + i);
        vol.append<int64_t>(10LL + i);
        mt.append<int32_t>(0);
    }

    auto r = exec.execute(
        "SELECT count(*) FROM trades WHERE symbol = 1 AND price > 105");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 4);
}

TEST(CompositeIndexEdge, AllPredicatesIndexed) {
    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    ZeptoPipeline pipeline(cfg);
    QueryExecutor exec(pipeline);

    auto& pm = pipeline.partition_manager();
    auto& part = pm.get_or_create(1, 1000LL);
    auto& ts  = part.add_column("timestamp", ColumnType::TIMESTAMP_NS);
    auto& pr  = part.add_column("price",     ColumnType::INT64);
    auto& vol = part.add_column("volume",    ColumnType::INT64);
    auto& ex  = part.add_column("exchange",  ColumnType::INT64);
    auto& mt  = part.add_column("msg_type",  ColumnType::INT32);
    for (int i = 0; i < 20; ++i) {
        ts.append<int64_t>(1000LL + i);
        pr.append<int64_t>(100LL + i);
        vol.append<int64_t>(10LL + i);
        ex.append<int64_t>(i < 10 ? 1LL : 2LL);
        mt.append<int32_t>(0);
    }
    part.set_sorted("timestamp");
    part.set_parted("exchange");

    auto r = exec.execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1005 AND 1015 AND exchange = 1");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 5);
}

TEST(CompositeIndexEdge, EmptyIntersectionResult) {
    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    ZeptoPipeline pipeline(cfg);
    QueryExecutor exec(pipeline);

    auto& pm = pipeline.partition_manager();
    auto& part = pm.get_or_create(1, 1000LL);
    auto& ts  = part.add_column("timestamp", ColumnType::TIMESTAMP_NS);
    auto& pr  = part.add_column("price",     ColumnType::INT64);
    auto& vol = part.add_column("volume",    ColumnType::INT64);
    auto& ex  = part.add_column("exchange",  ColumnType::INT64);
    auto& mt  = part.add_column("msg_type",  ColumnType::INT32);
    for (int i = 0; i < 20; ++i) {
        ts.append<int64_t>(1000LL + i);
        pr.append<int64_t>(100LL + i);
        vol.append<int64_t>(10LL + i);
        ex.append<int64_t>(i < 10 ? 1LL : 2LL);
        mt.append<int32_t>(0);
    }
    part.set_sorted("timestamp");
    part.set_parted("exchange");

    auto r = exec.execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1000 AND 1005 AND exchange = 2");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 0);
}

TEST(CompositeIndexEdge, SingleRowPartition) {
    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    ZeptoPipeline pipeline(cfg);
    QueryExecutor exec(pipeline);

    auto& pm = pipeline.partition_manager();
    auto& part = pm.get_or_create(1, 5000LL);
    auto& ts  = part.add_column("timestamp", ColumnType::TIMESTAMP_NS);
    auto& pr  = part.add_column("price",     ColumnType::INT64);
    auto& vol = part.add_column("volume",    ColumnType::INT64);
    auto& mt  = part.add_column("msg_type",  ColumnType::INT32);
    ts.append<int64_t>(5000LL);
    pr.append<int64_t>(999LL);
    vol.append<int64_t>(1LL);
    mt.append<int32_t>(0);
    part.set_sorted("timestamp");

    auto r = exec.execute(
        "SELECT price FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 5000 AND 5000");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 999);
}
