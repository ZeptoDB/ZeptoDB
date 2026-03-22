// ============================================================================
// APEX-DB: Hash Join + Window Function 테스트
// ============================================================================

#include "apex/execution/join_operator.h"
#include "apex/execution/window_function.h"
#include "apex/sql/tokenizer.h"
#include "apex/sql/parser.h"
#include "apex/storage/column_store.h"
#include "apex/storage/arena_allocator.h"

#include <gtest/gtest.h>
#include <vector>
#include <numeric>
#include <cstdint>
#include <climits>

using namespace apex::execution;
using namespace apex::sql;
using namespace apex::storage;

// ============================================================================
// Part 1: HashJoinOperator 테스트
// ============================================================================

// 1:1 매칭
TEST(HashJoin, OneToOne) {
    ArenaAllocator arena_l(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});
    ArenaAllocator arena_r(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});

    ColumnVector lk("id", ColumnType::INT64, arena_l);
    ColumnVector rk("id", ColumnType::INT64, arena_r);

    // 왼쪽: [1, 2, 3], 오른쪽: [2, 1, 3]
    lk.append<int64_t>(1); lk.append<int64_t>(2); lk.append<int64_t>(3);
    rk.append<int64_t>(2); rk.append<int64_t>(1); rk.append<int64_t>(3);

    HashJoinOperator hj;
    auto result = hj.execute(lk, rk);

    EXPECT_EQ(result.match_count, 3u);
    // 각 왼쪽 행이 정확히 하나의 오른쪽 행과 매칭되어야 함
    ASSERT_EQ(result.left_indices.size(), 3u);
    ASSERT_EQ(result.right_indices.size(), 3u);

    // 매칭 검증: left[0]=1 → right[1]=1 (인덱스 1)
    for (size_t i = 0; i < result.match_count; ++i) {
        int64_t li = result.left_indices[i];
        int64_t ri = result.right_indices[i];
        // 왼쪽 키와 오른쪽 키가 같아야 함
        const int64_t* ld = static_cast<const int64_t*>(lk.raw_data());
        const int64_t* rd = static_cast<const int64_t*>(rk.raw_data());
        EXPECT_EQ(ld[li], rd[ri]);
    }
}

// 1:N 매칭 (오른쪽에 중복 키)
TEST(HashJoin, OneToMany) {
    ArenaAllocator arena_l(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});
    ArenaAllocator arena_r(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});

    ColumnVector lk("symbol", ColumnType::INT64, arena_l);
    ColumnVector rk("symbol", ColumnType::INT64, arena_r);

    // 왼쪽: [1], 오른쪽: [1, 1, 1] → 3개 매칭
    lk.append<int64_t>(1);
    rk.append<int64_t>(1); rk.append<int64_t>(1); rk.append<int64_t>(1);

    HashJoinOperator hj;
    auto result = hj.execute(lk, rk);

    EXPECT_EQ(result.match_count, 3u);
    // 모두 왼쪽 인덱스 0
    for (size_t i = 0; i < result.match_count; ++i) {
        EXPECT_EQ(result.left_indices[i], 0);
    }
}

// N:M 매칭
TEST(HashJoin, ManyToMany) {
    ArenaAllocator arena_l(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});
    ArenaAllocator arena_r(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});

    ColumnVector lk("k", ColumnType::INT64, arena_l);
    ColumnVector rk("k", ColumnType::INT64, arena_r);

    // 왼쪽: [1, 1], 오른쪽: [1, 1] → 2*2=4 매칭
    lk.append<int64_t>(1); lk.append<int64_t>(1);
    rk.append<int64_t>(1); rk.append<int64_t>(1);

    HashJoinOperator hj;
    auto result = hj.execute(lk, rk);
    EXPECT_EQ(result.match_count, 4u);
}

// 매칭 없음
TEST(HashJoin, NoMatch) {
    ArenaAllocator arena_l(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});
    ArenaAllocator arena_r(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});

    ColumnVector lk("k", ColumnType::INT64, arena_l);
    ColumnVector rk("k", ColumnType::INT64, arena_r);

    lk.append<int64_t>(1); lk.append<int64_t>(2);
    rk.append<int64_t>(3); rk.append<int64_t>(4);

    HashJoinOperator hj;
    auto result = hj.execute(lk, rk);
    EXPECT_EQ(result.match_count, 0u);
    EXPECT_TRUE(result.left_indices.empty());
    EXPECT_TRUE(result.right_indices.empty());
}

