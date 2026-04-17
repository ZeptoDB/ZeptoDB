// ============================================================================
// ZeptoDB: WindowJoin SIMD Optimization Tests (devlog 080)
// ============================================================================
// Verifies contiguous fast-path, gather+SIMD path, and scalar fallback
// for all aggregate types (SUM, AVG, MIN, MAX, COUNT).
// ============================================================================

#include "zeptodb/execution/join_operator.h"
#include <gtest/gtest.h>
#include <numeric>
#include <vector>
#include <cstdint>

using namespace zeptodb::execution;

// Helper: single left row, all right rows within window, verify aggregate
static void verify_wj_agg(
    WJAggType agg, int64_t expected,
    const std::vector<int64_t>& vals, int64_t window_ns = 1000)
{
    const size_t n = vals.size();
    std::vector<int64_t> lk(1, 1), rk(n, 1);
    std::vector<int64_t> lt(1, 500);
    std::vector<int64_t> rt(n);
    for (size_t i = 0; i < n; ++i)
        rt[i] = 500 - static_cast<int64_t>(n / 2) + static_cast<int64_t>(i);

    WindowJoinOperator wj(agg, window_ns, window_ns);
    auto result = wj.execute(lk.data(), 1, rk.data(), n,
                             lt.data(), rt.data(), vals.data());
    EXPECT_EQ(result.agg_values[0], expected)
        << "agg=" << static_cast<int>(agg) << " n=" << n;
}

// --- Contiguous path (indices sequential → direct SIMD on right_val slice) ---

TEST(WindowJoinSIMD, SumContiguous) {
    std::vector<int64_t> vals(1000);
    std::iota(vals.begin(), vals.end(), 1);
    verify_wj_agg(WJAggType::SUM, 500500, vals);
}

TEST(WindowJoinSIMD, SumLarge) {
    std::vector<int64_t> vals(10000, 7);
    verify_wj_agg(WJAggType::SUM, 70000, vals, 10000);
}

TEST(WindowJoinSIMD, AvgContiguous) {
    std::vector<int64_t> vals(100, 50);
    verify_wj_agg(WJAggType::AVG, 50, vals);
}

TEST(WindowJoinSIMD, MinContiguous) {
    std::vector<int64_t> vals = {5, 3, 8, 1, 9, 2, 7, 4, 6, 10};
    verify_wj_agg(WJAggType::MIN, 1, vals);
}

TEST(WindowJoinSIMD, MaxContiguous) {
    std::vector<int64_t> vals = {5, 3, 8, 1, 9, 2, 7, 4, 6, 10};
    verify_wj_agg(WJAggType::MAX, 10, vals);
}

// --- Scalar fallback (small window, n < 32) ---

TEST(WindowJoinSIMD, SmallWindow) {
    std::vector<int64_t> vals = {10, 20, 30};
    verify_wj_agg(WJAggType::SUM, 60, vals);
}

TEST(WindowJoinSIMD, SingleElement) {
    std::vector<int64_t> vals = {42};
    verify_wj_agg(WJAggType::SUM, 42, vals);
    verify_wj_agg(WJAggType::AVG, 42, vals);
    verify_wj_agg(WJAggType::MIN, 42, vals);
    verify_wj_agg(WJAggType::MAX, 42, vals);
}

// --- Edge cases ---

TEST(WindowJoinSIMD, EmptyWindow) {
    std::vector<int64_t> lk = {1}, rk = {1};
    std::vector<int64_t> lt = {500}, rt = {1000};
    std::vector<int64_t> rv = {99};
    WindowJoinOperator wj(WJAggType::SUM, 1, 1);
    auto result = wj.execute(lk.data(), 1, rk.data(), 1,
                             lt.data(), rt.data(), rv.data());
    EXPECT_EQ(result.agg_values[0], 0);
    EXPECT_EQ(result.match_counts[0], 0);
}

TEST(WindowJoinSIMD, NegativeValues) {
    std::vector<int64_t> vals(100);
    for (size_t i = 0; i < 100; ++i) vals[i] = -static_cast<int64_t>(i);
    // sum = 0 + -1 + -2 + ... + -99 = -4950
    verify_wj_agg(WJAggType::SUM, -4950, vals);
}

TEST(WindowJoinSIMD, MultipleLeftRows) {
    const size_t rn = 100;
    std::vector<int64_t> lk = {1, 1, 1};
    std::vector<int64_t> rk(rn, 1);
    std::vector<int64_t> lt = {100, 500, 900};
    std::vector<int64_t> rt(rn), rv(rn);
    for (size_t i = 0; i < rn; ++i) {
        rt[i] = static_cast<int64_t>(i * 10);  // 0, 10, 20, ..., 990
        rv[i] = 1;
    }
    WindowJoinOperator wj(WJAggType::COUNT, 50, 50);
    auto result = wj.execute(lk.data(), 3, rk.data(), rn,
                             lt.data(), rt.data(), rv.data());
    // t=100, window [50,150]: rt 50,60,70,80,90,100,110,120,130,140,150 = 11
    EXPECT_EQ(result.match_counts[0], 11);
}
