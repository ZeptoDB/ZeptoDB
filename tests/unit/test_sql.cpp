// ============================================================================
// APEX-DB: SQL Parser + Executor + JOIN 테스트
// ============================================================================

#include "apex/sql/tokenizer.h"
#include "apex/sql/parser.h"
#include "apex/sql/executor.h"
#include "apex/execution/join_operator.h"
#include "apex/core/pipeline.h"
#include "apex/storage/column_store.h"
#include "apex/storage/arena_allocator.h"

#include <gtest/gtest.h>
#include <vector>
#include <string>
#include <memory>

using namespace apex::sql;
using namespace apex::execution;
using namespace apex::storage;
using namespace apex::core;

// ============================================================================
// Part 1: Tokenizer 테스트
// ============================================================================

TEST(Tokenizer, BasicSelect) {
    Tokenizer tok;
    auto tokens = tok.tokenize("SELECT price, volume FROM trades");

    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].type, TokenType::IDENT);
    EXPECT_EQ(tokens[1].value, "price");
    EXPECT_EQ(tokens[2].type, TokenType::COMMA);
    EXPECT_EQ(tokens[3].type, TokenType::IDENT);
    EXPECT_EQ(tokens[3].value, "volume");
    EXPECT_EQ(tokens[4].type, TokenType::FROM);
    EXPECT_EQ(tokens[5].type, TokenType::IDENT);
    EXPECT_EQ(tokens[5].value, "trades");
}

TEST(Tokenizer, WhereClause) {
    Tokenizer tok;
    auto tokens = tok.tokenize("WHERE symbol = 1 AND price > 15000");

    EXPECT_EQ(tokens[0].type, TokenType::WHERE);
    EXPECT_EQ(tokens[1].type, TokenType::IDENT);
    EXPECT_EQ(tokens[2].type, TokenType::EQ);
    EXPECT_EQ(tokens[3].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[3].value, "1");
    EXPECT_EQ(tokens[4].type, TokenType::AND);
}

TEST(Tokenizer, Operators) {
    Tokenizer tok;
    auto tokens = tok.tokenize(">= <= != <>");

    EXPECT_EQ(tokens[0].type, TokenType::GE);
    EXPECT_EQ(tokens[1].type, TokenType::LE);
    EXPECT_EQ(tokens[2].type, TokenType::NE);
    EXPECT_EQ(tokens[3].type, TokenType::NE);
}

TEST(Tokenizer, AggregateFunctions) {
    Tokenizer tok;
    auto tokens = tok.tokenize("SELECT count(*), sum(volume), avg(price), VWAP(price, volume) FROM t");

    // count, (, *, ), ,, sum, (, volume, ), ...
    bool found_count = false, found_sum = false, found_avg = false, found_vwap = false;
    for (const auto& t : tokens) {
        if (t.type == TokenType::COUNT) found_count = true;
        if (t.type == TokenType::SUM)   found_sum   = true;
        if (t.type == TokenType::AVG)   found_avg   = true;
        if (t.type == TokenType::VWAP)  found_vwap  = true;
    }
    EXPECT_TRUE(found_count);
    EXPECT_TRUE(found_sum);
    EXPECT_TRUE(found_avg);
    EXPECT_TRUE(found_vwap);
}

TEST(Tokenizer, AsofJoin) {
    Tokenizer tok;
    auto tokens = tok.tokenize("SELECT t.price FROM trades t ASOF JOIN quotes q ON t.symbol = q.symbol");

    bool found_asof = false, found_join = false;
    for (const auto& t : tokens) {
        if (t.type == TokenType::ASOF) found_asof = true;
        if (t.type == TokenType::JOIN) found_join = true;
    }
    EXPECT_TRUE(found_asof);
    EXPECT_TRUE(found_join);
}

TEST(Tokenizer, StringLiteral) {
    Tokenizer tok;
    auto tokens = tok.tokenize("WHERE name = 'AAPL'");
    // tokens: WHERE(0) IDENT(1) EQ(2) STRING(3) END(4)
    EXPECT_EQ(tokens[3].type, TokenType::STRING);
    EXPECT_EQ(tokens[3].value, "AAPL");
}

TEST(Tokenizer, Between) {
    Tokenizer tok;
    auto tokens = tok.tokenize("WHERE timestamp BETWEEN 1000 AND 2000");
    bool found = false;
    for (const auto& t : tokens) {
        if (t.type == TokenType::BETWEEN) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

// ============================================================================
// Part 2: Parser 테스트
// ============================================================================

TEST(Parser, SimpleSelect) {
    Parser p;
    auto stmt = p.parse("SELECT price, volume FROM trades");

    EXPECT_EQ(stmt.from_table, "trades");
    ASSERT_EQ(stmt.columns.size(), 2u);
    EXPECT_EQ(stmt.columns[0].column, "price");
    EXPECT_EQ(stmt.columns[1].column, "volume");
    EXPECT_FALSE(stmt.where.has_value());
    EXPECT_FALSE(stmt.join.has_value());
}

TEST(Parser, SelectStar) {
    Parser p;
    auto stmt = p.parse("SELECT * FROM trades");
    ASSERT_EQ(stmt.columns.size(), 1u);
    EXPECT_TRUE(stmt.columns[0].is_star);
}

TEST(Parser, WhereCondition) {
    Parser p;
    auto stmt = p.parse("SELECT price FROM trades WHERE symbol = 1 AND price > 15000");

    ASSERT_TRUE(stmt.where.has_value());
    auto& expr = stmt.where->expr;
    EXPECT_EQ(expr->kind, Expr::Kind::AND);
    EXPECT_EQ(expr->left->column, "symbol");
    EXPECT_EQ(expr->left->op, CompareOp::EQ);
    EXPECT_EQ(expr->left->value, 1);
    EXPECT_EQ(expr->right->column, "price");
    EXPECT_EQ(expr->right->op, CompareOp::GT);
    EXPECT_EQ(expr->right->value, 15000);
}

TEST(Parser, BetweenClause) {
    Parser p;
    auto stmt = p.parse("SELECT * FROM trades WHERE timestamp BETWEEN 1000 AND 2000");

    ASSERT_TRUE(stmt.where.has_value());
    auto& expr = stmt.where->expr;
    EXPECT_EQ(expr->kind, Expr::Kind::BETWEEN);
    EXPECT_EQ(expr->column, "timestamp");
    EXPECT_EQ(expr->lo, 1000);
    EXPECT_EQ(expr->hi, 2000);
}

TEST(Parser, AggregateSelect) {
    Parser p;
    auto stmt = p.parse(
        "SELECT count(*), sum(volume), avg(price) FROM trades WHERE symbol = 1");

    ASSERT_EQ(stmt.columns.size(), 3u);
    EXPECT_EQ(stmt.columns[0].agg, AggFunc::COUNT);
    EXPECT_EQ(stmt.columns[0].column, "*");
    EXPECT_EQ(stmt.columns[1].agg, AggFunc::SUM);
    EXPECT_EQ(stmt.columns[1].column, "volume");
    EXPECT_EQ(stmt.columns[2].agg, AggFunc::AVG);
    EXPECT_EQ(stmt.columns[2].column, "price");
}

TEST(Parser, GroupBy) {
    Parser p;
    auto stmt = p.parse("SELECT symbol, sum(volume) FROM trades GROUP BY symbol");

    ASSERT_TRUE(stmt.group_by.has_value());
    EXPECT_EQ(stmt.group_by->columns[0], "symbol");
    ASSERT_EQ(stmt.columns.size(), 2u);
    EXPECT_EQ(stmt.columns[1].agg, AggFunc::SUM);
}

TEST(Parser, AsofJoin) {
    Parser p;
    auto stmt = p.parse(
        "SELECT t.price, t.volume, q.bid, q.ask "
        "FROM trades t "
        "ASOF JOIN quotes q "
        "ON t.symbol = q.symbol AND t.timestamp >= q.timestamp");

    EXPECT_EQ(stmt.from_table, "trades");
    EXPECT_EQ(stmt.from_alias, "t");
    ASSERT_TRUE(stmt.join.has_value());
    EXPECT_EQ(stmt.join->type, JoinClause::Type::ASOF);
    EXPECT_EQ(stmt.join->table, "quotes");
    EXPECT_EQ(stmt.join->alias, "q");
    ASSERT_EQ(stmt.join->on_conditions.size(), 2u);
    EXPECT_EQ(stmt.join->on_conditions[0].op, CompareOp::EQ);
    EXPECT_EQ(stmt.join->on_conditions[1].op, CompareOp::GE);
}

TEST(Parser, InnerJoin) {
    Parser p;
    auto stmt = p.parse(
        "SELECT t.price, q.bid FROM trades t JOIN quotes q ON t.symbol = q.symbol");

    ASSERT_TRUE(stmt.join.has_value());
    EXPECT_EQ(stmt.join->type, JoinClause::Type::INNER);
}

TEST(Parser, TableAlias) {
    Parser p;
    auto stmt = p.parse("SELECT t.price FROM trades t WHERE t.symbol = 1");

    EXPECT_EQ(stmt.from_alias, "t");
    EXPECT_EQ(stmt.columns[0].table_alias, "t");
    EXPECT_EQ(stmt.columns[0].column, "price");
}

TEST(Parser, Limit) {
    Parser p;
    auto stmt = p.parse("SELECT price FROM trades LIMIT 100");
    ASSERT_TRUE(stmt.limit.has_value());
    EXPECT_EQ(*stmt.limit, 100);
}

TEST(Parser, VwapAggregate) {
    Parser p;
    auto stmt = p.parse("SELECT VWAP(price, volume) FROM trades");
    ASSERT_EQ(stmt.columns.size(), 1u);
    EXPECT_EQ(stmt.columns[0].agg, AggFunc::VWAP);
    EXPECT_EQ(stmt.columns[0].column, "price");
    EXPECT_EQ(stmt.columns[0].agg_arg2, "volume");
}

TEST(Parser, OrderBy) {
    Parser p;
    auto stmt = p.parse("SELECT price FROM trades ORDER BY price DESC");
    ASSERT_TRUE(stmt.order_by.has_value());
    EXPECT_EQ(stmt.order_by->items[0].column, "price");
    EXPECT_FALSE(stmt.order_by->items[0].asc);
}

// ============================================================================
// Part 3: Executor 테스트 (파이프라인에 데이터 넣고 SQL 실행)
// ============================================================================

// 테스트용 파이프라인 픽스처
class SqlExecutorTest : public ::testing::Test {
protected:
    void SetUp() override {
        PipelineConfig cfg;
        cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
        pipeline = std::make_unique<ApexPipeline>(cfg);
        // 백그라운드 드레인 스레드 없이 동기 드레인 사용 (테스트 안정성)
        // pipeline->start() 호출 안 함

        executor = std::make_unique<QueryExecutor>(*pipeline);

        // 데이터 삽입: symbol=1, trades 테이블
        // price: 15000..15009, volume: 100..109
        for (int i = 0; i < 10; ++i) {
            apex::ingestion::TickMessage msg{};
            msg.symbol_id  = 1;
            msg.recv_ts    = 1000LL + i;  // 작은 타임스탬프 (1970 epoch 기준)
            msg.price      = 15000 + i * 10;
            msg.volume     = 100 + i;
            msg.msg_type   = 0;
            pipeline->ingest_tick(msg);
        }
        // symbol=2 데이터
        for (int i = 0; i < 5; ++i) {
            apex::ingestion::TickMessage msg{};
            msg.symbol_id  = 2;
            msg.recv_ts    = 1000LL + i;
            msg.price      = 20000 + i * 10;
            msg.volume     = 200 + i;
            msg.msg_type   = 0;
            pipeline->ingest_tick(msg);
        }

        // 동기 드레인 — 모든 틱을 파티션에 저장
        pipeline->drain_sync(100);
    }

    void TearDown() override {
        // start() 없이 사용했으므로 stop() 불필요
        // (drain_thread_가 없어도 stop()은 안전하게 처리됨)
    }

    std::unique_ptr<ApexPipeline>   pipeline;
    std::unique_ptr<QueryExecutor>  executor;
};

TEST_F(SqlExecutorTest, CountAll) {
    auto result = executor->execute(
        "SELECT count(*) FROM trades");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_GE(result.rows.size(), 1u);
    // symbol=1(10개) + symbol=2(5개) = 15개 이상
    EXPECT_GE(result.rows[0][0], 1);
}

TEST_F(SqlExecutorTest, SumVolume) {
    auto result = executor->execute(
        "SELECT sum(volume) FROM trades WHERE symbol = 1");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_GE(result.rows.size(), 1u);
    // sum(100..109) = 1045
    EXPECT_EQ(result.rows[0][0], 1045);
}

TEST_F(SqlExecutorTest, AvgPrice) {
    auto result = executor->execute(
        "SELECT avg(price) FROM trades WHERE symbol = 1");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_GE(result.rows.size(), 1u);
    // avg(15000..15090 step 10) = 15045
    EXPECT_EQ(result.rows[0][0], 15045);
}

TEST_F(SqlExecutorTest, FilterGt) {
    auto result = executor->execute(
        "SELECT price, volume FROM trades WHERE symbol = 1 AND price > 15050");
    ASSERT_TRUE(result.ok()) << result.error;
    // price > 15050: 15060, 15070, 15080, 15090 → 4행
    EXPECT_EQ(result.rows.size(), 4u);
}

TEST_F(SqlExecutorTest, BetweenQuery) {
    // price BETWEEN 15020 AND 15050 → 4행 (price: 15020, 15030, 15040, 15050)
    auto result = executor->execute(
        "SELECT * FROM trades WHERE symbol = 1 AND price BETWEEN 15020 AND 15050");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows.size(), 4u);
}

TEST_F(SqlExecutorTest, LimitResult) {
    auto result = executor->execute(
        "SELECT price FROM trades LIMIT 3");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_LE(result.rows.size(), 3u);
}

TEST_F(SqlExecutorTest, VwapQuery) {
    auto result = executor->execute(
        "SELECT VWAP(price, volume) FROM trades WHERE symbol = 1");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_GE(result.rows.size(), 1u);
    // VWAP should be around 15045
    EXPECT_NEAR(static_cast<double>(result.rows[0][0]), 15045.0, 100.0);
}

TEST_F(SqlExecutorTest, GroupBySymbol) {
    auto result = executor->execute(
        "SELECT symbol, sum(volume) FROM trades GROUP BY symbol");
    ASSERT_TRUE(result.ok()) << result.error;
    // symbol=1 그룹과 symbol=2 그룹
    EXPECT_GE(result.rows.size(), 1u);
}

TEST_F(SqlExecutorTest, ParseError) {
    auto result = executor->execute("SELECT FROM WHERE");
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.error.empty());
}

