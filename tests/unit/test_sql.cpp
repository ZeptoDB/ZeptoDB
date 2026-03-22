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
// Part 4: ASOF JOIN 정확성 테스트
// ============================================================================

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
// Part 5: HTTP 엔드포인트 테스트 (선택적)
// ============================================================================
// HTTP 서버 테스트는 통합 테스트로 분리
// (빌드 시간 + 포트 충돌 고려하여 단독 실행)