// 빈 입력
TEST(HashJoin, EmptyInput) {
    ArenaAllocator arena(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});
    ColumnVector lk("k", ColumnType::INT64, arena);
    ColumnVector rk("k", ColumnType::INT64, arena);

    HashJoinOperator hj;
    auto result = hj.execute(lk, rk);
    EXPECT_EQ(result.match_count, 0u);
}

// 대규모: 1K 행 정확성
TEST(HashJoin, LargeCorrectness) {
    ArenaAllocator arena_l(ArenaConfig{.total_size = 4 << 20, .use_hugepages = false, .numa_node = -1});
    ArenaAllocator arena_r(ArenaConfig{.total_size = 4 << 20, .use_hugepages = false, .numa_node = -1});

    ColumnVector lk("k", ColumnType::INT64, arena_l);
    ColumnVector rk("k", ColumnType::INT64, arena_r);

    const int N = 1000;
    for (int i = 0; i < N; ++i) lk.append<int64_t>(i % 100);
    for (int i = 0; i < 100; ++i) rk.append<int64_t>(i);

    HashJoinOperator hj;
    auto result = hj.execute(lk, rk);

    // 왼쪽 1000행 * 오른쪽 1개씩 = 1000 매칭
    EXPECT_EQ(result.match_count, (size_t)N);

    // 모든 매칭 쌍의 키가 같아야 함
    const int64_t* ld = static_cast<const int64_t*>(lk.raw_data());
    const int64_t* rd = static_cast<const int64_t*>(rk.raw_data());
    for (size_t i = 0; i < result.match_count; ++i) {
        EXPECT_EQ(ld[result.left_indices[i]], rd[result.right_indices[i]]);
    }
}

// ============================================================================
// Part 2: WindowFunction 테스트
// ============================================================================

// ── ROW_NUMBER ──

TEST(WindowFunction, RowNumber_Simple) {
    const size_t n = 5;
    int64_t input[n]  = {10, 20, 30, 40, 50};
    int64_t output[n] = {};
    WindowFrame frame;

    WindowRowNumber wf;
    wf.compute(input, n, output, frame);

    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(output[i], static_cast<int64_t>(i + 1));
    }
}

TEST(WindowFunction, RowNumber_WithPartition) {
    const size_t n = 6;
    int64_t input[n]        = {1, 2, 3, 4, 5, 6};
    int64_t part_keys[n]    = {1, 1, 1, 2, 2, 2};
    int64_t output[n]       = {};
    WindowFrame frame;

    WindowRowNumber wf;
    wf.compute(input, n, output, frame, part_keys);

    // 파티션 1: 1, 2, 3 / 파티션 2: 1, 2, 3
    EXPECT_EQ(output[0], 1); EXPECT_EQ(output[1], 2); EXPECT_EQ(output[2], 3);
    EXPECT_EQ(output[3], 1); EXPECT_EQ(output[4], 2); EXPECT_EQ(output[5], 3);
}

// ── RANK ──

TEST(WindowFunction, Rank_WithTies) {
    const size_t n = 5;
    // 정렬된 값: 10, 10, 20, 20, 30 → rank: 1, 1, 3, 3, 5
    int64_t input[n]  = {10, 10, 20, 20, 30};
    int64_t output[n] = {};
    WindowFrame frame;

    WindowRank wf;
    wf.compute(input, n, output, frame);

    EXPECT_EQ(output[0], 1);
    EXPECT_EQ(output[1], 1);
    EXPECT_EQ(output[2], 3);
    EXPECT_EQ(output[3], 3);
    EXPECT_EQ(output[4], 5);
}

// ── DENSE_RANK ──

TEST(WindowFunction, DenseRank_WithTies) {
    const size_t n = 5;
    int64_t input[n]  = {10, 10, 20, 20, 30};
    int64_t output[n] = {};
    WindowFrame frame;

    WindowDenseRank wf;
    wf.compute(input, n, output, frame);

    EXPECT_EQ(output[0], 1);
    EXPECT_EQ(output[1], 1);
    EXPECT_EQ(output[2], 2);
    EXPECT_EQ(output[3], 2);
    EXPECT_EQ(output[4], 3);
}