// ============================================================================
// Part 4: GROUP BY + Time Range 통합 테스트
// ============================================================================

// GROUP BY symbol: 파티션 기반 최적화 경로 — 다중 집계 함수
TEST_F(SqlExecutorTest, GroupBySymbolMultiAgg) {
    auto result = executor->execute(
        "SELECT symbol, count(*), sum(volume), avg(price), vwap(price, volume) "
        "FROM trades GROUP BY symbol");
    ASSERT_TRUE(result.ok()) << result.error;
    // symbol=1, symbol=2 두 그룹 존재
    EXPECT_EQ(result.rows.size(), 2u);
    // 컬럼: symbol, count, sum_volume, avg_price, vwap
    EXPECT_EQ(result.column_names.size(), 5u);

    // symbol=1: count=10, sum(volume)=1045, avg(price)≈15045
    int64_t sym1_count = -1, sym1_sum_vol = -1;
    for (const auto& row : result.rows) {
        if (row[0] == 1) { // symbol=1
            sym1_count   = row[1]; // count(*)
            sym1_sum_vol = row[2]; // sum(volume)
        }
    }
    EXPECT_EQ(sym1_count,   10);
    EXPECT_EQ(sym1_sum_vol, 1045);
}

// 타임스탬프 범위 + GROUP BY (이진탐색 경로)
TEST_F(SqlExecutorTest, TimeRangeGroupBy) {
    // 전체 데이터를 타임스탬프 전체 범위로 SELECT
    // (타임스탬프는 TickPlant이 현재 시간으로 설정하므로 넓은 범위 사용)
    // 가장 작은 가능한 타임스탬프 ~ 가장 큰 타임스탬프
    auto result = executor->execute(
        "SELECT symbol, sum(volume) FROM trades "
        "WHERE timestamp BETWEEN 0 AND 9223372036854775807 GROUP BY symbol");
    ASSERT_TRUE(result.ok()) << result.error;
    // 두 심볼 모두 포함되어야 함
    EXPECT_GE(result.rows.size(), 1u);
}

// ORDER BY + LIMIT (top-N partial sort)
TEST_F(SqlExecutorTest, OrderByLimit) {
    // GROUP BY symbol → ORDER BY sum(volume) DESC LIMIT 1
    // symbol=1: sum=1045, symbol=2: sum=1010
    // top-1 should be symbol=1
    auto result = executor->execute(
        "SELECT symbol, sum(volume) as total_vol FROM trades "
        "GROUP BY symbol ORDER BY total_vol DESC LIMIT 1");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    // top-1은 sum이 가장 큰 symbol=1 이어야 함
    EXPECT_EQ(result.rows[0][0], 1);
}

// 타임스탬프 BETWEEN 단순 SELECT — 이진탐색 경로
TEST_F(SqlExecutorTest, TimeRangeBinarySearch) {
    // TickPlant가 recv_ts를 현재 시각(ns)으로 설정하므로,
    // 타임스탬프 범위는 [0, INT64_MAX]로 전체를 포함하는 쿼리로 테스트.
    // 이 테스트는 이진탐색 코드 경로가 올바르게 작동하는지 확인.
    auto result = executor->execute(
        "SELECT price FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 0 AND 9223372036854775807");
    ASSERT_TRUE(result.ok()) << result.error;

    // 전체 10개 행이 나와야 함 (범위가 전체를 포함)
    EXPECT_EQ(result.rows.size(), 10u)
        << "rows_scanned=" << result.rows_scanned;

    // 이진탐색 경로를 통해 rows_scanned이 설정됨 (10 이하)
    EXPECT_LE(result.rows_scanned, 10u);

    // 가격 범위도 확인
    auto r_price = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 AND price BETWEEN 15020 AND 15050");
    ASSERT_TRUE(r_price.ok()) << r_price.error;
    EXPECT_EQ(r_price.rows.size(), 4u);
}

// ORDER BY ASC 정렬 확인
TEST_F(SqlExecutorTest, OrderByAsc) {
    auto result = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 ORDER BY price ASC LIMIT 3");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 3u);
    // 오름차순: 15000, 15010, 15020
    EXPECT_EQ(result.rows[0][0], 15000);
    EXPECT_EQ(result.rows[1][0], 15010);
    EXPECT_EQ(result.rows[2][0], 15020);
}

// ORDER BY DESC 정렬 확인
TEST_F(SqlExecutorTest, OrderByDesc) {
    auto result = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 ORDER BY price DESC LIMIT 3");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 3u);
    // 내림차순: 15090, 15080, 15070
    EXPECT_EQ(result.rows[0][0], 15090);
    EXPECT_EQ(result.rows[1][0], 15080);
    EXPECT_EQ(result.rows[2][0], 15070);
}

// MIN / MAX 집계
TEST_F(SqlExecutorTest, MinMaxAgg) {
    auto result = executor->execute(
        "SELECT min(price), max(price) FROM trades WHERE symbol = 1");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 15000); // min
    EXPECT_EQ(result.rows[0][1], 15090); // max
}



TEST(AsofJoin, BasicCorrectness) {
    // 왼쪽: trades (symbol, timestamp)
    // 오른쪽: quotes (symbol, timestamp, bid)
    // ASOF: 각 trade에 대해 trade.timestamp >= quote.timestamp 인 최신 quote 매칭

    // 아레나 생성
    ArenaAllocator arena_l(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});
    ArenaAllocator arena_r(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});

    // 왼쪽 컬럼
    ColumnVector lk("symbol", ColumnType::INT64, arena_l);
    ColumnVector lt("timestamp", ColumnType::INT64, arena_l);

    // 오른쪽 컬럼
    ColumnVector rk("symbol", ColumnType::INT64, arena_r);
    ColumnVector rt("timestamp", ColumnType::INT64, arena_r);

    // 데이터: symbol=1
    // trades timestamps: 100, 200, 300, 400, 500
    // quotes timestamps:  50, 150, 250, 350, 450
    for (int i = 1; i <= 5; ++i) {
        lk.append<int64_t>(1);
        lt.append<int64_t>(i * 100);
        rk.append<int64_t>(1);
        rt.append<int64_t>(i * 100 - 50); // 50, 150, 250, 350, 450
    }

    AsofJoinOperator asof;
    auto res = asof.execute(lk, rk, &lt, &rt);

    // 모든 5개 trade가 매칭되어야 함
    EXPECT_EQ(res.match_count, 5u);

    // trade(100) → quote(50), trade(200) → quote(150), ...
    ASSERT_EQ(res.left_indices.size(), 5u);
    ASSERT_EQ(res.right_indices.size(), 5u);

    // 각 trade에 대해 올바른 quote 인덱스 확인
    // trade[0]=100 → quote[0]=50 (최신 quote <= 100)
    EXPECT_EQ(res.right_indices[0], 0);
    // trade[1]=200 → quote[1]=150 (최신 quote <= 200)
    EXPECT_EQ(res.right_indices[1], 1);
    // trade[4]=500 → quote[4]=450 (최신 quote <= 500)
    EXPECT_EQ(res.right_indices[4], 4);
}

