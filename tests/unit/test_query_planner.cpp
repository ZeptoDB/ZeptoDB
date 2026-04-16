// ============================================================================
// ZeptoDB: Query Planner Tests (Phase 3-6)
// ============================================================================

#include <gtest/gtest.h>
#include "zeptodb/execution/query_planner.h"
#include "zeptodb/execution/table_statistics.h"
#include "zeptodb/sql/ast.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/core/pipeline.h"

using namespace zeptodb::execution;
using namespace zeptodb::sql;
using namespace zeptodb::core;

// ============================================================================
// LogicalPlan::build tests
// ============================================================================

TEST(QueryPlanner, BuildSimpleSelect) {
    SelectStmt stmt;
    stmt.from_table = "trades";
    SelectExpr col;
    col.column = "price";
    stmt.columns.push_back(col);

    auto root = LogicalPlan::build(stmt);
    ASSERT_NE(root, nullptr);
    // Should be PROJECT → SCAN
    EXPECT_EQ(root->type, LogicalNode::Type::PROJECT);
    ASSERT_NE(root->left, nullptr);
    EXPECT_EQ(root->left->type, LogicalNode::Type::SCAN);
    EXPECT_EQ(root->left->table_name, "trades");
}

TEST(QueryPlanner, BuildSelectWithWhere) {
    SelectStmt stmt;
    stmt.from_table = "trades";
    SelectExpr col;
    col.column = "price";
    stmt.columns.push_back(col);

    auto expr = std::make_shared<Expr>();
    expr->kind = Expr::Kind::COMPARE;
    expr->column = "symbol";
    expr->op = CompareOp::EQ;
    expr->value = 1;
    stmt.where = WhereClause{expr};

    auto root = LogicalPlan::build(stmt);
    ASSERT_NE(root, nullptr);
    // PROJECT → FILTER → SCAN
    EXPECT_EQ(root->type, LogicalNode::Type::PROJECT);
    ASSERT_NE(root->left, nullptr);
    EXPECT_EQ(root->left->type, LogicalNode::Type::FILTER);
    ASSERT_NE(root->left->left, nullptr);
    EXPECT_EQ(root->left->left->type, LogicalNode::Type::SCAN);
}

TEST(QueryPlanner, BuildSelectWithJoin) {
    SelectStmt stmt;
    stmt.from_table = "trades";
    SelectExpr col;
    col.column = "price";
    stmt.columns.push_back(col);

    JoinClause jc;
    jc.type = JoinClause::Type::INNER;
    jc.table = "quotes";
    JoinCondition cond;
    cond.left_col = "symbol";
    cond.right_col = "symbol";
    jc.on_conditions.push_back(cond);
    stmt.join = jc;

    auto root = LogicalPlan::build(stmt);
    ASSERT_NE(root, nullptr);
    // PROJECT → JOIN → (SCAN, SCAN)
    EXPECT_EQ(root->type, LogicalNode::Type::PROJECT);
    ASSERT_NE(root->left, nullptr);
    EXPECT_EQ(root->left->type, LogicalNode::Type::JOIN);
    ASSERT_NE(root->left->left, nullptr);
    EXPECT_EQ(root->left->left->type, LogicalNode::Type::SCAN);
    EXPECT_EQ(root->left->left->table_name, "trades");
    ASSERT_NE(root->left->right, nullptr);
    EXPECT_EQ(root->left->right->type, LogicalNode::Type::SCAN);
    EXPECT_EQ(root->left->right->table_name, "quotes");
}

TEST(QueryPlanner, BuildSelectWithGroupBy) {
    SelectStmt stmt;
    stmt.from_table = "trades";
    SelectExpr col;
    col.agg = AggFunc::COUNT;
    col.column = "*";
    stmt.columns.push_back(col);

    GroupByClause gb;
    gb.columns.push_back("symbol");
    stmt.group_by = gb;

    auto root = LogicalPlan::build(stmt);
    ASSERT_NE(root, nullptr);
    // AGGREGATE → SCAN
    EXPECT_EQ(root->type, LogicalNode::Type::AGGREGATE);
    ASSERT_NE(root->left, nullptr);
    EXPECT_EQ(root->left->type, LogicalNode::Type::SCAN);
    EXPECT_EQ(root->group_keys.size(), 1u);
    EXPECT_EQ(root->group_keys[0], "symbol");
}