// ── SUM UNBOUNDED PRECEDING (누적 합) ──

TEST(WindowFunction, Sum_CumulativeSum) {
    const size_t n = 5;
    int64_t input[n]  = {10, 20, 30, 40, 50};
    int64_t output[n] = {};
    WindowFrame frame;
    frame.preceding = INT64_MAX; // UNBOUNDED PRECEDING
    frame.following = 0;

    WindowSum wf;
    wf.compute(input, n, output, frame);

    EXPECT_EQ(output[0], 10);
    EXPECT_EQ(output[1], 30);
    EXPECT_EQ(output[2], 60);
    EXPECT_EQ(output[3], 100);
    EXPECT_EQ(output[4], 150);
}

// ── SUM with ROWS N PRECEDING (슬라이딩 윈도우) ──

TEST(WindowFunction, Sum_SlidingWindow) {
    const size_t n = 6;
    int64_t input[n]  = {1, 2, 3, 4, 5, 6};
    int64_t output[n] = {};
    WindowFrame frame;
    frame.preceding = 2; // ROWS 2 PRECEDING
    frame.following = 0;

    WindowSum wf;
    wf.compute(input, n, output, frame);

    // output[0] = 1
    // output[1] = 1+2 = 3
    // output[2] = 1+2+3 = 6
    // output[3] = 2+3+4 = 9
    // output[4] = 3+4+5 = 12
    // output[5] = 4+5+6 = 15
    EXPECT_EQ(output[0], 1);
    EXPECT_EQ(output[1], 3);
    EXPECT_EQ(output[2], 6);
    EXPECT_EQ(output[3], 9);
    EXPECT_EQ(output[4], 12);
    EXPECT_EQ(output[5], 15);
}

// ── SUM with PARTITION BY ──

TEST(WindowFunction, Sum_PartitionBy) {
    const size_t n = 6;
    int64_t input[n]     = {1, 2, 3, 4, 5, 6};
    int64_t part_keys[n] = {1, 1, 1, 2, 2, 2};
    int64_t output[n]    = {};
    WindowFrame frame;
    frame.preceding = INT64_MAX; // UNBOUNDED PRECEDING (cumulative)
    frame.following = 0;

    WindowSum wf;
    wf.compute(input, n, output, frame, part_keys);

    // 파티션 1: 1, 1+2=3, 1+2+3=6
    EXPECT_EQ(output[0], 1);
    EXPECT_EQ(output[1], 3);
    EXPECT_EQ(output[2], 6);
    // 파티션 2: 4, 4+5=9, 4+5+6=15
    EXPECT_EQ(output[3], 4);
    EXPECT_EQ(output[4], 9);
    EXPECT_EQ(output[5], 15);
}

// ── AVG with ROWS N PRECEDING ──

TEST(WindowFunction, Avg_SlidingWindow) {
    const size_t n = 5;
    // 롤링 2-period AVG
    int64_t input[n]  = {10, 20, 30, 40, 50};
    int64_t output[n] = {};
    WindowFrame frame;
    frame.preceding = 1; // ROWS 1 PRECEDING
    frame.following = 0;

    WindowAvg wf;
    wf.compute(input, n, output, frame);

    // output[0] = 10/1 = 10
    // output[1] = (10+20)/2 = 15
    // output[2] = (20+30)/2 = 25
    // output[3] = (30+40)/2 = 35
    // output[4] = (40+50)/2 = 45
    EXPECT_EQ(output[0], 10);
    EXPECT_EQ(output[1], 15);
    EXPECT_EQ(output[2], 25);
    EXPECT_EQ(output[3], 35);
    EXPECT_EQ(output[4], 45);
}

// ── AVG with PARTITION BY ──

TEST(WindowFunction, Avg_PartitionBy) {
    const size_t n = 4;
    int64_t input[n]     = {10, 20, 30, 40};
    int64_t part_keys[n] = {1, 1, 2, 2};
    int64_t output[n]    = {};
    WindowFrame frame;
    frame.preceding = INT64_MAX; // 전체 윈도우
    frame.following = 0;

    WindowAvg wf;
    wf.compute(input, n, output, frame, part_keys);

    // 파티션 1: [10, 20] → output=[10, 15]
    EXPECT_EQ(output[0], 10);
    EXPECT_EQ(output[1], 15);
    // 파티션 2: [30, 40] → output=[30, 35]
    EXPECT_EQ(output[2], 30);
    EXPECT_EQ(output[3], 35);
}