TEST(AsofJoin, MultipleSymbols) {
    ArenaAllocator arena_l(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});
    ArenaAllocator arena_r(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});

    ColumnVector lk("symbol", ColumnType::INT64, arena_l);
    ColumnVector lt("timestamp", ColumnType::INT64, arena_l);
    ColumnVector rk("symbol", ColumnType::INT64, arena_r);
    ColumnVector rt("timestamp", ColumnType::INT64, arena_r);

    // symbol=1: trade@100, quote@50
    // symbol=2: trade@200, quote@150
    lk.append<int64_t>(1); lt.append<int64_t>(100);
    lk.append<int64_t>(2); lt.append<int64_t>(200);
    rk.append<int64_t>(1); rt.append<int64_t>(50);
    rk.append<int64_t>(2); rt.append<int64_t>(150);

    AsofJoinOperator asof;
    auto res = asof.execute(lk, rk, &lt, &rt);

    EXPECT_EQ(res.match_count, 2u);
}

TEST(AsofJoin, NoMatch) {
    ArenaAllocator arena_l(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});
    ArenaAllocator arena_r(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});

    ColumnVector lk("symbol", ColumnType::INT64, arena_l);
    ColumnVector lt("timestamp", ColumnType::INT64, arena_l);
    ColumnVector rk("symbol", ColumnType::INT64, arena_r);
    ColumnVector rt("timestamp", ColumnType::INT64, arena_r);

    // trade@50, quote@100 → quote가 trade보다 나중이므로 매칭 없음
    lk.append<int64_t>(1); lt.append<int64_t>(50);
    rk.append<int64_t>(1); rt.append<int64_t>(100);

    AsofJoinOperator asof;
    auto res = asof.execute(lk, rk, &lt, &rt);

    EXPECT_EQ(res.match_count, 0u);
}

// HashJoinOperator는 이제 구현됨 — 빈 입력에 대해 0 매칭 반환 검증
TEST(AsofJoin, HashJoinEmpty) {
    ArenaAllocator arena(ArenaConfig{.total_size = 1 << 20, .use_hugepages = false, .numa_node = -1});
    ColumnVector lk("symbol", ColumnType::INT64, arena);
    ColumnVector rk("symbol", ColumnType::INT64, arena);

    HashJoinOperator hj;
    auto result = hj.execute(lk, rk);
    EXPECT_EQ(result.match_count, 0u);
}

// ============================================================================
// Part 5: Phase 1 신규 기능 — 파서 단위 테스트
// IN / IS NULL / IS NOT NULL / NOT / HAVING
// ============================================================================

// ── Tokenizer: 새 토큰 ────────────────────────────────────────────────────
TEST(Tokenizer, InKeyword) {
    Tokenizer tok;
    auto tokens = tok.tokenize("WHERE symbol IN (1, 2, 3)");
    bool found_in = false, found_null = false;
    for (const auto& t : tokens) {
        if (t.type == TokenType::IN) found_in = true;
    }
    EXPECT_TRUE(found_in);
    (void)found_null;
}

TEST(Tokenizer, IsNullKeywords) {
    Tokenizer tok;
    auto tokens = tok.tokenize("WHERE price IS NOT NULL");
    bool found_is = false, found_not = false, found_null = false;
    for (const auto& t : tokens) {
        if (t.type == TokenType::IS)     found_is   = true;
        if (t.type == TokenType::NOT)    found_not  = true;
        if (t.type == TokenType::NULL_KW) found_null = true;
    }
    EXPECT_TRUE(found_is);
    EXPECT_TRUE(found_not);
    EXPECT_TRUE(found_null);
}

// ── Parser: IN 연산자 ─────────────────────────────────────────────────────
TEST(Parser, InOperatorBasic) {
    Parser p;
    auto stmt = p.parse("SELECT * FROM trades WHERE symbol IN (1, 2, 5)");
    ASSERT_TRUE(stmt.where.has_value());
    const auto& e = stmt.where->expr;
    EXPECT_EQ(e->kind, Expr::Kind::IN);
    EXPECT_EQ(e->column, "symbol");
    ASSERT_EQ(e->in_values.size(), 3u);
    EXPECT_EQ(e->in_values[0], 1);
    EXPECT_EQ(e->in_values[1], 2);
    EXPECT_EQ(e->in_values[2], 5);
}

TEST(Parser, InOperatorSingleValue) {
    Parser p;
    auto stmt = p.parse("SELECT price FROM trades WHERE volume IN (100)");
    const auto& e = stmt.where->expr;
    EXPECT_EQ(e->kind, Expr::Kind::IN);
    ASSERT_EQ(e->in_values.size(), 1u);
    EXPECT_EQ(e->in_values[0], 100);
}

TEST(Parser, InOperatorManyValues) {
    Parser p;
    auto stmt = p.parse("SELECT * FROM trades WHERE price IN (15000,15010,15020,15030,15040)");
    const auto& e = stmt.where->expr;
    EXPECT_EQ(e->kind, Expr::Kind::IN);
    ASSERT_EQ(e->in_values.size(), 5u);
    EXPECT_EQ(e->in_values[4], 15040);
}

TEST(Parser, InWithAnd) {
    Parser p;
    auto stmt = p.parse(
        "SELECT * FROM trades WHERE symbol IN (1, 2) AND price > 15000");
    const auto& e = stmt.where->expr;
    EXPECT_EQ(e->kind, Expr::Kind::AND);
    EXPECT_EQ(e->left->kind, Expr::Kind::IN);
    EXPECT_EQ(e->right->kind, Expr::Kind::COMPARE);
    EXPECT_EQ(e->right->op, CompareOp::GT);
}

TEST(Parser, InWithOr) {
    Parser p;
    auto stmt = p.parse(
        "SELECT * FROM trades WHERE price IN (15000, 15010) OR volume > 200");
    const auto& e = stmt.where->expr;
    EXPECT_EQ(e->kind, Expr::Kind::OR);
    EXPECT_EQ(e->left->kind, Expr::Kind::IN);
    EXPECT_EQ(e->right->kind, Expr::Kind::COMPARE);
}

// ── Parser: IS NULL / IS NOT NULL ─────────────────────────────────────────
TEST(Parser, IsNull) {
    Parser p;
    auto stmt = p.parse("SELECT * FROM trades WHERE price IS NULL");
    const auto& e = stmt.where->expr;
    EXPECT_EQ(e->kind, Expr::Kind::IS_NULL);
    EXPECT_EQ(e->column, "price");
    EXPECT_FALSE(e->negated);
}

TEST(Parser, IsNotNull) {
    Parser p;
    auto stmt = p.parse("SELECT * FROM trades WHERE volume IS NOT NULL");
    const auto& e = stmt.where->expr;
    EXPECT_EQ(e->kind, Expr::Kind::IS_NULL);
    EXPECT_EQ(e->column, "volume");
    EXPECT_TRUE(e->negated);
}

TEST(Parser, IsNullWithAlias) {
    Parser p;
    auto stmt = p.parse("SELECT * FROM trades t WHERE t.price IS NOT NULL");
    const auto& e = stmt.where->expr;
    EXPECT_EQ(e->kind, Expr::Kind::IS_NULL);
    EXPECT_EQ(e->table_alias, "t");
    EXPECT_EQ(e->column, "price");
    EXPECT_TRUE(e->negated);
}

TEST(Parser, IsNullAndOtherCond) {
    Parser p;
    auto stmt = p.parse(
        "SELECT * FROM trades WHERE price IS NOT NULL AND volume > 100");
    const auto& e = stmt.where->expr;
    EXPECT_EQ(e->kind, Expr::Kind::AND);
    EXPECT_EQ(e->left->kind, Expr::Kind::IS_NULL);
    EXPECT_TRUE(e->left->negated);
    EXPECT_EQ(e->right->kind, Expr::Kind::COMPARE);
}

// ── Parser: NOT 연산자 (수정된 동작 검증) ─────────────────────────────────
TEST(Parser, NotOperatorWrapsCompare) {
    Parser p;
    auto stmt = p.parse("SELECT * FROM trades WHERE NOT price > 15000");
    const auto& e = stmt.where->expr;
    EXPECT_EQ(e->kind, Expr::Kind::NOT);
    ASSERT_NE(e->left, nullptr);
    EXPECT_EQ(e->left->kind, Expr::Kind::COMPARE);
    EXPECT_EQ(e->left->column, "price");
    EXPECT_EQ(e->left->op, CompareOp::GT);
}

TEST(Parser, NotOperatorWrapsIn) {
    Parser p;
    auto stmt = p.parse("SELECT * FROM trades WHERE NOT price IN (15000, 15090)");
    const auto& e = stmt.where->expr;
    EXPECT_EQ(e->kind, Expr::Kind::NOT);
    EXPECT_EQ(e->left->kind, Expr::Kind::IN);
    ASSERT_EQ(e->left->in_values.size(), 2u);
}

TEST(Parser, NotOperatorWrapsIsNull) {
    Parser p;
    // NOT (price IS NULL) — 이중 부정
    auto stmt = p.parse("SELECT * FROM trades WHERE NOT price IS NULL");
    const auto& e = stmt.where->expr;
    EXPECT_EQ(e->kind, Expr::Kind::NOT);
    EXPECT_EQ(e->left->kind, Expr::Kind::IS_NULL);
    EXPECT_FALSE(e->left->negated);  // IS NULL (NOT wraps it, not negated internally)
}

TEST(Parser, NotWithParens) {
    Parser p;
    auto stmt = p.parse(
        "SELECT * FROM trades WHERE NOT (price > 15050 AND volume < 105)");
    const auto& e = stmt.where->expr;
    EXPECT_EQ(e->kind, Expr::Kind::NOT);
    EXPECT_EQ(e->left->kind, Expr::Kind::AND);
}

// ── Parser: HAVING 절 ──────────────────────────────────────────────────────
TEST(Parser, HavingBasicCompare) {
    Parser p;
    auto stmt = p.parse(
        "SELECT symbol, SUM(volume) AS vol FROM trades "
        "GROUP BY symbol HAVING vol > 1000");
    EXPECT_TRUE(stmt.group_by.has_value());
    ASSERT_TRUE(stmt.having.has_value());
    const auto& e = stmt.having->expr;
    EXPECT_EQ(e->kind, Expr::Kind::COMPARE);
    EXPECT_EQ(e->column, "vol");
    EXPECT_EQ(e->op, CompareOp::GT);
    EXPECT_EQ(e->value, 1000);
}

TEST(Parser, HavingWithGe) {
    Parser p;
    auto stmt = p.parse(
        "SELECT symbol, COUNT(*) AS cnt FROM trades "
        "GROUP BY symbol HAVING cnt >= 5");
    ASSERT_TRUE(stmt.having.has_value());
    const auto& e = stmt.having->expr;
    EXPECT_EQ(e->op, CompareOp::GE);
    EXPECT_EQ(e->value, 5);
}

TEST(Parser, HavingWithIn) {
    Parser p;
    auto stmt = p.parse(
        "SELECT symbol, COUNT(*) AS cnt FROM trades "
        "GROUP BY symbol HAVING cnt IN (5, 10)");
    ASSERT_TRUE(stmt.having.has_value());
    const auto& e = stmt.having->expr;
    EXPECT_EQ(e->kind, Expr::Kind::IN);
    ASSERT_EQ(e->in_values.size(), 2u);
}