TEST(QueryPlanner, BuildSelectWithSortAndLimit) {
    SelectStmt stmt;
    stmt.from_table = "trades";
    SelectExpr col;
    col.column = "price";
    stmt.columns.push_back(col);

    OrderByClause ob;
    OrderByItem item;
    item.column = "price";
    item.asc = false;
    ob.items.push_back(item);
    stmt.order_by = ob;
    stmt.limit = 10;

    auto root = LogicalPlan::build(stmt);
    ASSERT_NE(root, nullptr);
    // LIMIT → SORT → PROJECT → SCAN
    EXPECT_EQ(root->type, LogicalNode::Type::LIMIT);
    EXPECT_EQ(root->limit_n, 10);
    ASSERT_NE(root->left, nullptr);
    EXPECT_EQ(root->left->type, LogicalNode::Type::SORT);
    ASSERT_NE(root->left->left, nullptr);
    EXPECT_EQ(root->left->left->type, LogicalNode::Type::PROJECT);
}

// ============================================================================
// LogicalPlan::optimize — predicate pushdown
// ============================================================================

TEST(QueryPlanner, PredicatePushdownBelowJoin) {
    SelectStmt stmt;
    stmt.from_table = "trades";
    SelectExpr col;
    col.column = "price";
    stmt.columns.push_back(col);

    JoinClause jc;
    jc.type = JoinClause::Type::INNER;
    jc.table = "quotes";
    JoinCondition cond;
    cond.left_col = "symbol";
    cond.right_col = "symbol";
    jc.on_conditions.push_back(cond);
    stmt.join = jc;

    // WHERE trades.symbol = 1 (references only left side)
    auto expr = std::make_shared<Expr>();
    expr->kind = Expr::Kind::COMPARE;
    expr->table_alias = "trades";
    expr->column = "symbol";
    expr->op = CompareOp::EQ;
    expr->value = 1;
    stmt.where = WhereClause{expr};

    auto root = LogicalPlan::build(stmt);
    LogicalPlan::optimize(root);

    // After pushdown: PROJECT → JOIN → (FILTER → SCAN[trades], SCAN[quotes])
    EXPECT_EQ(root->type, LogicalNode::Type::PROJECT);
    ASSERT_NE(root->left, nullptr);
    EXPECT_EQ(root->left->type, LogicalNode::Type::JOIN);

    // Left child of JOIN should now be FILTER → SCAN
    auto& join_left = root->left->left;
    ASSERT_NE(join_left, nullptr);
    EXPECT_EQ(join_left->type, LogicalNode::Type::FILTER);
    ASSERT_NE(join_left->left, nullptr);
    EXPECT_EQ(join_left->left->type, LogicalNode::Type::SCAN);
    EXPECT_EQ(join_left->left->table_name, "trades");

    // Right child of JOIN should still be plain SCAN
    auto& join_right = root->left->right;
    ASSERT_NE(join_right, nullptr);
    EXPECT_EQ(join_right->type, LogicalNode::Type::SCAN);
    EXPECT_EQ(join_right->table_name, "quotes");
}

// ============================================================================
// PhysicalPlanner tests
// ============================================================================

TEST(QueryPlanner, PhysicalPlanSeqScan) {
    SelectStmt stmt;
    stmt.from_table = "trades";
    SelectExpr col;
    col.column = "price";
    stmt.columns.push_back(col);

    auto logical = LogicalPlan::build(stmt);
    LogicalPlan::optimize(logical);

    TableStatistics stats;
    auto physical = PhysicalPlanner::plan(logical, stats);
    ASSERT_NE(physical, nullptr);

    // Walk to find the SCAN node
    auto node = physical;
    while (node && node->type != PhysicalNode::Type::SEQ_SCAN)
        node = node->left;
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->type, PhysicalNode::Type::SEQ_SCAN);
    EXPECT_EQ(node->table_name, "trades");
}

