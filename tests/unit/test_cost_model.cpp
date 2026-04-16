#include <gtest/gtest.h>
#include "zeptodb/execution/cost_model.h"

using namespace zeptodb::execution;

// ============================================================================
// Selectivity tests
// ============================================================================

TEST(CostModel, SelectivityEq) {
    ColumnStats cs;
    for (int64_t i = 0; i < 100; ++i) cs.update(i);

    double sel = CostModel::selectivity_eq(cs);
    // 1/distinct ≈ 1/100 = 0.01, allow HLL error
    EXPECT_GT(sel, 0.0);
    EXPECT_LE(sel, 1.0);
    EXPECT_LT(sel, 0.1);  // should be small for 100 distinct values
}

TEST(CostModel, SelectivityEqSingleDistinct) {
    ColumnStats cs;
    for (int i = 0; i < 100; ++i) cs.update(42);

    double sel = CostModel::selectivity_eq(cs);
    EXPECT_DOUBLE_EQ(sel, 1.0);  // 1 distinct → selectivity = 1.0
}

TEST(CostModel, SelectivityRange) {
    ColumnStats cs;
    cs.update(0);
    cs.update(100);

    double sel = CostModel::selectivity_range(cs, 20, 40);
    EXPECT_NEAR(sel, 0.2, 0.01);  // (40-20)/(100-0) = 0.2
}

TEST(CostModel, SelectivityRangeClamped) {
    ColumnStats cs;
    cs.update(0);
    cs.update(100);

    // Range wider than data
    double sel = CostModel::selectivity_range(cs, -50, 200);
    EXPECT_DOUBLE_EQ(sel, 1.0);  // clamped to 1.0
}

TEST(CostModel, SelectivityRangeSameMinMax) {
    ColumnStats cs;
    cs.update(42);

    double sel = CostModel::selectivity_range(cs, 40, 50);
    EXPECT_DOUBLE_EQ(sel, 1.0);  // max == min → 1.0
}

TEST(CostModel, SelectivityIn) {
    ColumnStats cs;
    for (int64_t i = 0; i < 100; ++i) cs.update(i);

    double sel = CostModel::selectivity_in(cs, 5);
    // 5/distinct ≈ 5/100 = 0.05
    EXPECT_GT(sel, 0.0);
    EXPECT_LE(sel, 1.0);
    EXPECT_LT(sel, 0.2);
}

TEST(CostModel, SelectivityAnd) {
    double a = 0.1, b = 0.2;
    EXPECT_NEAR(CostModel::selectivity_and(a, b), 0.02, 1e-9);
}

TEST(CostModel, SelectivityOr) {
    double a = 0.1, b = 0.2;
    // a + b - a*b = 0.1 + 0.2 - 0.02 = 0.28
    EXPECT_NEAR(CostModel::selectivity_or(a, b), 0.28, 1e-9);
}

// ============================================================================
// Cost comparison tests
// ============================================================================

TEST(CostModel, IndexVsSeqScanLowSelectivity) {
    // Low selectivity (few rows) → index scan preferred
    EXPECT_TRUE(CostModel::prefer_index_scan(1'000'000, 0.001));
    EXPECT_TRUE(CostModel::prefer_index_scan(1'000'000, 0.01));
}

TEST(CostModel, IndexVsSeqScanHighSelectivity) {
    // High selectivity (many rows) → seq scan preferred
    EXPECT_FALSE(CostModel::prefer_index_scan(1'000'000, 0.5));
    EXPECT_FALSE(CostModel::prefer_index_scan(1'000'000, 1.0));
}

TEST(CostModel, IndexScanCrossover) {
    // Find the crossover point — should be around 0.15-0.25
    bool low = CostModel::prefer_index_scan(100'000, 0.05);
    bool high = CostModel::prefer_index_scan(100'000, 0.50);
    EXPECT_TRUE(low);
    EXPECT_FALSE(high);
}

TEST(CostModel, HashJoinBuildSideSelection) {
    // Smaller table as build side should be cheaper
    auto small_build = CostModel::estimate_hash_join(1000, 1'000'000);
    auto large_build = CostModel::estimate_hash_join(1'000'000, 1000);

    EXPECT_LT(small_build.total(), large_build.total());
}

TEST(CostModel, SortCostMonotonic) {
    auto c1 = CostModel::estimate_sort(1000);
    auto c2 = CostModel::estimate_sort(10000);
    auto c3 = CostModel::estimate_sort(100000);

    EXPECT_LT(c1.total(), c2.total());
    EXPECT_LT(c2.total(), c3.total());
}

TEST(CostModel, AggregateCost) {
    auto c = CostModel::estimate_aggregate(10000, 100);
    EXPECT_GT(c.total(), 0.0);
    EXPECT_EQ(c.est_rows, 100u);  // output rows = group count
}

TEST(CostModel, SeqScanCost) {
    auto c = CostModel::estimate_seq_scan(10000, 5);
    EXPECT_DOUBLE_EQ(c.io_cost, 10000.0 * 5.0 * SEQ_COST);
    EXPECT_EQ(c.est_rows, 10000u);
}