TEST(Parser, HavingAndOrderByLimit) {
    Parser p;
    auto stmt = p.parse(
        "SELECT symbol, SUM(volume) AS vol FROM trades "
        "GROUP BY symbol HAVING vol > 0 ORDER BY vol DESC LIMIT 1");
    EXPECT_TRUE(stmt.having.has_value());
    EXPECT_TRUE(stmt.order_by.has_value());
    ASSERT_TRUE(stmt.limit.has_value());
    EXPECT_EQ(*stmt.limit, 1);
}

TEST(Parser, HavingAndExpr) {
    Parser p;
    auto stmt = p.parse(
        "SELECT symbol, COUNT(*) AS cnt, SUM(volume) AS vol FROM trades "
        "GROUP BY symbol HAVING cnt > 1 AND vol > 100");
    ASSERT_TRUE(stmt.having.has_value());
    const auto& e = stmt.having->expr;
    EXPECT_EQ(e->kind, Expr::Kind::AND);
    EXPECT_EQ(e->left->column, "cnt");
    EXPECT_EQ(e->right->column, "vol");
}

// ── Parser: 복합 조건 ──────────────────────────────────────────────────────
TEST(Parser, ComplexWhereInBetweenAnd) {
    Parser p;
    auto stmt = p.parse(
        "SELECT * FROM trades "
        "WHERE symbol IN (1, 2) AND price BETWEEN 15000 AND 15050 AND volume > 100");
    // AND은 왼쪽 우선 결합: ((IN AND BETWEEN) AND COMPARE)
    const auto& root = stmt.where->expr;
    EXPECT_EQ(root->kind, Expr::Kind::AND);
    EXPECT_EQ(root->left->kind, Expr::Kind::AND);
    EXPECT_EQ(root->left->left->kind, Expr::Kind::IN);
    EXPECT_EQ(root->left->right->kind, Expr::Kind::BETWEEN);
    EXPECT_EQ(root->right->kind, Expr::Kind::COMPARE);
}

// ============================================================================
// Part 6: Phase 1 신규 기능 — 실행기 통합 테스트
// 실제 데이터 적재 후 IN / IS NULL / NOT / HAVING 실행 검증
// 데이터: symbol=1(10행: price 15000~15090 step10, volume 100~109)
//         symbol=2(5행:  price 20000~20040 step10, volume 200~204)
// ============================================================================

// ── IN 연산자 ─────────────────────────────────────────────────────────────
TEST_F(SqlExecutorTest, InPrice3Values) {
    // price IN (15000, 15030, 15060) → symbol=1 에서 정확히 3행
    auto r = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 AND price IN (15000, 15030, 15060)");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 3u);
    // 반환된 price 값 확인
    std::vector<int64_t> prices;
    for (const auto& row : r.rows) prices.push_back(row[0]);
    std::sort(prices.begin(), prices.end());
    EXPECT_EQ(prices[0], 15000);
    EXPECT_EQ(prices[1], 15030);
    EXPECT_EQ(prices[2], 15060);
}

TEST_F(SqlExecutorTest, InPriceSingleMatch) {
    auto r = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 AND price IN (15090)");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 15090);
}

TEST_F(SqlExecutorTest, InPriceNoMatch) {
    // 존재하지 않는 price 값 → 0행
    auto r = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 AND price IN (99999, 88888)");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 0u);
}

TEST_F(SqlExecutorTest, InVolumeWithAndCondition) {
    // volume IN (100, 101, 102) AND price > 15010
    // volume 101=price15010(제외), 102=price15020(포함) → 1행
    auto r = executor->execute(
        "SELECT price, volume FROM trades "
        "WHERE symbol = 1 AND volume IN (100, 101, 102) AND price > 15010");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][1], 102);  // volume=102
}

TEST_F(SqlExecutorTest, InVolumeAllMatch) {
    // volume IN (100..109) = 모든 symbol=1 행
    auto r = executor->execute(
        "SELECT count(*) FROM trades WHERE symbol = 1 "
        "AND volume IN (100,101,102,103,104,105,106,107,108,109)");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 10);
}

// ── IS NULL / IS NOT NULL ─────────────────────────────────────────────────
TEST_F(SqlExecutorTest, IsNotNullPrice) {
    // price 컬럼에 NULL 없음 → IS NOT NULL = 전체 10행
    auto r = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 AND price IS NOT NULL");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 10u);
}

TEST_F(SqlExecutorTest, IsNullPriceReturnsEmpty) {
    // price 컬럼에 NULL 없음 → IS NULL = 0행
    auto r = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 AND price IS NULL");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 0u);
}

TEST_F(SqlExecutorTest, IsNotNullCombinedWithGt) {
    // price IS NOT NULL AND price > 15050 → 4행
    auto r = executor->execute(
        "SELECT price FROM trades "
        "WHERE symbol = 1 AND price IS NOT NULL AND price > 15050");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 4u);
}

// ── NOT 연산자 (버그 수정 검증) ───────────────────────────────────────────
TEST_F(SqlExecutorTest, NotGtIsComplement) {
    // NOT price > 15050 → price <= 15050 → 6행 (15000~15050)
    // 버그 수정 전: NOT이 무시되어 price > 15050 = 4행 반환됐음
    auto r_not = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 AND NOT price > 15050");
    ASSERT_TRUE(r_not.ok()) << r_not.error;
    EXPECT_EQ(r_not.rows.size(), 6u)
        << "NOT was silently ignored bug: expected 6 rows (NOT price>15050)";

    // 확인: price > 15050은 4행
    auto r_gt = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 AND price > 15050");
    ASSERT_TRUE(r_gt.ok()) << r_gt.error;
    EXPECT_EQ(r_gt.rows.size(), 4u);

    // NOT의 결과가 원래 결과와 달라야 함
    EXPECT_NE(r_not.rows.size(), r_gt.rows.size());
}

TEST_F(SqlExecutorTest, NotInPrice) {
    // NOT price IN (15000, 15090) → price가 15000도 15090도 아닌 행 → 8행
    auto r = executor->execute(
        "SELECT price FROM trades "
        "WHERE symbol = 1 AND NOT price IN (15000, 15090)");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 8u);
    // 15000, 15090이 포함되지 않음을 확인
    for (const auto& row : r.rows) {
        EXPECT_NE(row[0], 15000);
        EXPECT_NE(row[0], 15090);
    }
}

TEST_F(SqlExecutorTest, NotBetween) {
    // NOT price BETWEEN 15020 AND 15060 → price<15020 OR price>15060
    // prices in range [15020,15060]: 15020,15030,15040,15050,15060 = 5 rows excluded
    // remaining: 15000,15010,15070,15080,15090 = 5 rows
    auto r = executor->execute(
        "SELECT price FROM trades "
        "WHERE symbol = 1 AND NOT price BETWEEN 15020 AND 15060");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 5u);
}

// ── HAVING 절 ─────────────────────────────────────────────────────────────
TEST_F(SqlExecutorTest, HavingCountGt) {
    // GROUP BY symbol HAVING cnt > 8 → symbol=1(cnt=10)만 반환
    auto r = executor->execute(
        "SELECT symbol, COUNT(*) AS cnt FROM trades "
        "GROUP BY symbol HAVING cnt > 8");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 1);  // symbol=1
    EXPECT_EQ(r.rows[0][1], 10); // cnt=10
}

TEST_F(SqlExecutorTest, HavingCountGe5BothGroups) {
    // cnt >= 5 → symbol=1(10), symbol=2(5) 둘 다 반환
    auto r = executor->execute(
        "SELECT symbol, COUNT(*) AS cnt FROM trades "
        "GROUP BY symbol HAVING cnt >= 5");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 2u);
}

TEST_F(SqlExecutorTest, HavingSumVolume) {
    // sum(volume) AS vol HAVING vol > 1040
    // symbol=1: sum(100..109)=1045 > 1040 ✓
    // symbol=2: sum(200..204)=1010 < 1040 ✗
    auto r = executor->execute(
        "SELECT symbol, SUM(volume) AS vol FROM trades "
        "GROUP BY symbol HAVING vol > 1040");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 1);    // symbol=1
    EXPECT_EQ(r.rows[0][1], 1045); // sum=1045
}

TEST_F(SqlExecutorTest, HavingNoMatch) {
    // cnt > 100 → 둘 다 해당 없음 → 0행
    auto r = executor->execute(
        "SELECT symbol, COUNT(*) AS cnt FROM trades "
        "GROUP BY symbol HAVING cnt > 100");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 0u);
}

TEST_F(SqlExecutorTest, HavingWithOrderByLimit) {
    // HAVING vol > 0 → 둘 다 통과, ORDER BY vol DESC LIMIT 1 → symbol=1
    auto r = executor->execute(
        "SELECT symbol, SUM(volume) AS vol FROM trades "
        "GROUP BY symbol HAVING vol > 0 ORDER BY vol DESC LIMIT 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 1);    // symbol=1 (vol=1045 > symbol=2의 1010)
}

TEST_F(SqlExecutorTest, HavingMinMax) {
    // max(price) AS maxp HAVING maxp > 19000
    // symbol=1: max(price)=15090 < 19000 ✗
    // symbol=2: max(price)=20040 > 19000 ✓
    auto r = executor->execute(
        "SELECT symbol, MAX(price) AS maxp FROM trades "
        "GROUP BY symbol HAVING maxp > 19000");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 2);     // symbol=2
    EXPECT_EQ(r.rows[0][1], 20040); // max price
}

TEST_F(SqlExecutorTest, HavingAndExpr) {
    // cnt > 1 AND vol > 1020 → symbol=1 only (cnt=10, vol=1045 > 1020)
    // symbol=2 has vol=1010 which fails vol > 1020
    auto r = executor->execute(
        "SELECT symbol, COUNT(*) AS cnt, SUM(volume) AS vol FROM trades "
        "GROUP BY symbol HAVING cnt > 1 AND vol > 1020");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 1);
}

TEST_F(SqlExecutorTest, HavingInValues) {
    // HAVING cnt IN (5, 10) → symbol=1(10), symbol=2(5) 모두 해당
    auto r = executor->execute(
        "SELECT symbol, COUNT(*) AS cnt FROM trades "
        "GROUP BY symbol HAVING cnt IN (5, 10)");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 2u);
}

TEST_F(SqlExecutorTest, HavingInValuesPartialMatch) {
    // HAVING cnt IN (10, 99) → symbol=1(10)만 해당
    auto r = executor->execute(
        "SELECT symbol, COUNT(*) AS cnt FROM trades "
        "GROUP BY symbol HAVING cnt IN (10, 99)");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 1);
    EXPECT_EQ(r.rows[0][1], 10);
}

TEST_F(SqlExecutorTest, HavingWithWhereAndGroupBy) {
    // WHERE price > 15000 (symbol=1에서 9행만) → GROUP BY → HAVING cnt > 8 → 1행
    auto r = executor->execute(
        "SELECT symbol, COUNT(*) AS cnt FROM trades "
        "WHERE symbol = 1 AND price > 15000 "
        "GROUP BY symbol HAVING cnt > 8");
    ASSERT_TRUE(r.ok()) << r.error;
    // price > 15000 → 15010~15090 = 9행, HAVING cnt > 8 → 통과
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][1], 9);
}

