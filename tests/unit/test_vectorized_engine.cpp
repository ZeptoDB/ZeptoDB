// ============================================================================
// Test: Vectorized Execution Engine (Layer 3)
// ============================================================================

#include "zeptodb/execution/vectorized_engine.h"
#include <gtest/gtest.h>
#include <vector>
#include <numeric>

using namespace zeptodb::execution;

TEST(VectorizedEngine, FilterGtI64) {
    constexpr size_t N = 8192;
    std::vector<int64_t> prices(N);
    for (size_t i = 0; i < N; ++i) {
        prices[i] = static_cast<int64_t>(i * 10);
    }

    SelectionVector sel(N);
    filter_gt_i64(prices.data(), N, 50000, sel);

    // prices > 50000: indices 5001..8191 → 3191 rows
    EXPECT_EQ(sel.size(), 3191);
    EXPECT_EQ(sel[0], 5001);
}

TEST(VectorizedEngine, SumI64) {
    constexpr size_t N = 10000;
    std::vector<int64_t> data(N);
    std::iota(data.begin(), data.end(), 1); // 1..10000

    int64_t result = sum_i64(data.data(), N);
    // Sum 1..N = N*(N+1)/2
    EXPECT_EQ(result, static_cast<int64_t>(N) * (N + 1) / 2);
}

TEST(VectorizedEngine, SumI64Selected) {
    std::vector<int64_t> data = {10, 20, 30, 40, 50};
    SelectionVector sel(5);
    sel.add(1); // 20
    sel.add(3); // 40

    int64_t result = sum_i64_selected(data.data(), sel);
    EXPECT_EQ(result, 60);
}

TEST(VectorizedEngine, VWAP) {
    // Simple VWAP test
    // Price: 100, 200, 300 (fixed-point x10000)
    // Volume: 10, 20, 30
    // VWAP = (100*10 + 200*20 + 300*30) / (10+20+30) = 14000/60 = 233.33...
    std::vector<int64_t> prices = {100, 200, 300};
    std::vector<int64_t> volumes = {10, 20, 30};

    double result = vwap(prices.data(), volumes.data(), 3);
    EXPECT_NEAR(result, 233.333, 0.01);
}

TEST(VectorizedEngine, VWAPZeroVolume) {
    std::vector<int64_t> prices = {100, 200};
    std::vector<int64_t> volumes = {0, 0};

    double result = vwap(prices.data(), volumes.data(), 2);
    EXPECT_EQ(result, 0.0);
}
