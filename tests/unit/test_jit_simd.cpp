// ============================================================================
// Test: JIT SIMD Emit (compile_simd — explicit <4 x i64> vector IR)
// ============================================================================

#include "zeptodb/execution/jit_engine.h"
#include <gtest/gtest.h>
#include <vector>
#include <cstdint>

using namespace zeptodb::execution;

TEST(JITSIMDEmit, SimpleGt) {
    JITEngine jit;
    ASSERT_TRUE(jit.initialize());
    auto fn = jit.compile_simd("price > 100");
    ASSERT_NE(fn, nullptr);

    std::vector<int64_t> prices = {50, 101, 200, 99, 150, 100, 300, 0};
    std::vector<int64_t> volumes(8, 0);
    std::vector<uint32_t> out(8);
    size_t count = 0;
    fn(prices.data(), volumes.data(), 8, out.data(), &count);
    // 50≤100, 101>100✓, 200>100✓, 99≤100, 150>100✓, 100≤100, 300>100✓, 0≤100
    EXPECT_EQ(count, 4u);
    EXPECT_EQ(out[0], 1u);
    EXPECT_EQ(out[1], 2u);
    EXPECT_EQ(out[2], 4u);
    EXPECT_EQ(out[3], 6u);
}

TEST(JITSIMDEmit, AndCondition) {
    JITEngine jit;
    ASSERT_TRUE(jit.initialize());
    auto fn = jit.compile_simd("price > 100 AND volume > 50");
    ASSERT_NE(fn, nullptr);

    std::vector<int64_t> prices  = {200, 50, 150, 200};
    std::vector<int64_t> volumes = {100, 100, 30, 60};
    std::vector<uint32_t> out(4);
    size_t count = 0;
    fn(prices.data(), volumes.data(), 4, out.data(), &count);
    // [0]200,100✓ [1]50,100✗ [2]150,30✗ [3]200,60✓
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(out[0], 0u);
    EXPECT_EQ(out[1], 3u);
}

TEST(JITSIMDEmit, OrCondition) {
    JITEngine jit;
    ASSERT_TRUE(jit.initialize());
    auto fn = jit.compile_simd("price > 200 OR volume > 500");
    ASSERT_NE(fn, nullptr);

    std::vector<int64_t> prices  = {100, 300, 50, 10};
    std::vector<int64_t> volumes = {100, 100, 600, 10};
    std::vector<uint32_t> out(4);
    size_t count = 0;
    fn(prices.data(), volumes.data(), 4, out.data(), &count);
    // [0]neither✗ [1]p300>200✓ [2]v600>500✓ [3]neither✗
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(out[0], 1u);
    EXPECT_EQ(out[1], 2u);
}

TEST(JITSIMDEmit, Multiplier) {
    JITEngine jit;
    ASSERT_TRUE(jit.initialize());
    auto fn = jit.compile_simd("volume * 10 > 500");
    ASSERT_NE(fn, nullptr);

    std::vector<int64_t> prices(4, 0);
    std::vector<int64_t> volumes = {40, 51, 60, 49};
    std::vector<uint32_t> out(4);
    size_t count = 0;
    fn(prices.data(), volumes.data(), 4, out.data(), &count);
    // 400≤500, 510>500✓, 600>500✓, 490≤500
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(out[0], 1u);
    EXPECT_EQ(out[1], 2u);
}

TEST(JITSIMDEmit, ScalarTail) {
    JITEngine jit;
    ASSERT_TRUE(jit.initialize());
    auto fn = jit.compile_simd("price > 0");
    ASSERT_NE(fn, nullptr);

    std::vector<int64_t> prices = {1, 2, 3, 4, 5, 6, 7};  // 7 elements
    std::vector<int64_t> volumes(7, 0);
    std::vector<uint32_t> out(7);
    size_t count = 0;
    fn(prices.data(), volumes.data(), 7, out.data(), &count);
    EXPECT_EQ(count, 7u);
    for (uint32_t i = 0; i < 7; ++i) EXPECT_EQ(out[i], i);
}

TEST(JITSIMDEmit, EmptyInput) {
    JITEngine jit;
    ASSERT_TRUE(jit.initialize());
    auto fn = jit.compile_simd("price > 100");
    ASSERT_NE(fn, nullptr);

    size_t count = 99;
    fn(nullptr, nullptr, 0, nullptr, &count);
    EXPECT_EQ(count, 0u);
}

TEST(JITSIMDEmit, AllMatch) {
    JITEngine jit;
    ASSERT_TRUE(jit.initialize());
    auto fn = jit.compile_simd("price > 0");
    ASSERT_NE(fn, nullptr);

    std::vector<int64_t> prices(1024, 100);
    std::vector<int64_t> volumes(1024, 0);
    std::vector<uint32_t> out(1024);
    size_t count = 0;
    fn(prices.data(), volumes.data(), 1024, out.data(), &count);
    EXPECT_EQ(count, 1024u);
}