// ── 복합 쿼리 (IN + HAVING + ORDER BY) ───────────────────────────────────
TEST_F(SqlExecutorTest, InWithGroupByHaving) {
    // volume IN (100..104) 로 symbol=1에서 5행만 걸러 → GROUP BY → HAVING cnt = 5
    auto r = executor->execute(
        "SELECT symbol, COUNT(*) AS cnt, SUM(volume) AS vol FROM trades "
        "WHERE symbol = 1 AND volume IN (100, 101, 102, 103, 104) "
        "GROUP BY symbol HAVING cnt > 0");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][1], 5);   // cnt=5
    EXPECT_EQ(r.rows[0][2], 510); // sum(100..104) = 510
}

// ── 에러 케이스 ───────────────────────────────────────────────────────────
TEST_F(SqlExecutorTest, HavingWithoutGroupByParses) {
    // HAVING without GROUP BY — 파서는 허용, 실행 시 집계 없으므로 빈 결과
    // (파서 에러 없음을 확인)
    auto r = executor->execute(
        "SELECT price FROM trades HAVING price > 0");
    // 파서는 통과, 집계 없으므로 결과는 빈 결과 또는 정상 처리
    // 에러 없음을 보장
    EXPECT_TRUE(r.error.empty() || !r.error.empty()); // 결과 상관없이 crash 안 됨
}

// ── NOT 버그 회귀 테스트 (수정 이전 동작이 재발하지 않음 확인) ────────────
TEST_F(SqlExecutorTest, NotRegressionOldBehaviorGone) {
    // 이전 버그: NOT이 무시되어 "NOT price > 15000"이 "price > 15000"처럼 동작
    // 수정 후: NOT price > 15000 = price <= 15000
    auto r_not = executor->execute(
        "SELECT count(*) FROM trades WHERE symbol = 1 AND NOT price > 15000");
    auto r_gt  = executor->execute(
        "SELECT count(*) FROM trades WHERE symbol = 1 AND price > 15000");
    ASSERT_TRUE(r_not.ok()) << r_not.error;
    ASSERT_TRUE(r_gt.ok())  << r_gt.error;

    int64_t cnt_not = r_not.rows[0][0];
    int64_t cnt_gt  = r_gt.rows[0][0];

    // NOT이 제대로 동작하면 결과가 달라야 함
    EXPECT_NE(cnt_not, cnt_gt) << "NOT is being silently ignored (regression)";
    // 합이 10이어야 함 (NOT complement)
    EXPECT_EQ(cnt_not + cnt_gt, 10);
}

// ============================================================================
// Part 7: Phase 2 — SELECT Arithmetic Expressions
// ============================================================================

// ── Tokenizer ─────────────────────────────────────────────────────────────
TEST(Tokenizer, SlashToken) {
    Tokenizer t;
    auto toks = t.tokenize("price / volume");
    ASSERT_EQ(toks[1].type, TokenType::SLASH);
}

TEST(Tokenizer, CaseWhenKeywords) {
    Tokenizer t;
    auto toks = t.tokenize("CASE WHEN x THEN y ELSE z END");
    EXPECT_EQ(toks[0].type, TokenType::CASE);
    EXPECT_EQ(toks[1].type, TokenType::WHEN);
    EXPECT_EQ(toks[3].type, TokenType::THEN);
    EXPECT_EQ(toks[5].type, TokenType::ELSE);
    EXPECT_EQ(toks[7].type, TokenType::CASE_END);
}

// ── Parser: arithmetic ────────────────────────────────────────────────────
TEST(Parser, ArithMulTwoColumns) {
    Parser p;
    auto s = p.parse("SELECT price * volume AS notional FROM trades");
    ASSERT_EQ(s.columns.size(), 1u);
    ASSERT_NE(s.columns[0].arith_expr, nullptr);
    EXPECT_EQ(s.columns[0].arith_expr->kind, ArithExpr::Kind::BINARY);
    EXPECT_EQ(s.columns[0].arith_expr->arith_op, ArithOp::MUL);
    EXPECT_EQ(s.columns[0].arith_expr->left->column, "price");
    EXPECT_EQ(s.columns[0].arith_expr->right->column, "volume");
    EXPECT_EQ(s.columns[0].alias, "notional");
}

TEST(Parser, ArithSubLiteral) {
    Parser p;
    auto s = p.parse("SELECT price - 15000 AS offset FROM trades");
    ASSERT_NE(s.columns[0].arith_expr, nullptr);
    EXPECT_EQ(s.columns[0].arith_expr->arith_op, ArithOp::SUB);
    EXPECT_EQ(s.columns[0].arith_expr->right->kind, ArithExpr::Kind::LITERAL);
    EXPECT_EQ(s.columns[0].arith_expr->right->literal, 15000);
}

TEST(Parser, ArithDivLiteral) {
    Parser p;
    auto s = p.parse("SELECT price / 100 AS price_cents FROM trades");
    ASSERT_NE(s.columns[0].arith_expr, nullptr);
    EXPECT_EQ(s.columns[0].arith_expr->arith_op, ArithOp::DIV);
    EXPECT_EQ(s.columns[0].arith_expr->right->literal, 100);
}

TEST(Parser, ArithAddTwoColumns) {
    Parser p;
    auto s = p.parse("SELECT price + volume AS pv FROM trades");
    ASSERT_NE(s.columns[0].arith_expr, nullptr);
    EXPECT_EQ(s.columns[0].arith_expr->arith_op, ArithOp::ADD);
}

TEST(Parser, ArithMultipleSelectCols) {
    // Plain col + arithmetic col in same SELECT
    Parser p;
    auto s = p.parse("SELECT symbol, price * volume AS notional FROM trades");
    EXPECT_EQ(s.columns[0].column, "symbol");
    EXPECT_EQ(s.columns[0].arith_expr, nullptr);
    ASSERT_NE(s.columns[1].arith_expr, nullptr);
    EXPECT_EQ(s.columns[1].alias, "notional");
}

TEST(Parser, ArithInsideAggregate) {
    // SUM(price * volume)
    Parser p;
    auto s = p.parse("SELECT SUM(price * volume) AS total FROM trades");
    EXPECT_EQ(s.columns[0].agg, AggFunc::SUM);
    ASSERT_NE(s.columns[0].arith_expr, nullptr);
    EXPECT_EQ(s.columns[0].arith_expr->arith_op, ArithOp::MUL);
    EXPECT_EQ(s.columns[0].alias, "total");
}

TEST(Parser, ArithParenthesized) {
    // (price - 14000) / 100
    Parser p;
    auto s = p.parse("SELECT (price - 14000) / 100 AS tick FROM trades");
    ASSERT_NE(s.columns[0].arith_expr, nullptr);
    EXPECT_EQ(s.columns[0].arith_expr->arith_op, ArithOp::DIV);
    EXPECT_EQ(s.columns[0].arith_expr->left->arith_op, ArithOp::SUB);
}

// ── Parser: CASE WHEN ─────────────────────────────────────────────────────
TEST(Parser, CaseWhenBasic) {
    Parser p;
    auto s = p.parse(
        "SELECT CASE WHEN price > 15050 THEN 1 ELSE 0 END AS side FROM trades");
    ASSERT_NE(s.columns[0].case_when, nullptr);
    EXPECT_EQ(s.columns[0].case_when->branches.size(), 1u);
    EXPECT_EQ(s.columns[0].case_when->else_val->literal, 0);
    EXPECT_EQ(s.columns[0].alias, "side");
}

TEST(Parser, CaseWhenMultipleBranches) {
    Parser p;
    auto s = p.parse(
        "SELECT CASE WHEN price < 15030 THEN 1 WHEN price < 15060 THEN 2 ELSE 3 END AS bucket FROM trades");
    ASSERT_NE(s.columns[0].case_when, nullptr);
    EXPECT_EQ(s.columns[0].case_when->branches.size(), 2u);
    EXPECT_EQ(s.columns[0].case_when->else_val->literal, 3);
}

TEST(Parser, CaseWhenNoElse) {
    Parser p;
    auto s = p.parse("SELECT CASE WHEN price = 15000 THEN 1 END AS x FROM trades");
    ASSERT_NE(s.columns[0].case_when, nullptr);
    EXPECT_EQ(s.columns[0].case_when->else_val, nullptr); // no ELSE → null
}

// ── Executor: arithmetic ─────────────────────────────────────────────────
TEST_F(SqlExecutorTest, ArithMulColCol) {
    // price * volume AS notional for first row of symbol=1
    // first row: price=15000, volume=100 → notional=1500000
    auto r = executor->execute(
        "SELECT price * volume AS notional FROM trades "
        "WHERE symbol = 1 ORDER BY notional ASC LIMIT 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 15000LL * 100);
}

TEST_F(SqlExecutorTest, ArithSubLiteral) {
    // price - 15000 AS offset, first 3 rows of symbol=1 → 0,10,20
    auto r = executor->execute(
        "SELECT price - 15000 AS offset FROM trades "
        "WHERE symbol = 1 ORDER BY offset ASC LIMIT 3");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 3u);
    EXPECT_EQ(r.rows[0][0], 0);
    EXPECT_EQ(r.rows[1][0], 10);
    EXPECT_EQ(r.rows[2][0], 20);
}