TEST(QueryPlanner, PhysicalPlanHashJoinBuildSide) {
    // Build a JOIN logical plan
    SelectStmt stmt;
    stmt.from_table = "trades";
    SelectExpr col;
    col.column = "price";
    stmt.columns.push_back(col);

    JoinClause jc;
    jc.type = JoinClause::Type::INNER;
    jc.table = "quotes";
    JoinCondition cond;
    cond.left_col = "symbol";
    cond.right_col = "symbol";
    jc.on_conditions.push_back(cond);
    stmt.join = jc;

    auto logical = LogicalPlan::build(stmt);
    LogicalPlan::optimize(logical);

    TableStatistics stats;
    auto physical = PhysicalPlanner::plan(logical, stats);
    ASSERT_NE(physical, nullptr);

    // Find the HASH_JOIN node
    auto node = physical;
    while (node && node->type != PhysicalNode::Type::HASH_JOIN)
        node = node->left;
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->type, PhysicalNode::Type::HASH_JOIN);
    // Both sides have same estimated rows (both empty stats → 1 row each)
    // so build_right should be false (left is build by default when equal)
    EXPECT_FALSE(node->build_right);
}

TEST(QueryPlanner, PhysicalPlanAsofJoin) {
    SelectStmt stmt;
    stmt.from_table = "trades";
    SelectExpr col;
    col.column = "price";
    stmt.columns.push_back(col);

    JoinClause jc;
    jc.type = JoinClause::Type::ASOF;
    jc.table = "quotes";
    stmt.join = jc;

    auto logical = LogicalPlan::build(stmt);
    TableStatistics stats;
    auto physical = PhysicalPlanner::plan(logical, stats);

    // Find ASOF_JOIN node
    auto node = physical;
    while (node && node->type != PhysicalNode::Type::ASOF_JOIN)
        node = node->left;
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->type, PhysicalNode::Type::ASOF_JOIN);
}

TEST(QueryPlanner, PhysicalPlanTopNSort) {
    SelectStmt stmt;
    stmt.from_table = "trades";
    SelectExpr col;
    col.column = "price";
    stmt.columns.push_back(col);

    OrderByClause ob;
    OrderByItem item;
    item.column = "price";
    ob.items.push_back(item);
    stmt.order_by = ob;
    stmt.limit = 5;

    auto logical = LogicalPlan::build(stmt);
    TableStatistics stats;
    auto physical = PhysicalPlanner::plan(logical, stats);

    // LIMIT → child should be TOPN_SORT (upgraded from FULL_SORT)
    ASSERT_NE(physical, nullptr);
    EXPECT_EQ(physical->type, PhysicalNode::Type::LIMIT);
    ASSERT_NE(physical->left, nullptr);
    EXPECT_EQ(physical->left->type, PhysicalNode::Type::TOPN_SORT);
}

// ============================================================================
// PhysicalNode::describe tests
// ============================================================================

TEST(QueryPlanner, DescribeSeqScan) {
    PhysicalNode node;
    node.type = PhysicalNode::Type::SEQ_SCAN;
    node.table_name = "trades";
    node.cost = {10.0, 0.0, 1000};
    node.est_rows = 1000;
    auto desc = node.describe();
    EXPECT_NE(desc.find("SeqScan"), std::string::npos);
    EXPECT_NE(desc.find("trades"), std::string::npos);
    EXPECT_NE(desc.find("1000"), std::string::npos);
}

TEST(QueryPlanner, DescribeHashJoin) {
    PhysicalNode node;
    node.type = PhysicalNode::Type::HASH_JOIN;
    node.build_right = true;
    node.cost = {0.0, 100.0, 5000};
    node.est_rows = 5000;
    auto desc = node.describe();
    EXPECT_NE(desc.find("HashJoin"), std::string::npos);
    EXPECT_NE(desc.find("build=right"), std::string::npos);
}

// ============================================================================
// needs_cost_planning tests
// ============================================================================

TEST(QueryPlanner, NeedsCostPlanningSimple) {
    SelectStmt stmt;
    stmt.from_table = "trades";
    EXPECT_FALSE(QueryExecutor::needs_cost_planning(stmt));
}

TEST(QueryPlanner, NeedsCostPlanningJoin) {
    SelectStmt stmt;
    stmt.from_table = "trades";
    JoinClause jc;
    jc.type = JoinClause::Type::INNER;
    jc.table = "quotes";
    stmt.join = jc;
    EXPECT_TRUE(QueryExecutor::needs_cost_planning(stmt));
}

TEST(QueryPlanner, NeedsCostPlanningSubquery) {
    SelectStmt stmt;
    stmt.from_subquery = std::make_shared<SelectStmt>();
    EXPECT_TRUE(QueryExecutor::needs_cost_planning(stmt));
}

TEST(QueryPlanner, NeedsCostPlanningCTE) {
    SelectStmt stmt;
    stmt.from_table = "trades";
    CTEDef cte;
    cte.name = "cte1";
    cte.stmt = std::make_shared<SelectStmt>();
    stmt.cte_defs.push_back(cte);
    EXPECT_TRUE(QueryExecutor::needs_cost_planning(stmt));
}