TEST(JITSIMDEmit, NoneMatch) {
    JITEngine jit;
    ASSERT_TRUE(jit.initialize());
    auto fn = jit.compile_simd("price > 1000");
    ASSERT_NE(fn, nullptr);

    std::vector<int64_t> prices(1024, 100);
    std::vector<int64_t> volumes(1024, 0);
    std::vector<uint32_t> out(1024);
    size_t count = 0;
    fn(prices.data(), volumes.data(), 1024, out.data(), &count);
    EXPECT_EQ(count, 0u);
}

// ============================================================================
// Edge case & dynamic correctness tests
// Verify compile_simd() produces identical results to compile_bulk() (oracle)
// ============================================================================

// Helper: run both bulk and simd, compare results
static void verify_simd_vs_bulk(
    const std::string& expr,
    const std::vector<int64_t>& prices,
    const std::vector<int64_t>& volumes,
    const char* label)
{
    JITEngine jit;
    ASSERT_TRUE(jit.initialize()) << label;

    auto bulk_fn = jit.compile_bulk(expr);
    auto simd_fn = jit.compile_simd(expr);
    ASSERT_NE(bulk_fn, nullptr) << label << " bulk compile failed: " << jit.last_error();
    ASSERT_NE(simd_fn, nullptr) << label << " simd compile failed: " << jit.last_error();

    size_t n = prices.size();
    std::vector<uint32_t> bulk_out(n), simd_out(n);
    size_t bulk_cnt = 0, simd_cnt = 0;

    bulk_fn(prices.data(), volumes.data(), static_cast<int64_t>(n),
            bulk_out.data(), reinterpret_cast<size_t*>(&bulk_cnt));
    simd_fn(prices.data(), volumes.data(), static_cast<int64_t>(n),
            simd_out.data(), reinterpret_cast<size_t*>(&simd_cnt));

    ASSERT_EQ(simd_cnt, bulk_cnt)
        << label << ": count mismatch for expr=\"" << expr << "\" n=" << n;
    for (size_t i = 0; i < simd_cnt; ++i) {
        EXPECT_EQ(simd_out[i], bulk_out[i])
            << label << ": index[" << i << "] mismatch for expr=\"" << expr << "\"";
    }
}

// --- Pure scalar tail (n < 4): vector loop never executes ---

TEST(JITSIMDEmit, PureScalarN1) {
    verify_simd_vs_bulk("price > 50", {100}, {0}, "n=1 match");
    verify_simd_vs_bulk("price > 50", {10}, {0}, "n=1 no match");
}

TEST(JITSIMDEmit, PureScalarN2) {
    verify_simd_vs_bulk("price > 50", {100, 10}, {0, 0}, "n=2");
}

TEST(JITSIMDEmit, PureScalarN3) {
    verify_simd_vs_bulk("price > 50", {100, 10, 200}, {0, 0, 0}, "n=3");
}

// --- Exact vector boundary (n = 4, 8, 12): no scalar tail ---

TEST(JITSIMDEmit, ExactN4) {
    verify_simd_vs_bulk("price > 50", {100, 10, 200, 30}, {0, 0, 0, 0}, "n=4");
}

TEST(JITSIMDEmit, ExactN8) {
    verify_simd_vs_bulk("price > 50",
        {100, 10, 200, 30, 60, 51, 49, 1000},
        {0, 0, 0, 0, 0, 0, 0, 0}, "n=8");
}

// --- Negative values ---

TEST(JITSIMDEmit, NegativeValues) {
    verify_simd_vs_bulk("price > -50",
        {-100, -49, 0, -50, 100, -51, -1, 50},
        {0, 0, 0, 0, 0, 0, 0, 0}, "negative prices");
}

TEST(JITSIMDEmit, NegativeThreshold) {
    verify_simd_vs_bulk("price < -10",
        {-100, -11, -10, -9, 0, 5, -20, -5},
        {0, 0, 0, 0, 0, 0, 0, 0}, "negative threshold LT");
}

TEST(JITSIMDEmit, NegativeMultiplier) {
    // volume * -1 > -50 means volume < 50
    verify_simd_vs_bulk("volume * -1 > -50",
        {0, 0, 0, 0, 0, 0, 0, 0},
        {49, 50, 51, 0, -1, 100, 30, 49}, "negative multiplier");
}

// --- All 6 comparison operators ---

TEST(JITSIMDEmit, OpGE) {
    verify_simd_vs_bulk("price >= 100",
        {99, 100, 101, 50, 100, 200, 0, 100},
        {0, 0, 0, 0, 0, 0, 0, 0}, "GE");
}

TEST(JITSIMDEmit, OpLT) {
    verify_simd_vs_bulk("price < 100",
        {99, 100, 101, 50, 100, 200, 0, 100},
        {0, 0, 0, 0, 0, 0, 0, 0}, "LT");
}