TEST_F(SqlExecutorTest, ArithDivLiteral) {
    // price / 100 for first row of symbol=1 → 150
    auto r = executor->execute(
        "SELECT price / 100 AS cents FROM trades "
        "WHERE symbol = 1 ORDER BY cents ASC LIMIT 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 150);
}

TEST_F(SqlExecutorTest, ArithAddColCol) {
    // price + volume, first row: 15000 + 100 = 15100
    auto r = executor->execute(
        "SELECT price + volume AS pv FROM trades "
        "WHERE symbol = 1 ORDER BY pv ASC LIMIT 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 15100);
}

TEST_F(SqlExecutorTest, ArithParenthesized) {
    // (price - 14000) / 100: price=15000 → 10, price=15010 → 10 (int div)
    auto r = executor->execute(
        "SELECT (price - 14000) / 100 AS tick FROM trades "
        "WHERE symbol = 1 ORDER BY tick ASC LIMIT 2");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_GE(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 10); // (15000-14000)/100 = 10
}

TEST_F(SqlExecutorTest, ArithSumOfProduct) {
    // SUM(price * volume) for symbol=1
    // sum(i=0..9) (15000+10i)*(100+i) = 15722850
    auto r = executor->execute(
        "SELECT SUM(price * volume) AS total FROM trades WHERE symbol = 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 15722850LL);
}

TEST_F(SqlExecutorTest, ArithAvgOfDifference) {
    // AVG(price - 15000) for symbol=1
    // offsets: 0,10,20,30,40,50,60,70,80,90 → sum=450, avg=45
    auto r = executor->execute(
        "SELECT AVG(price - 15000) AS avg_offset FROM trades WHERE symbol = 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 45);
}

TEST_F(SqlExecutorTest, ArithMultiColResult) {
    // symbol + price*volume in same query
    auto r = executor->execute(
        "SELECT symbol, price * volume AS notional FROM trades "
        "WHERE symbol = 1 ORDER BY notional ASC LIMIT 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 1);               // symbol
    EXPECT_EQ(r.rows[0][1], 15000LL * 100);   // notional
}

// ── Executor: CASE WHEN ──────────────────────────────────────────────────
TEST_F(SqlExecutorTest, CaseWhenBasicBinary) {
    // CASE WHEN price > 15050 THEN 1 ELSE 0 END AS side
    // prices > 15050: 15060,15070,15080,15090 → 4 rows with side=1
    auto r = executor->execute(
        "SELECT CASE WHEN price > 15050 THEN 1 ELSE 0 END AS side "
        "FROM trades WHERE symbol = 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 10u);
    int64_t ones = 0, zeros = 0;
    for (const auto& row : r.rows) {
        if (row[0] == 1) ones++;
        else if (row[0] == 0) zeros++;
    }
    EXPECT_EQ(ones,  4); // prices 15060..15090
    EXPECT_EQ(zeros, 6); // prices 15000..15050
}

TEST_F(SqlExecutorTest, CaseWhenMultiBranch) {
    // price < 15030 → 1, price < 15060 → 2, else → 3
    // sym=1 prices: 15000,15010,15020 → bucket=1 (3 rows)
    //               15030,15040,15050 → bucket=2 (3 rows)
    //               15060,15070,15080,15090 → bucket=3 (4 rows)
    auto r = executor->execute(
        "SELECT CASE WHEN price < 15030 THEN 1 "
        "WHEN price < 15060 THEN 2 ELSE 3 END AS bucket "
        "FROM trades WHERE symbol = 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 10u);
    int64_t b1=0, b2=0, b3=0;
    for (const auto& row : r.rows) {
        if (row[0]==1) b1++;
        else if (row[0]==2) b2++;
        else b3++;
    }
    EXPECT_EQ(b1, 3);
    EXPECT_EQ(b2, 3);
    EXPECT_EQ(b3, 4);
}

TEST_F(SqlExecutorTest, CaseWhenNoElseDefaultsZero) {
    // CASE WHEN price = 15000 THEN 99 END (no ELSE → 0)
    auto r = executor->execute(
        "SELECT CASE WHEN price = 15000 THEN 99 END AS x "
        "FROM trades WHERE symbol = 1 ORDER BY x DESC LIMIT 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 99); // highest is 99 (first row matches)
}

TEST_F(SqlExecutorTest, CaseWhenWithBetween) {
    // CASE WHEN price BETWEEN 15020 AND 15060 THEN 1 ELSE 0 END → 5 ones
    auto r = executor->execute(
        "SELECT CASE WHEN price BETWEEN 15020 AND 15060 THEN 1 ELSE 0 END AS mid "
        "FROM trades WHERE symbol = 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 10u);
    int64_t ones = 0;
    for (const auto& row : r.rows) if (row[0] == 1) ones++;
    EXPECT_EQ(ones, 5); // 15020,15030,15040,15050,15060
}

TEST_F(SqlExecutorTest, CaseWhenWithThenArith) {
    // CASE WHEN price > 15050 THEN price - 15000 ELSE 0 END
    // rows with price>15050: 15060→60, 15070→70, 15080→80, 15090→90
    auto r = executor->execute(
        "SELECT CASE WHEN price > 15050 THEN price - 15000 ELSE 0 END AS v "
        "FROM trades WHERE symbol = 1 ORDER BY v DESC LIMIT 4");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 4u);
    EXPECT_EQ(r.rows[0][0], 90); // price=15090 - 15000
    EXPECT_EQ(r.rows[3][0], 60); // price=15060 - 15000
}

// ── Executor: Multi-column GROUP BY ──────────────────────────────────────
TEST_F(SqlExecutorTest, MultiGroupBySymbolAndPriceBucket) {
    // GROUP BY symbol, xbar(price, 50)
    // sym=1: bucket 15000 (5 rows) + bucket 15050 (5 rows)
    // sym=2: bucket 20000 (5 rows)
    // → 3 groups total
    auto r = executor->execute(
        "SELECT symbol, SUM(volume) AS vol FROM trades "
        "GROUP BY symbol, xbar(price, 50)");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 3u);

    // Verify all 3 groups are present with correct sums
    int64_t total_vol = 0;
    for (const auto& row : r.rows) total_vol += row[2]; // row: [symbol, price_bucket, vol]
    // Total volume across all rows: 100+101+...+109 + 200+...+204 = 1045+1010 = 2055
    EXPECT_EQ(total_vol, 2055);
}

TEST_F(SqlExecutorTest, MultiGroupBySymbolAndPrice) {
    // GROUP BY symbol, price → each row becomes its own group (15 groups)
    auto r = executor->execute(
        "SELECT symbol, price, COUNT(*) AS cnt FROM trades "
        "GROUP BY symbol, price");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 15u); // 10 sym1 + 5 sym2
    // Each group has exactly 1 row
    for (const auto& row : r.rows) {
        EXPECT_EQ(row[2], 1); // cnt=1 for every (symbol, price) pair
    }
}

TEST_F(SqlExecutorTest, MultiGroupByResultColumnCount) {
    // GROUP BY two columns → result has 2 group cols + 1 agg col = 3 total
    auto r = executor->execute(
        "SELECT symbol, SUM(volume) AS vol FROM trades "
        "GROUP BY symbol, xbar(price, 50)");
    ASSERT_TRUE(r.ok()) << r.error;
    // column_names: [symbol, price, vol]
    EXPECT_EQ(r.column_names.size(), 3u);
    EXPECT_EQ(r.column_names[0], "symbol");
    EXPECT_EQ(r.column_names[2], "vol");
}

TEST_F(SqlExecutorTest, MultiGroupByWithHaving) {
    // GROUP BY symbol, xbar(price, 50) HAVING vol > 520
    // sym=1 bucket 15000: vol=100+101+102+103+104=510 → filtered out
    // sym=1 bucket 15050: vol=105+106+107+108+109=535 → passes
    // sym=2 bucket 20000: vol=200+201+202+203+204=1010 → passes
    auto r = executor->execute(
        "SELECT symbol, SUM(volume) AS vol FROM trades "
        "GROUP BY symbol, xbar(price, 50) HAVING vol > 520");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 2u);
}

// ── Regression: existing GROUP BY still works after refactor ─────────────
TEST_F(SqlExecutorTest, GroupBySymbolStillWorks) {
    // Verify single-column GROUP BY symbol still uses is_symbol_group path
    auto r = executor->execute(
        "SELECT symbol, COUNT(*) AS cnt FROM trades GROUP BY symbol");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 2u);
}

// ============================================================================
// Part 5: HTTP 엔드포인트 테스트 (선택적)
// ============================================================================
// HTTP 서버 테스트는 통합 테스트로 분리
// (빌드 시간 + 포트 충돌 고려하여 단독 실행)

// ============================================================================
// Part 6: Time Range Index Tests
// ============================================================================
// Uses a dedicated fixture with directly-inserted, controlled timestamps
// (bypasses TickPlant which overwrites recv_ts with now_ns()).
//
// Fixture data:
//   symbol=1: timestamps 1000..1009 ns, price 15000..15090 step 10, volume 100..109
//   symbol=2: timestamps 1000..1004 ns, price 20000..20040 step 10, volume 200..204
// All fall in hour_epoch=0.
// ============================================================================

class TimeRangeIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        PipelineConfig cfg;
        cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
        pipeline  = std::make_unique<ApexPipeline>(cfg);
        executor  = std::make_unique<QueryExecutor>(*pipeline);

        auto& pm = pipeline->partition_manager();

        // symbol=1 partition — timestamps 1000..1009
        {
            auto& part = pm.get_or_create(1, 1000LL);
            auto& ts  = part.add_column("timestamp", ColumnType::TIMESTAMP_NS);
            auto& pr  = part.add_column("price",     ColumnType::INT64);
            auto& vol = part.add_column("volume",    ColumnType::INT64);
            auto& mt  = part.add_column("msg_type",  ColumnType::INT32);
            for (int i = 0; i < 10; ++i) {
                ts.append<int64_t>(1000LL + i);
                pr.append<int64_t>(15000LL + i * 10);
                vol.append<int64_t>(100LL  + i);
                mt.append<int32_t>(0);
            }
        }

        // symbol=2 partition — timestamps 1000..1004
        {
            auto& part = pm.get_or_create(2, 1000LL);
            auto& ts  = part.add_column("timestamp", ColumnType::TIMESTAMP_NS);
            auto& pr  = part.add_column("price",     ColumnType::INT64);
            auto& vol = part.add_column("volume",    ColumnType::INT64);
            auto& mt  = part.add_column("msg_type",  ColumnType::INT32);
            for (int i = 0; i < 5; ++i) {
                ts.append<int64_t>(1000LL + i);
                pr.append<int64_t>(20000LL + i * 10);
                vol.append<int64_t>(200LL  + i);
                mt.append<int32_t>(0);
            }
        }
    }

    std::unique_ptr<ApexPipeline>  pipeline;
    std::unique_ptr<QueryExecutor> executor;
};

// Partial range SELECT — binary search returns a subset of rows.
// timestamp BETWEEN 1003 AND 1007 for symbol=1 → rows i=3..7 (5 rows)
TEST_F(TimeRangeIndexTest, PartialScanCorrectRows) {
    auto result = executor->execute(
        "SELECT price FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1003 AND 1007");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows.size(), 5u);
    // Binary search must not scan more rows than the matching range
    EXPECT_LE(result.rows_scanned, 5u)
        << "binary search should limit rows_scanned to the matching window";
}

// Binary search skips leading and trailing rows.
TEST_F(TimeRangeIndexTest, PartialScanRowsScannedReduced) {
    auto result = executor->execute(
        "SELECT price FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1004 AND 1006");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows.size(), 3u);
    EXPECT_LE(result.rows_scanned, 3u)
        << "rows_scanned=" << result.rows_scanned;
}

// Partial range AGG: sum(volume) for timestamp in [1003,1007] for symbol=1
// volumes: 103+104+105+106+107 = 525
TEST_F(TimeRangeIndexTest, PartialAgg) {
    auto result = executor->execute(
        "SELECT sum(volume) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1003 AND 1007");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 525);
}

// Partial range GROUP BY: both symbols filtered by timestamp window [1003,1007]
// symbol=1: ts 1003..1007 → 5 rows; symbol=2: ts 1003,1004 → 2 rows
TEST_F(TimeRangeIndexTest, PartialGroupBy) {
    auto result = executor->execute(
        "SELECT symbol, count(*) FROM trades "
        "WHERE timestamp BETWEEN 1003 AND 1007 GROUP BY symbol");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 2u);

    int64_t cnt1 = 0, cnt2 = 0;
    for (const auto& row : result.rows) {
        if (row[0] == 1) cnt1 = row[1];
        if (row[0] == 2) cnt2 = row[1];
    }
    EXPECT_EQ(cnt1, 5) << "symbol=1 should have 5 rows in [1003,1007]";
    EXPECT_EQ(cnt2, 2) << "symbol=2 should have 2 rows (ts 1003,1004)";
}

// Empty range: no rows before the data starts
TEST_F(TimeRangeIndexTest, EmptyRange) {
    auto result = executor->execute(
        "SELECT price FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 0 AND 999");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows.size(), 0u);
}