// ── MIN/MAX ──

TEST(WindowFunction, Min_Cumulative) {
    const size_t n = 5;
    int64_t input[n]  = {30, 10, 40, 20, 50};
    int64_t output[n] = {};
    WindowFrame frame;
    frame.preceding = INT64_MAX;
    frame.following = 0;

    WindowMin wf;
    wf.compute(input, n, output, frame);

    EXPECT_EQ(output[0], 30);
    EXPECT_EQ(output[1], 10);
    EXPECT_EQ(output[2], 10);
    EXPECT_EQ(output[3], 10);
    EXPECT_EQ(output[4], 10);
}

TEST(WindowFunction, Max_Cumulative) {
    const size_t n = 5;
    int64_t input[n]  = {30, 10, 40, 20, 50};
    int64_t output[n] = {};
    WindowFrame frame;
    frame.preceding = INT64_MAX;
    frame.following = 0;

    WindowMax wf;
    wf.compute(input, n, output, frame);

    EXPECT_EQ(output[0], 30);
    EXPECT_EQ(output[1], 30);
    EXPECT_EQ(output[2], 40);
    EXPECT_EQ(output[3], 40);
    EXPECT_EQ(output[4], 50);
}

// ── LAG ──

TEST(WindowFunction, Lag_Offset1) {
    const size_t n = 5;
    int64_t input[n]  = {100, 200, 300, 400, 500};
    int64_t output[n] = {};
    WindowFrame frame;

    WindowLag wf(1, 0); // offset=1, default=0
    wf.compute(input, n, output, frame);

    EXPECT_EQ(output[0], 0);    // 이전 행 없음 → default
    EXPECT_EQ(output[1], 100);
    EXPECT_EQ(output[2], 200);
    EXPECT_EQ(output[3], 300);
    EXPECT_EQ(output[4], 400);
}

TEST(WindowFunction, Lag_WithPartition) {
    const size_t n = 6;
    int64_t input[n]     = {10, 20, 30, 40, 50, 60};
    int64_t part_keys[n] = {1, 1, 1, 2, 2, 2};
    int64_t output[n]    = {};
    WindowFrame frame;

    WindowLag wf(1, -1); // default=-1
    wf.compute(input, n, output, frame, part_keys);

    // 파티션 1: [-1, 10, 20]
    EXPECT_EQ(output[0], -1);
    EXPECT_EQ(output[1], 10);
    EXPECT_EQ(output[2], 20);
    // 파티션 2: [-1, 40, 50]
    EXPECT_EQ(output[3], -1);
    EXPECT_EQ(output[4], 40);
    EXPECT_EQ(output[5], 50);
}

// ── LEAD ──

TEST(WindowFunction, Lead_Offset1) {
    const size_t n = 5;
    int64_t input[n]  = {100, 200, 300, 400, 500};
    int64_t output[n] = {};
    WindowFrame frame;

    WindowLead wf(1, 0);
    wf.compute(input, n, output, frame);

    EXPECT_EQ(output[0], 200);
    EXPECT_EQ(output[1], 300);
    EXPECT_EQ(output[2], 400);
    EXPECT_EQ(output[3], 500);
    EXPECT_EQ(output[4], 0);    // 다음 행 없음 → default
}

TEST(WindowFunction, Lead_WithPartition) {
    const size_t n = 6;
    int64_t input[n]     = {10, 20, 30, 40, 50, 60};
    int64_t part_keys[n] = {1, 1, 1, 2, 2, 2};
    int64_t output[n]    = {};
    WindowFrame frame;

    WindowLead wf(1, -1);
    wf.compute(input, n, output, frame, part_keys);

    // 파티션 1: [20, 30, -1]
    EXPECT_EQ(output[0], 20);
    EXPECT_EQ(output[1], 30);
    EXPECT_EQ(output[2], -1);
    // 파티션 2: [50, 60, -1]
    EXPECT_EQ(output[3], 50);
    EXPECT_EQ(output[4], 60);
    EXPECT_EQ(output[5], -1);
}

