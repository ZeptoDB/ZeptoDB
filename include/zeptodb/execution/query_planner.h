#pragma once
// ============================================================================
// Layer 3: Query Planner — Logical → Physical Plan Translation
// ============================================================================
// Design doc: docs/design/cost_based_planner.md §3.3–3.4
//   - Phase 3: LogicalNode tree + rule-based optimizations
//   - Phase 4: PhysicalNode tree + cost-based plan selection
//   - 2-tier adaptive routing: simple queries skip planning entirely
// ============================================================================

#include "zeptodb/execution/cost_model.h"
#include "zeptodb/execution/table_statistics.h"
#include "zeptodb/sql/ast.h"
#include <memory>
#include <string>
#include <vector>

namespace zeptodb::execution {

// ============================================================================
// LogicalNode — tree of logical operators
// ============================================================================
struct LogicalNode {
    enum class Type { SCAN, FILTER, PROJECT, AGGREGATE, JOIN, SORT, LIMIT, WINDOW };
    Type type;

    // SCAN
    std::string table_name;
    std::vector<std::string> scan_columns;  // projection pushdown

    // FILTER
    std::shared_ptr<sql::Expr> predicate;

    // AGGREGATE
    std::vector<sql::SelectExpr> agg_exprs;
    std::vector<std::string> group_keys;

    // JOIN
    sql::JoinClause::Type join_type = sql::JoinClause::Type::INNER;
    std::vector<sql::JoinCondition> join_conds;

    // SORT
    std::vector<sql::OrderByItem> order_items;

    // LIMIT
    int64_t limit_n = -1;
    int64_t offset_n = 0;

    // Children
    std::shared_ptr<LogicalNode> left;
    std::shared_ptr<LogicalNode> right;  // for JOIN

    // Cost estimate (filled by optimizer)
    CostEstimate cost;
    size_t est_rows = 0;
};

// ============================================================================
// LogicalPlan — build and optimize logical plan from AST
// ============================================================================
class LogicalPlan {
public:
    /// Build logical plan tree from a parsed SELECT statement
    static std::shared_ptr<LogicalNode> build(const sql::SelectStmt& stmt);

    /// Apply rule-based optimizations (predicate pushdown, projection pushdown)
    static void optimize(std::shared_ptr<LogicalNode>& root);

private:
    static void pushdown_predicates(std::shared_ptr<LogicalNode>& node);
    static void pushdown_projections(std::shared_ptr<LogicalNode>& node,
                                     const std::vector<std::string>& needed);
};

// ============================================================================
// PhysicalNode — tree of physical operators with cost estimates
// ============================================================================
struct PhysicalNode {
    enum class Type {
        SEQ_SCAN, INDEX_SCAN, SORTED_RANGE_SCAN,
        HASH_JOIN, ASOF_JOIN, WINDOW_JOIN,
        HASH_AGGREGATE, SORT_AGGREGATE,
        FULL_SORT, TOPN_SORT,
        FILTER, PROJECT, LIMIT
    };
    Type type;
    CostEstimate cost;
    size_t est_rows = 0;

    // Scan details
    std::string table_name;
    std::string index_column;  // for INDEX_SCAN / SORTED_RANGE_SCAN
    double selectivity = 1.0;

    // JOIN: which side is build
    bool build_right = false;

    // Children
    std::shared_ptr<PhysicalNode> left;
    std::shared_ptr<PhysicalNode> right;

    /// Single-line description for EXPLAIN output
    std::string describe() const;
};

// ============================================================================
// PhysicalPlanner — convert logical plan to physical plan using cost model
// ============================================================================
class PhysicalPlanner {
public:
    /// Convert logical plan to physical plan using cost model + statistics
    static std::shared_ptr<PhysicalNode> plan(
        const std::shared_ptr<LogicalNode>& logical,
        const TableStatistics& stats);

private:
    static std::shared_ptr<PhysicalNode> plan_scan(
        const std::shared_ptr<LogicalNode>& node,
        const TableStatistics& stats);

    static std::shared_ptr<PhysicalNode> plan_join(
        const std::shared_ptr<LogicalNode>& node,
        const std::shared_ptr<PhysicalNode>& left,
        const std::shared_ptr<PhysicalNode>& right);
};

// ============================================================================
// EXPLAIN helper — format physical plan tree as indented text lines
// ============================================================================
void format_explain_tree(const std::shared_ptr<PhysicalNode>& node,
                         std::vector<std::string>& out,
                         const std::string& prefix = "",
                         bool is_last = true);

} // namespace zeptodb::execution