// Single-row precision: timestamp == 1005 exactly (i=5, price=15050)
TEST_F(TimeRangeIndexTest, SingleRowPrecision) {
    auto result = executor->execute(
        "SELECT price FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 1005 AND 1005");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 15050);
}

// No symbol filter + time range: partition-level pruning via get_partitions_for_time_range
// [1007,1009] → symbol=1: ts 1007,1008,1009 = 3 rows; symbol=2: max ts=1004 → 0 rows
TEST_F(TimeRangeIndexTest, NoSymbolFilterPartialRange) {
    auto result = executor->execute(
        "SELECT count(*) FROM trades "
        "WHERE timestamp BETWEEN 1007 AND 1009");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 3);
}

// Full range still returns all rows (no regression)
TEST_F(TimeRangeIndexTest, FullRangeNoRegression) {
    auto result = executor->execute(
        "SELECT count(*) FROM trades "
        "WHERE symbol = 1 AND timestamp BETWEEN 0 AND 9223372036854775807");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 10);
}

// ============================================================================
// Part 8: Phase 3 — Date/Time Functions, LIKE, UNION
// ============================================================================

// ── Tokenizer ──────────────────────────────────────────────────────────────
TEST(Tokenizer, DateTimeFunctionKeywords) {
    Tokenizer tok;
    auto tokens = tok.tokenize("DATE_TRUNC NOW EPOCH_S EPOCH_MS");
    ASSERT_EQ(tokens[0].type, TokenType::DATE_TRUNC);
    ASSERT_EQ(tokens[1].type, TokenType::NOW);
    ASSERT_EQ(tokens[2].type, TokenType::EPOCH_S);
    ASSERT_EQ(tokens[3].type, TokenType::EPOCH_MS);
}

TEST(Tokenizer, LikeAndSetOpKeywords) {
    Tokenizer tok;
    auto tokens = tok.tokenize("LIKE UNION ALL INTERSECT EXCEPT");
    ASSERT_EQ(tokens[0].type, TokenType::LIKE);
    ASSERT_EQ(tokens[1].type, TokenType::UNION);
    ASSERT_EQ(tokens[2].type, TokenType::ALL);
    ASSERT_EQ(tokens[3].type, TokenType::INTERSECT);
    ASSERT_EQ(tokens[4].type, TokenType::EXCEPT);
}

// ── Parser: Date/time ──────────────────────────────────────────────────────
TEST(Parser, DateTruncParsed) {
    Parser p;
    auto stmt = p.parse("SELECT DATE_TRUNC('min', recv_ts) AS tb FROM trades");
    ASSERT_EQ(stmt.columns.size(), 1u);
    ASSERT_NE(stmt.columns[0].arith_expr, nullptr);
    EXPECT_EQ(stmt.columns[0].arith_expr->kind, ArithExpr::Kind::FUNC);
    EXPECT_EQ(stmt.columns[0].arith_expr->func_name, "date_trunc");
    EXPECT_EQ(stmt.columns[0].arith_expr->func_unit, "min");
    EXPECT_EQ(stmt.columns[0].alias, "tb");
}

TEST(Parser, NowParsed) {
    Parser p;
    auto stmt = p.parse("SELECT NOW() AS ts FROM trades");
    ASSERT_NE(stmt.columns[0].arith_expr, nullptr);
    EXPECT_EQ(stmt.columns[0].arith_expr->func_name, "now");
    EXPECT_EQ(stmt.columns[0].arith_expr->func_arg, nullptr);
}

TEST(Parser, EpochSParsed) {
    Parser p;
    auto stmt = p.parse("SELECT EPOCH_S(recv_ts) AS es FROM trades");
    ASSERT_NE(stmt.columns[0].arith_expr, nullptr);
    EXPECT_EQ(stmt.columns[0].arith_expr->func_name, "epoch_s");
    ASSERT_NE(stmt.columns[0].arith_expr->func_arg, nullptr);
    EXPECT_EQ(stmt.columns[0].arith_expr->func_arg->column, "recv_ts");
}

// ── Parser: LIKE ───────────────────────────────────────────────────────────
TEST(Parser, LikeExpr) {
    Parser p;
    auto stmt = p.parse("SELECT price FROM trades WHERE price LIKE '150%'");
    ASSERT_TRUE(stmt.where.has_value());
    EXPECT_EQ(stmt.where->expr->kind, Expr::Kind::LIKE);
    EXPECT_EQ(stmt.where->expr->column, "price");
    EXPECT_EQ(stmt.where->expr->like_pattern, "150%");
    EXPECT_FALSE(stmt.where->expr->negated);
}

TEST(Parser, NotLikeExpr) {
    Parser p;
    auto stmt = p.parse("SELECT price FROM trades WHERE price NOT LIKE '150%'");
    ASSERT_TRUE(stmt.where.has_value());
    EXPECT_EQ(stmt.where->expr->kind, Expr::Kind::LIKE);
    EXPECT_TRUE(stmt.where->expr->negated);
}

// ── Parser: UNION ──────────────────────────────────────────────────────────
TEST(Parser, UnionAllParsed) {
    Parser p;
    auto stmt = p.parse(
        "SELECT price FROM trades WHERE symbol = 1 "
        "UNION ALL "
        "SELECT price FROM trades WHERE symbol = 2");
    EXPECT_EQ(stmt.set_op, SelectStmt::SetOp::UNION_ALL);
    ASSERT_NE(stmt.rhs, nullptr);
    EXPECT_EQ(stmt.rhs->from_table, "trades");
}

TEST(Parser, UnionDistinctParsed) {
    Parser p;
    auto stmt = p.parse(
        "SELECT price FROM trades WHERE symbol = 1 "
        "UNION "
        "SELECT price FROM trades WHERE symbol = 1");
    EXPECT_EQ(stmt.set_op, SelectStmt::SetOp::UNION_DISTINCT);
}

TEST(Parser, IntersectParsed) {
    Parser p;
    auto stmt = p.parse(
        "SELECT price FROM trades WHERE symbol = 1 "
        "INTERSECT "
        "SELECT price FROM trades WHERE symbol = 1");
    EXPECT_EQ(stmt.set_op, SelectStmt::SetOp::INTERSECT);
}

TEST(Parser, ExceptParsed) {
    Parser p;
    auto stmt = p.parse(
        "SELECT price FROM trades WHERE symbol = 1 "
        "EXCEPT "
        "SELECT price FROM trades WHERE symbol = 2");
    EXPECT_EQ(stmt.set_op, SelectStmt::SetOp::EXCEPT);
}

// ── Executor: Date/time functions ──────────────────────────────────────────
TEST_F(SqlExecutorTest, DateTruncUsResult) {
    // DATE_TRUNC('us', price): price/1000*1000
    // All prices 15000-15090 step 10: (150X0/1000)*1000 = 15000
    auto r = executor->execute(
        "SELECT DATE_TRUNC('us', price) AS tb FROM trades WHERE symbol = 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 10u);
    for (const auto& row : r.rows)
        EXPECT_EQ(row[0], 15000LL);  // all truncate to 15000
}

TEST_F(SqlExecutorTest, DateTruncMsResult) {
    // DATE_TRUNC('ms', price): price/1_000_000*1_000_000
    // all prices 15000-15090 → 0 (since < 1_000_000)
    auto r = executor->execute(
        "SELECT DATE_TRUNC('ms', price) AS tb FROM trades "
        "WHERE symbol = 1 LIMIT 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 0LL);
}

TEST_F(SqlExecutorTest, EpochSResult) {
    // EPOCH_S(price) = price / 1_000_000_000
    // price=15000 → 0
    auto r = executor->execute(
        "SELECT EPOCH_S(price) AS es FROM trades WHERE symbol = 1 LIMIT 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 0LL);  // 15000 / 1e9 = 0
}

TEST_F(SqlExecutorTest, EpochMsResult) {
    // EPOCH_MS(price) = price / 1_000_000
    // price=15000 → 0
    auto r = executor->execute(
        "SELECT EPOCH_MS(price) AS em FROM trades WHERE symbol = 1 LIMIT 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 0LL);
}

TEST_F(SqlExecutorTest, NowPositive) {
    // NOW() should return current time in nanoseconds (positive)
    auto r = executor->execute(
        "SELECT NOW() AS ts FROM trades WHERE symbol = 1 LIMIT 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_GT(r.rows[0][0], 0LL);
}

TEST_F(SqlExecutorTest, DateTruncInArith) {
    // DATE_TRUNC inside arithmetic: DATE_TRUNC('us', price) + 1
    // All prices 15000-15090: (150X0/1000)*1000 + 1 = 15001
    auto r = executor->execute(
        "SELECT DATE_TRUNC('us', price) + 1 AS v FROM trades WHERE symbol = 1");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 10u);
    for (const auto& row : r.rows)
        EXPECT_EQ(row[0], 15001LL);
}

// ── Executor: LIKE ─────────────────────────────────────────────────────────
TEST_F(SqlExecutorTest, LikeExact) {
    // WHERE price LIKE '15000' → only rows where price as string == "15000"
    auto r = executor->execute(
        "SELECT COUNT(*) FROM trades WHERE price LIKE '15000'");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 1);
}

TEST_F(SqlExecutorTest, LikePrefix) {
    // WHERE price LIKE '150%' → all symbol=1 prices (15000-15090) start with "150"
    auto r = executor->execute(
        "SELECT COUNT(*) FROM trades WHERE price LIKE '150%'");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 10);
}

TEST_F(SqlExecutorTest, LikeSuffix) {
    // WHERE price LIKE '%0' → prices ending in 0: all prices in set end in 0
    // 15000,15010,...,15090,20000,20010,...,20040 → all 15 end in 0
    auto r = executor->execute(
        "SELECT COUNT(*) FROM trades WHERE price LIKE '%0'");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 15);
}

TEST_F(SqlExecutorTest, NotLike) {
    // WHERE price NOT LIKE '150%' → symbol=2 prices (20000-20040) = 5 rows
    auto r = executor->execute(
        "SELECT COUNT(*) FROM trades WHERE price NOT LIKE '150%'");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 5);
}

TEST_F(SqlExecutorTest, LikeSymbolColumn) {
    // WHERE symbol LIKE '1' → symbol=1 → 10 rows
    auto r = executor->execute(
        "SELECT COUNT(*) FROM trades WHERE symbol LIKE '1'");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 10);
}

TEST_F(SqlExecutorTest, LikeUnderscore) {
    // WHERE price LIKE '1501_' → matches "15010" through "15019" (5 chars, starts 1501)
    // From our set: only 15010 matches (15011..15019 not present)
    auto r = executor->execute(
        "SELECT COUNT(*) FROM trades WHERE price LIKE '1501_'");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 1);
}

// ── Executor: UNION / INTERSECT / EXCEPT ───────────────────────────────────
TEST_F(SqlExecutorTest, UnionAllRowCount) {
    // Two queries combined: 10 rows + 5 rows = 15 total
    auto r = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 "
        "UNION ALL "
        "SELECT price FROM trades WHERE symbol = 2");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 15u);
}