// ── O(n) 성능 확인: 대규모 SUM ──
TEST(WindowFunction, Sum_LargeN) {
    const size_t n = 100000;
    std::vector<int64_t> input(n, 1);
    std::vector<int64_t> output(n, 0);
    WindowFrame frame;
    frame.preceding = 19; // ROWS 19 PRECEDING (20-period sum)
    frame.following = 0;

    WindowSum wf;
    wf.compute(input.data(), n, output.data(), frame);

    // 처음 19개는 점차 증가, 이후 모두 20
    for (size_t i = 0; i < n; ++i) {
        int64_t expected = static_cast<int64_t>(std::min(i + 1, (size_t)20));
        EXPECT_EQ(output[i], expected) << " at i=" << i;
        if (output[i] != expected) break; // 첫 번째 실패에서 중단
    }
}

// ============================================================================
// Part 3: SQL Parser — 윈도우 함수 파싱 테스트
// ============================================================================

TEST(Parser, WindowFunction_RowNumber) {
    Parser p;
    // ROW_NUMBER() OVER (ORDER BY timestamp)
    auto stmt = p.parse(
        "SELECT price, ROW_NUMBER() OVER (ORDER BY timestamp) AS rn FROM trades");
    ASSERT_EQ(stmt.columns.size(), 2u);
    EXPECT_EQ(stmt.columns[1].window_func, WindowFunc::ROW_NUMBER);
    EXPECT_EQ(stmt.columns[1].alias, "rn");
    EXPECT_TRUE(stmt.columns[1].window_spec.has_value());
    EXPECT_EQ(stmt.columns[1].window_spec->order_by_cols.size(), 1u);
    EXPECT_EQ(stmt.columns[1].window_spec->order_by_cols[0], "timestamp");
}

TEST(Parser, WindowFunction_SumPartitionBy) {
    Parser p;
    auto stmt = p.parse(
        "SELECT symbol, SUM(volume) OVER (PARTITION BY symbol ORDER BY timestamp) AS cumvol FROM trades");
    ASSERT_EQ(stmt.columns.size(), 2u);
    EXPECT_EQ(stmt.columns[1].window_func, WindowFunc::SUM);
    EXPECT_EQ(stmt.columns[1].column, "volume");
    ASSERT_TRUE(stmt.columns[1].window_spec.has_value());
    EXPECT_EQ(stmt.columns[1].window_spec->partition_by_cols.size(), 1u);
    EXPECT_EQ(stmt.columns[1].window_spec->partition_by_cols[0], "symbol");
}

TEST(Parser, WindowFunction_AvgRowsPreceding) {
    Parser p;
    auto stmt = p.parse(
        "SELECT price, AVG(price) OVER (PARTITION BY symbol ORDER BY timestamp ROWS 20 PRECEDING) AS ma20 FROM trades");
    ASSERT_EQ(stmt.columns.size(), 2u);
    EXPECT_EQ(stmt.columns[1].window_func, WindowFunc::AVG);
    ASSERT_TRUE(stmt.columns[1].window_spec.has_value());
    EXPECT_TRUE(stmt.columns[1].window_spec->has_frame);
    EXPECT_EQ(stmt.columns[1].window_spec->preceding, 20);
    EXPECT_EQ(stmt.columns[1].window_spec->following, 0);
}

TEST(Parser, WindowFunction_Lag) {
    Parser p;
    auto stmt = p.parse(
        "SELECT price, LAG(price, 1) OVER (PARTITION BY symbol ORDER BY timestamp) AS prev_price FROM trades");
    ASSERT_EQ(stmt.columns.size(), 2u);
    EXPECT_EQ(stmt.columns[1].window_func, WindowFunc::LAG);
    EXPECT_EQ(stmt.columns[1].column, "price");
    EXPECT_EQ(stmt.columns[1].window_offset, 1);
}

TEST(Parser, WindowFunction_Rank) {
    Parser p;
    auto stmt = p.parse(
        "SELECT price, RANK() OVER (ORDER BY price DESC) AS price_rank FROM trades");
    ASSERT_EQ(stmt.columns.size(), 2u);
    EXPECT_EQ(stmt.columns[1].window_func, WindowFunc::RANK);
    ASSERT_TRUE(stmt.columns[1].window_spec.has_value());
    EXPECT_FALSE(stmt.columns[1].window_spec->order_by_asc[0]); // DESC
}