TEST(JITSIMDEmit, OpLE) {
    verify_simd_vs_bulk("price <= 100",
        {99, 100, 101, 50, 100, 200, 0, 100},
        {0, 0, 0, 0, 0, 0, 0, 0}, "LE");
}

TEST(JITSIMDEmit, OpEQ) {
    verify_simd_vs_bulk("price == 100",
        {99, 100, 101, 100, 0, 100, 200, 100},
        {0, 0, 0, 0, 0, 0, 0, 0}, "EQ");
}

TEST(JITSIMDEmit, OpNE) {
    verify_simd_vs_bulk("price != 100",
        {99, 100, 101, 100, 0, 100, 200, 100},
        {0, 0, 0, 0, 0, 0, 0, 0}, "NE");
}

// --- Complex compound expressions ---

TEST(JITSIMDEmit, TripleAnd) {
    verify_simd_vs_bulk("price > 50 AND price < 200 AND volume > 10",
        {100, 300, 60, 150, 40, 100, 199, 201},
        {20, 20, 5, 100, 20, 11, 11, 20}, "triple AND");
}

TEST(JITSIMDEmit, TripleOr) {
    verify_simd_vs_bulk("price > 900 OR price < 10 OR volume > 500",
        {5, 500, 950, 100, 8, 100, 1000, 50},
        {0, 0, 0, 600, 0, 501, 0, 0}, "triple OR");
}

TEST(JITSIMDEmit, MixedAndOr) {
    // OR has lower precedence: (price > 100 AND volume > 50) OR price > 500
    verify_simd_vs_bulk("price > 100 AND volume > 50 OR price > 500",
        {200, 50, 600, 150, 501, 100, 200, 10},
        {100, 100, 10, 30, 0, 51, 60, 0}, "AND+OR mixed");
}

TEST(JITSIMDEmit, MultiplierWithAnd) {
    verify_simd_vs_bulk("price * 2 > 300 AND volume > 0",
        {100, 151, 200, 149, 160, 50, 300, 0},
        {1, 1, 1, 1, 0, 1, 1, 1}, "multiplier+AND");
}

// --- Boundary values: INT64_MIN, INT64_MAX, 0 ---

TEST(JITSIMDEmit, BoundaryValues) {
    int64_t IMIN = INT64_MIN;
    int64_t IMAX = INT64_MAX;
    verify_simd_vs_bulk("price > 0",
        {IMIN, -1, 0, 1, IMAX, IMIN + 1, IMAX - 1, 0},
        {0, 0, 0, 0, 0, 0, 0, 0}, "boundary INT64");
}

TEST(JITSIMDEmit, ZeroThreshold) {
    verify_simd_vs_bulk("price >= 0",
        {-1, 0, 1, -100, 100, 0, -50, 50},
        {0, 0, 0, 0, 0, 0, 0, 0}, "zero threshold GE");
}

// --- Large dataset: stress test vector loop with many iterations ---

TEST(JITSIMDEmit, LargeDataset) {
    const size_t N = 100003;  // prime number, not multiple of 4
    std::vector<int64_t> prices(N), volumes(N);
    for (size_t i = 0; i < N; ++i) {
        prices[i] = static_cast<int64_t>(i % 200);   // 0..199 repeating
        volumes[i] = static_cast<int64_t>(i % 100);   // 0..99 repeating
    }
    verify_simd_vs_bulk("price > 150 AND volume > 30", prices, volumes, "large N=100003");
}

TEST(JITSIMDEmit, LargeAllMatch) {
    const size_t N = 8192;
    std::vector<int64_t> prices(N, 1000), volumes(N, 1000);
    verify_simd_vs_bulk("price > 0", prices, volumes, "large all-match N=8192");
}

TEST(JITSIMDEmit, LargeNoneMatch) {
    const size_t N = 8192;
    std::vector<int64_t> prices(N, 0), volumes(N, 0);
    verify_simd_vs_bulk("price > 999", prices, volumes, "large none-match N=8192");
}

// --- Volume-only expressions (price unused) ---

TEST(JITSIMDEmit, VolumeOnly) {
    verify_simd_vs_bulk("volume > 100",
        {0, 0, 0, 0, 0, 0, 0, 0},
        {50, 101, 200, 99, 150, 100, 300, 0}, "volume only");
}

// --- Alternating match pattern (worst case for branch prediction) ---

TEST(JITSIMDEmit, AlternatingPattern) {
    std::vector<int64_t> prices(16), volumes(16, 0);
    for (size_t i = 0; i < 16; ++i) prices[i] = (i % 2 == 0) ? 200 : 50;
    verify_simd_vs_bulk("price > 100", prices, volumes, "alternating");
}

// --- Single element in vector group matches ---

TEST(JITSIMDEmit, SingleMatchPerGroup) {
    // Each group of 4 has exactly 1 match at different positions
    verify_simd_vs_bulk("price > 100",
        {200, 50, 50, 50,   50, 200, 50, 50,   50, 50, 200, 50,   50, 50, 50, 200},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, "single match per group");
}