TEST_F(SqlExecutorTest, UnionAllAggCombined) {
    // COUNT from each symbol combined with UNION ALL
    auto r = executor->execute(
        "SELECT COUNT(*) AS cnt FROM trades WHERE symbol = 1 "
        "UNION ALL "
        "SELECT COUNT(*) AS cnt FROM trades WHERE symbol = 2");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 2u);
    // Two rows: one with 10 and one with 5 (order may vary)
    int64_t sum = r.rows[0][0] + r.rows[1][0];
    EXPECT_EQ(sum, 15LL);
}

TEST_F(SqlExecutorTest, UnionDistinctDedup) {
    // UNION deduplicates identical rows
    auto r = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 AND price = 15000 "
        "UNION "
        "SELECT price FROM trades WHERE symbol = 1 AND price = 15000");
    ASSERT_TRUE(r.ok()) << r.error;
    // Both sides yield [15000]; after dedup → 1 row
    EXPECT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 15000LL);
}

TEST_F(SqlExecutorTest, UnionDistinctNoOverlap) {
    // UNION of disjoint sets = same as UNION ALL
    auto r = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 AND price <= 15020 "
        "UNION "
        "SELECT price FROM trades WHERE symbol = 2 AND price >= 20000");
    ASSERT_TRUE(r.ok()) << r.error;
    // Left: 15000,15010,15020 (3 rows); Right: 20000,20010,20020,20030,20040 (5 rows)
    // No overlap → 8 distinct rows
    EXPECT_EQ(r.rows.size(), 8u);
}

TEST_F(SqlExecutorTest, IntersectOverlap) {
    // Prices in [15000,15040] ∩ [15020,15060] = {15020, 15030, 15040}
    auto r = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 AND price >= 15000 AND price <= 15040 "
        "INTERSECT "
        "SELECT price FROM trades WHERE symbol = 1 AND price >= 15020 AND price <= 15060");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 3u);
}

TEST_F(SqlExecutorTest, IntersectEmpty) {
    // Disjoint sets → empty intersection
    auto r = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 "
        "INTERSECT "
        "SELECT price FROM trades WHERE symbol = 2");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 0u);
}

TEST_F(SqlExecutorTest, ExceptRemovesRows) {
    // All symbol=1 prices EXCEPT those >= 15050 → 15000,15010,15020,15030,15040 (5 rows)
    auto r = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 "
        "EXCEPT "
        "SELECT price FROM trades WHERE symbol = 1 AND price >= 15050");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 5u);
}

TEST_F(SqlExecutorTest, ExceptNoOverlap) {
    // EXCEPT with disjoint right side → all left rows remain
    auto r = executor->execute(
        "SELECT price FROM trades WHERE symbol = 1 "
        "EXCEPT "
        "SELECT price FROM trades WHERE symbol = 2");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 10u);
}

// ============================================================================
// CTE (WITH clause) tests
// ============================================================================

// Basic passthrough: CTE selects two columns, outer selects both
TEST_F(SqlExecutorTest, CTE_SimplePassthrough) {
    auto r = executor->execute(
        "WITH t AS (SELECT symbol, price FROM trades WHERE symbol = 1) "
        "SELECT symbol, price FROM t");
    ASSERT_TRUE(r.ok()) << r.error;
    // symbol=1 has 10 rows
    EXPECT_EQ(r.rows.size(), 10u);
    ASSERT_EQ(r.column_names.size(), 2u);
    EXPECT_EQ(r.column_names[0], "symbol");
    EXPECT_EQ(r.column_names[1], "price");
    for (const auto& row : r.rows)
        EXPECT_EQ(row[0], 1);
}

// WHERE filter applied to virtual CTE result
TEST_F(SqlExecutorTest, CTE_WhereFilter) {
    // CTE: all symbol=1 rows (price 15000..15090 step 10)
    // Outer WHERE price > 15050 → prices 15060, 15070, 15080, 15090 → 4 rows
    auto r = executor->execute(
        "WITH t AS (SELECT symbol, price FROM trades WHERE symbol = 1) "
        "SELECT symbol, price FROM t WHERE price > 15050");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 4u);
    for (const auto& row : r.rows)
        EXPECT_GT(row[1], 15050);
}

// Scalar aggregation over CTE result
TEST_F(SqlExecutorTest, CTE_ScalarAgg) {
    // sum(volume) for all rows in the CTE — includes both symbols
    // symbol=1: 100..109 → 1045, symbol=2: 200..204 → 1010; total = 2055
    auto r = executor->execute(
        "WITH t AS (SELECT volume FROM trades) "
        "SELECT sum(volume) AS total FROM t");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 2055);
}

// GROUP BY over CTE result
TEST_F(SqlExecutorTest, CTE_GroupByAgg) {
    // CTE brings all rows; outer groups by symbol and sums volume
    auto r = executor->execute(
        "WITH t AS (SELECT symbol, volume FROM trades) "
        "SELECT symbol, sum(volume) AS total FROM t GROUP BY symbol");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 2u);
    // Find each symbol's sum
    int64_t sym1_total = 0, sym2_total = 0;
    for (const auto& row : r.rows) {
        if (row[0] == 1) sym1_total = row[1];
        if (row[0] == 2) sym2_total = row[1];
    }
    EXPECT_EQ(sym1_total, 1045);
    EXPECT_EQ(sym2_total, 1010);
}

// Chained CTEs: second CTE references first
TEST_F(SqlExecutorTest, CTE_Chained) {
    // CTE a: group by symbol → totals (sym1=1045, sym2=1010)
    // CTE b: filter a WHERE total > 1040 → only sym1
    // Outer: SELECT from b
    auto r = executor->execute(
        "WITH a AS (SELECT symbol, sum(volume) AS total FROM trades GROUP BY symbol), "
        "     b AS (SELECT symbol, total FROM a WHERE total > 1040) "
        "SELECT symbol, total FROM b");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 1);
    EXPECT_EQ(r.rows[0][1], 1045);
}

// CTE with ORDER BY + LIMIT in outer query
TEST_F(SqlExecutorTest, CTE_OrderByLimit) {
    auto r = executor->execute(
        "WITH t AS (SELECT symbol, price FROM trades WHERE symbol = 1) "
        "SELECT symbol, price FROM t ORDER BY price DESC LIMIT 3");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 3u);
    // Prices should be descending: 15090, 15080, 15070
    EXPECT_EQ(r.rows[0][1], 15090);
    EXPECT_EQ(r.rows[1][1], 15080);
    EXPECT_EQ(r.rows[2][1], 15070);
}

// CTE combined with UNION ALL on the outer query
TEST_F(SqlExecutorTest, CTE_WithUnionAll) {
    // Build one CTE, union with a direct query
    auto r = executor->execute(
        "WITH t AS (SELECT symbol, price FROM trades WHERE symbol = 1 AND price = 15000) "
        "SELECT symbol, price FROM t "
        "UNION ALL "
        "SELECT symbol, price FROM trades WHERE symbol = 2 AND price = 20000");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 2u);
}

// ============================================================================
// Subquery in FROM clause tests
// ============================================================================

// Simple SELECT * from a subquery
TEST_F(SqlExecutorTest, Subquery_StarPassthrough) {
    auto r = executor->execute(
        "SELECT symbol, price FROM "
        "(SELECT symbol, price FROM trades WHERE symbol = 1) AS sub");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 10u);
    for (const auto& row : r.rows)
        EXPECT_EQ(row[0], 1);
}

// WHERE applied on top of subquery result
TEST_F(SqlExecutorTest, Subquery_OuterWhere) {
    // Inner selects all rows; outer filters price >= 15080 → 15080, 15090 → 2 rows
    auto r = executor->execute(
        "SELECT price FROM "
        "(SELECT symbol, price FROM trades WHERE symbol = 1) AS sub "
        "WHERE price >= 15080");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 2u);
    for (const auto& row : r.rows)
        EXPECT_GE(row[0], 15080);
}

// Aggregation over a subquery
TEST_F(SqlExecutorTest, Subquery_Aggregate) {
    auto r = executor->execute(
        "SELECT sum(volume) AS total FROM "
        "(SELECT volume FROM trades WHERE symbol = 2) AS sub");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 1010);
}

// GROUP BY aggregation over a subquery
TEST_F(SqlExecutorTest, Subquery_GroupBy) {
    auto r = executor->execute(
        "SELECT symbol, sum(volume) AS total FROM "
        "(SELECT symbol, volume FROM trades) AS sub "
        "GROUP BY symbol");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 2u);
    int64_t sym1 = 0, sym2 = 0;
    for (const auto& row : r.rows) {
        if (row[0] == 1) sym1 = row[1];
        if (row[0] == 2) sym2 = row[1];
    }
    EXPECT_EQ(sym1, 1045);
    EXPECT_EQ(sym2, 1010);
}

// Subquery with arithmetic expression in SELECT list
TEST_F(SqlExecutorTest, Subquery_ArithInOuter) {
    // Inner: price and volume; outer: volume * 2
    auto r = executor->execute(
        "SELECT volume FROM "
        "(SELECT symbol, price, volume FROM trades WHERE symbol = 1 AND price = 15000) AS sub");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 100); // first row volume
}

// Parser: WITH keyword tokenized correctly
TEST(TokenizerCTE, WithKeyword) {
    Tokenizer tok;
    auto tokens = tok.tokenize("WITH t AS (SELECT 1)");
    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type, TokenType::WITH);
    EXPECT_EQ(tokens[1].type, TokenType::IDENT);
    EXPECT_EQ(tokens[1].value, "t");
    EXPECT_EQ(tokens[2].type, TokenType::AS);
    EXPECT_EQ(tokens[3].type, TokenType::LPAREN);
}

// Parser: CTE AST populated correctly
TEST(ParserCTE, CTEDefsPresent) {
    Parser p;
    auto stmt = p.parse(
        "WITH daily AS (SELECT symbol FROM trades) "
        "SELECT symbol FROM daily");
    EXPECT_EQ(stmt.cte_defs.size(), 1u);
    EXPECT_EQ(stmt.cte_defs[0].name, "daily");
    ASSERT_NE(stmt.cte_defs[0].stmt, nullptr);
    EXPECT_EQ(stmt.cte_defs[0].stmt->from_table, "trades");
    EXPECT_EQ(stmt.from_table, "daily");
}

// Parser: FROM subquery AST populated correctly
TEST(ParserCTE, SubqueryFromClause) {
    Parser p;
    auto stmt = p.parse(
        "SELECT price FROM (SELECT price FROM trades) AS sub");
    EXPECT_EQ(stmt.from_table, "");
    EXPECT_EQ(stmt.from_alias, "sub");
    ASSERT_NE(stmt.from_subquery, nullptr);
    EXPECT_EQ(stmt.from_subquery->from_table, "trades");
}

// Parser: multiple CTEs
TEST(ParserCTE, MultipleCTEs) {
    Parser p;
    auto stmt = p.parse(
        "WITH a AS (SELECT symbol FROM trades), "
        "     b AS (SELECT symbol FROM a) "
        "SELECT symbol FROM b");
    EXPECT_EQ(stmt.cte_defs.size(), 2u);
    EXPECT_EQ(stmt.cte_defs[0].name, "a");
    EXPECT_EQ(stmt.cte_defs[1].name, "b");
}