TEST(Parser, WindowFunction_DenseRank) {
    Parser p;
    auto stmt = p.parse(
        "SELECT price, DENSE_RANK() OVER (ORDER BY price) AS dr FROM trades");
    ASSERT_EQ(stmt.columns.size(), 2u);
    EXPECT_EQ(stmt.columns[1].window_func, WindowFunc::DENSE_RANK);
}

TEST(Parser, WindowFunction_Lead) {
    Parser p;
    auto stmt = p.parse(
        "SELECT price, LEAD(price, 2) OVER (ORDER BY timestamp) AS next2 FROM trades");
    ASSERT_EQ(stmt.columns.size(), 2u);
    EXPECT_EQ(stmt.columns[1].window_func, WindowFunc::LEAD);
    EXPECT_EQ(stmt.columns[1].window_offset, 2);
}

TEST(Parser, WindowFunction_RowsBetween) {
    Parser p;
    // ROWS BETWEEN 5 PRECEDING AND 5 FOLLOWING
    auto stmt = p.parse(
        "SELECT price, SUM(price) OVER (ROWS BETWEEN 5 PRECEDING AND 5 FOLLOWING) AS s FROM trades");
    ASSERT_EQ(stmt.columns.size(), 2u);
    ASSERT_TRUE(stmt.columns[1].window_spec.has_value());
    EXPECT_TRUE(stmt.columns[1].window_spec->has_frame);
    EXPECT_EQ(stmt.columns[1].window_spec->preceding, 5);
    EXPECT_EQ(stmt.columns[1].window_spec->following, 5);
}

TEST(Parser, WindowFunction_UnboundedPreceding) {
    Parser p;
    auto stmt = p.parse(
        "SELECT price, SUM(price) OVER (ROWS UNBOUNDED PRECEDING) AS s FROM trades");
    ASSERT_EQ(stmt.columns.size(), 2u);
    ASSERT_TRUE(stmt.columns[1].window_spec.has_value());
    EXPECT_TRUE(stmt.columns[1].window_spec->has_frame);
    EXPECT_EQ(stmt.columns[1].window_spec->preceding, INT64_MAX);
}

// ============================================================================
// Part 4: Tokenizer — 윈도우 키워드 테스트
// ============================================================================

TEST(Tokenizer, WindowKeywords) {
    Tokenizer tok;
    auto tokens = tok.tokenize("OVER PARTITION ROWS PRECEDING FOLLOWING UNBOUNDED CURRENT ROW");

    // 각 키워드가 올바른 타입으로 인식되어야 함
    EXPECT_EQ(tokens[0].type, TokenType::OVER);
    EXPECT_EQ(tokens[1].type, TokenType::PARTITION);
    EXPECT_EQ(tokens[2].type, TokenType::ROWS);
    EXPECT_EQ(tokens[3].type, TokenType::PRECEDING);
    EXPECT_EQ(tokens[4].type, TokenType::FOLLOWING);
    EXPECT_EQ(tokens[5].type, TokenType::UNBOUNDED);
    EXPECT_EQ(tokens[6].type, TokenType::CURRENT);
    EXPECT_EQ(tokens[7].type, TokenType::ROW);
}

TEST(Tokenizer, WindowFuncKeywords) {
    Tokenizer tok;
    auto tokens = tok.tokenize("RANK DENSE_RANK ROW_NUMBER LAG LEAD");

    EXPECT_EQ(tokens[0].type, TokenType::RANK);
    EXPECT_EQ(tokens[1].type, TokenType::DENSE_RANK);
    EXPECT_EQ(tokens[2].type, TokenType::ROW_NUMBER);
    EXPECT_EQ(tokens[3].type, TokenType::LAG);
    EXPECT_EQ(tokens[4].type, TokenType::LEAD);
}

// ============================================================================
// Part 5: make_window_function factory 테스트
// ============================================================================

TEST(WindowFunction, Factory) {
    // 기본 factory 확인
    auto wf1 = make_window_function("ROW_NUMBER");
    EXPECT_NE(wf1, nullptr);

    auto wf2 = make_window_function("SUM");
    EXPECT_NE(wf2, nullptr);

    auto wf3 = make_window_function("LAG", 2, -99);
    EXPECT_NE(wf3, nullptr);

    EXPECT_THROW(make_window_function("UNKNOWN_FUNC"), std::runtime_error);
}