TEST(QueryPlanner, NeedsCostPlanningSetOp) {
    SelectStmt stmt;
    stmt.from_table = "trades";
    stmt.set_op = SelectStmt::SetOp::UNION_ALL;
    EXPECT_TRUE(QueryExecutor::needs_cost_planning(stmt));
}

// ============================================================================
// EXPLAIN output test (end-to-end with pipeline)
// ============================================================================

class CostPlanExplainTest : public ::testing::Test {
protected:
    void SetUp() override {
        PipelineConfig cfg;
        cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
        pipeline = std::make_unique<ZeptoPipeline>(cfg);
        executor = std::make_unique<QueryExecutor>(*pipeline);

        // Insert trades data
        for (int i = 0; i < 10; ++i) {
            zeptodb::ingestion::TickMessage msg{};
            msg.symbol_id = 1;
            msg.recv_ts   = 1000LL + i;
            msg.price     = 15000 + i * 10;
            msg.volume    = 100 + i;
            msg.msg_type  = 0;
            pipeline->ingest_tick(msg);
        }
        pipeline->drain_sync(100);
    }

    std::unique_ptr<ZeptoPipeline>  pipeline;
    std::unique_ptr<QueryExecutor>  executor;
};

TEST_F(CostPlanExplainTest, ExplainJoinShowsCostPlan) {
    auto result = executor->execute(
        "EXPLAIN SELECT t.price, q.price "
        "FROM trades t INNER JOIN trades q ON t.symbol = q.symbol");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_FALSE(result.string_rows.empty());

    // Should contain cost-based plan output
    bool found_hash_join = false;
    bool found_cost = false;
    bool found_seq_scan = false;
    for (const auto& line : result.string_rows) {
        if (line.find("HashJoin") != std::string::npos) found_hash_join = true;
        if (line.find("cost:") != std::string::npos) found_cost = true;
        if (line.find("SeqScan") != std::string::npos) found_seq_scan = true;
    }
    EXPECT_TRUE(found_hash_join) << "Expected HashJoin in EXPLAIN output";
    EXPECT_TRUE(found_cost) << "Expected cost estimates in EXPLAIN output";
    EXPECT_TRUE(found_seq_scan) << "Expected SeqScan in EXPLAIN output";
}

TEST_F(CostPlanExplainTest, ExplainSimpleKeepsOldFormat) {
    auto result = executor->execute(
        "EXPLAIN SELECT price, volume FROM trades WHERE symbol = 1");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_FALSE(result.string_rows.empty());
    // Simple query should use old EXPLAIN format (no HashJoin)
    EXPECT_NE(result.string_rows[0].find("TableScan"), std::string::npos);
}

// ============================================================================
// format_explain_tree test
// ============================================================================

TEST(QueryPlanner, FormatExplainTree) {
    auto join = std::make_shared<PhysicalNode>();
    join->type = PhysicalNode::Type::HASH_JOIN;
    join->build_right = true;
    join->cost = {100.0, 50.0, 5000};
    join->est_rows = 5000;

    auto left_scan = std::make_shared<PhysicalNode>();
    left_scan->type = PhysicalNode::Type::SEQ_SCAN;
    left_scan->table_name = "trades";
    left_scan->cost = {50.0, 0.0, 1000};
    left_scan->est_rows = 1000;

    auto right_scan = std::make_shared<PhysicalNode>();
    right_scan->type = PhysicalNode::Type::SEQ_SCAN;
    right_scan->table_name = "quotes";
    right_scan->cost = {25.0, 0.0, 500};
    right_scan->est_rows = 500;

    join->left = left_scan;
    join->right = right_scan;

    std::vector<std::string> lines;
    format_explain_tree(join, lines);

    ASSERT_GE(lines.size(), 3u);
    EXPECT_NE(lines[0].find("HashJoin"), std::string::npos);
    EXPECT_NE(lines[0].find("build=right"), std::string::npos);

    // Children should be indented
    bool found_trades = false, found_quotes = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].find("trades") != std::string::npos) found_trades = true;
        if (lines[i].find("quotes") != std::string::npos) found_quotes = true;
    }
    EXPECT_TRUE(found_trades);
    EXPECT_TRUE(found_quotes);
}
