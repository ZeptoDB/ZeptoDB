// ============================================================================
// Layer 3: Query Planner — Logical/Physical Plan Implementation
// ============================================================================
// Design doc: docs/design/cost_based_planner.md §3.3–3.4
// ============================================================================

#include "zeptodb/execution/query_planner.h"
#include "zeptodb/common/logger.h"
#include <algorithm>
#include <cstdio>
#include <unordered_set>

namespace zeptodb::execution {

// ============================================================================
// Helper: collect column names referenced in an Expr tree
// ============================================================================
static void collect_expr_columns(const std::shared_ptr<sql::Expr>& expr,
                                 std::unordered_set<std::string>& cols) {
    if (!expr) return;
    if (expr->kind == sql::Expr::Kind::COMPARE ||
        expr->kind == sql::Expr::Kind::BETWEEN ||
        expr->kind == sql::Expr::Kind::IN ||
        expr->kind == sql::Expr::Kind::IS_NULL ||
        expr->kind == sql::Expr::Kind::LIKE) {
        if (!expr->column.empty()) cols.insert(expr->column);
    }
    collect_expr_columns(expr->left, cols);
    collect_expr_columns(expr->right, cols);
}

// Helper: check if an Expr references only columns from a given table alias
static bool expr_references_only(const std::shared_ptr<sql::Expr>& expr,
                                 const std::string& table) {
    if (!expr) return true;
    if (expr->kind == sql::Expr::Kind::AND || expr->kind == sql::Expr::Kind::OR ||
        expr->kind == sql::Expr::Kind::NOT) {
        return expr_references_only(expr->left, table) &&
               expr_references_only(expr->right, table);
    }
    // Leaf predicate: check table_alias
    if (!expr->table_alias.empty()) return expr->table_alias == table;
    // No alias — ambiguous, don't push down
    return false;
}

// ============================================================================
// LogicalPlan::build — AST → logical operator tree
// ============================================================================
std::shared_ptr<LogicalNode> LogicalPlan::build(const sql::SelectStmt& stmt) {
    std::shared_ptr<LogicalNode> root;

    // Base: SCAN node(s)
    if (stmt.join.has_value()) {
        // JOIN: two SCANs → JOIN
        auto left_scan = std::make_shared<LogicalNode>();
        left_scan->type = LogicalNode::Type::SCAN;
        left_scan->table_name = stmt.from_table;

        auto right_scan = std::make_shared<LogicalNode>();
        right_scan->type = LogicalNode::Type::SCAN;
        right_scan->table_name = stmt.join->table;

        auto join_node = std::make_shared<LogicalNode>();
        join_node->type = LogicalNode::Type::JOIN;
        join_node->join_type = stmt.join->type;
        join_node->join_conds = stmt.join->on_conditions;
        join_node->left = left_scan;
        join_node->right = right_scan;
        root = join_node;
    } else {
        auto scan = std::make_shared<LogicalNode>();
        scan->type = LogicalNode::Type::SCAN;
        scan->table_name = stmt.from_table;
        root = scan;
    }

    // FILTER (WHERE)
    if (stmt.where.has_value() && stmt.where->expr) {
        auto filter = std::make_shared<LogicalNode>();
        filter->type = LogicalNode::Type::FILTER;
        filter->predicate = stmt.where->expr;
        filter->left = root;
        root = filter;
    }

    // AGGREGATE (GROUP BY or aggregate functions)
    bool has_agg = false;
    for (const auto& col : stmt.columns)
        if (col.agg != sql::AggFunc::NONE) { has_agg = true; break; }

    if (has_agg || stmt.group_by.has_value()) {
        auto agg = std::make_shared<LogicalNode>();
        agg->type = LogicalNode::Type::AGGREGATE;
        agg->agg_exprs = stmt.columns;
        if (stmt.group_by.has_value())
            agg->group_keys = stmt.group_by->columns;
        agg->left = root;
        root = agg;
    } else {
        // PROJECT (non-aggregate SELECT list)
        auto proj = std::make_shared<LogicalNode>();
        proj->type = LogicalNode::Type::PROJECT;
        for (const auto& col : stmt.columns)
            if (!col.column.empty()) proj->scan_columns.push_back(col.column);
        proj->left = root;
        root = proj;
    }

    // SORT (ORDER BY)
    if (stmt.order_by.has_value()) {
        auto sort = std::make_shared<LogicalNode>();
        sort->type = LogicalNode::Type::SORT;
        sort->order_items = stmt.order_by->items;
        sort->left = root;
        root = sort;
    }

    // LIMIT
    if (stmt.limit.has_value()) {
        auto limit = std::make_shared<LogicalNode>();
        limit->type = LogicalNode::Type::LIMIT;
        limit->limit_n = *stmt.limit;
        limit->offset_n = stmt.offset.value_or(0);
        limit->left = root;
        root = limit;
    }

    return root;
}

// ============================================================================
// LogicalPlan::optimize — rule-based optimizations
// ============================================================================
void LogicalPlan::optimize(std::shared_ptr<LogicalNode>& root) {
    pushdown_predicates(root);

    // Collect needed columns for projection pushdown
    std::vector<std::string> needed;
    // Walk tree to collect all referenced columns
    std::function<void(const std::shared_ptr<LogicalNode>&)> collect;
    collect = [&](const std::shared_ptr<LogicalNode>& n) {
        if (!n) return;
        if (n->type == LogicalNode::Type::PROJECT || n->type == LogicalNode::Type::SCAN) {
            for (const auto& c : n->scan_columns)
                needed.push_back(c);
        }
        if (n->type == LogicalNode::Type::AGGREGATE) {
            for (const auto& e : n->agg_exprs)
                if (!e.column.empty()) needed.push_back(e.column);
            for (const auto& k : n->group_keys)
                needed.push_back(k);
        }
        if (n->type == LogicalNode::Type::FILTER && n->predicate) {
            std::unordered_set<std::string> cols;
            collect_expr_columns(n->predicate, cols);
            for (const auto& c : cols) needed.push_back(c);
        }
        if (n->type == LogicalNode::Type::JOIN) {
            for (const auto& jc : n->join_conds) {
                needed.push_back(jc.left_col);
                needed.push_back(jc.right_col);
            }
        }
        if (n->type == LogicalNode::Type::SORT) {
            for (const auto& o : n->order_items)
                needed.push_back(o.column);
        }
        collect(n->left);
        collect(n->right);
    };
    collect(root);

    pushdown_projections(root, needed);
}

// ============================================================================
// Predicate pushdown: move FILTER below JOIN when it references only one side
// ============================================================================
void LogicalPlan::pushdown_predicates(std::shared_ptr<LogicalNode>& node) {
    if (!node) return;

    // Recurse first
    pushdown_predicates(node->left);
    pushdown_predicates(node->right);

    // Pattern: FILTER → JOIN → push filter to the appropriate side
    if (node->type == LogicalNode::Type::FILTER && node->left &&
        node->left->type == LogicalNode::Type::JOIN) {
        auto& join = node->left;
        const auto& pred = node->predicate;

        // Determine left/right table names
        std::string left_table;
        std::string right_table;
        if (join->left) left_table = join->left->table_name;
        if (join->right) right_table = join->right->table_name;

        // Check if predicate references only left side
        if (!left_table.empty() && expr_references_only(pred, left_table)) {
            auto new_filter = std::make_shared<LogicalNode>();
            new_filter->type = LogicalNode::Type::FILTER;
            new_filter->predicate = pred;
            new_filter->left = join->left;
            join->left = new_filter;
            // Remove the original FILTER node — replace with JOIN
            node = join;
            ZEPTO_INFO("QueryPlanner: pushed predicate below JOIN to left side ({})", left_table);
            return;
        }

        // Check if predicate references only right side
        if (!right_table.empty() && expr_references_only(pred, right_table)) {
            auto new_filter = std::make_shared<LogicalNode>();
            new_filter->type = LogicalNode::Type::FILTER;
            new_filter->predicate = pred;
            new_filter->left = join->right;
            join->right = new_filter;
            node = join;
            ZEPTO_INFO("QueryPlanner: pushed predicate below JOIN to right side ({})", right_table);
            return;
        }
    }
}

// ============================================================================
// Projection pushdown: set scan_columns on SCAN nodes
// For JOIN queries, each SCAN only gets columns belonging to its table.
// ============================================================================
void LogicalPlan::pushdown_projections(std::shared_ptr<LogicalNode>& node,
                                       const std::vector<std::string>& needed) {
    if (!node) return;
    if (node->type == LogicalNode::Type::SCAN) {
        // Deduplicate
        std::unordered_set<std::string> uniq(needed.begin(), needed.end());
        node->scan_columns.assign(uniq.begin(), uniq.end());
        return;
    }
    // For JOIN nodes, split needed columns by table alias from join conditions
    if (node->type == LogicalNode::Type::JOIN) {
        // Collect columns that belong to left vs right based on join conditions
        std::unordered_set<std::string> left_cols, right_cols;
        std::string left_table = node->left ? node->left->table_name : "";
        std::string right_table = node->right ? node->right->table_name : "";

        for (const auto& jc : node->join_conds) {
            left_cols.insert(jc.left_col);
            right_cols.insert(jc.right_col);
        }
        // For other needed columns, we don't know which table they belong to,
        // so add them to both sides (safe over-approximation)
        for (const auto& c : needed) {
            left_cols.insert(c);
            right_cols.insert(c);
        }

        std::vector<std::string> left_vec(left_cols.begin(), left_cols.end());
        std::vector<std::string> right_vec(right_cols.begin(), right_cols.end());
        pushdown_projections(node->left, left_vec);
        pushdown_projections(node->right, right_vec);
        return;
    }
    pushdown_projections(node->left, needed);
    pushdown_projections(node->right, needed);
}

// ============================================================================
// PhysicalPlanner::plan — logical → physical with cost-based decisions
// ============================================================================
std::shared_ptr<PhysicalNode> PhysicalPlanner::plan(
    const std::shared_ptr<LogicalNode>& logical,
    const TableStatistics& stats) {
    if (!logical) return nullptr;

    switch (logical->type) {
    case LogicalNode::Type::SCAN:
        return plan_scan(logical, stats);

    case LogicalNode::Type::FILTER: {
        auto child = plan(logical->left, stats);
        auto node = std::make_shared<PhysicalNode>();
        node->type = PhysicalNode::Type::FILTER;
        node->left = child;
        size_t child_rows = child ? child->est_rows : 0;
        // Apply default selectivity (0.33) to reduce estimated rows
        constexpr double DEFAULT_FILTER_SEL = 0.33;
        node->selectivity = DEFAULT_FILTER_SEL;
        node->est_rows = static_cast<size_t>(child_rows * DEFAULT_FILTER_SEL + 0.5);
        if (node->est_rows == 0 && child_rows > 0) node->est_rows = 1;
        node->cost = child ? child->cost : CostEstimate{};
        node->cost.cpu_cost += child_rows * 0.1;  // filter eval cost
        return node;
    }

    case LogicalNode::Type::PROJECT: {
        auto child = plan(logical->left, stats);
        auto node = std::make_shared<PhysicalNode>();
        node->type = PhysicalNode::Type::PROJECT;
        node->left = child;
        node->est_rows = child ? child->est_rows : 0;
        node->cost = child ? child->cost : CostEstimate{};
        return node;
    }

    case LogicalNode::Type::AGGREGATE: {
        auto child = plan(logical->left, stats);
        auto node = std::make_shared<PhysicalNode>();
        node->type = PhysicalNode::Type::HASH_AGGREGATE;
        node->left = child;
        size_t in_rows = child ? child->est_rows : 0;
        size_t groups = logical->group_keys.empty() ? 1 : std::max<size_t>(1, in_rows / 10);
        node->cost = CostModel::estimate_aggregate(in_rows, groups);
        node->est_rows = node->cost.est_rows;
        // Add child cost
        if (child) {
            node->cost.io_cost += child->cost.io_cost;
            node->cost.cpu_cost += child->cost.cpu_cost;
        }
        return node;
    }

    case LogicalNode::Type::JOIN: {
        auto left_phys = plan(logical->left, stats);
        auto right_phys = plan(logical->right, stats);
        return plan_join(logical, left_phys, right_phys);
    }

    case LogicalNode::Type::SORT: {
        auto child = plan(logical->left, stats);
        auto node = std::make_shared<PhysicalNode>();
        size_t rows = child ? child->est_rows : 0;
        // SORT with LIMIT → TOPN_SORT (check if parent is LIMIT)
        // We don't have parent info here, so default to FULL_SORT.
        // The caller can upgrade to TOPN_SORT if LIMIT is present.
        node->type = PhysicalNode::Type::FULL_SORT;
        node->left = child;
        node->cost = CostModel::estimate_sort(rows);
        if (child) {
            node->cost.io_cost += child->cost.io_cost;
            node->cost.cpu_cost += child->cost.cpu_cost;
        }
        node->est_rows = rows;
        return node;
    }

    case LogicalNode::Type::LIMIT: {
        auto child = plan(logical->left, stats);
        // If child is FULL_SORT, upgrade to TOPN_SORT
        if (child && child->type == PhysicalNode::Type::FULL_SORT && logical->limit_n > 0) {
            child->type = PhysicalNode::Type::TOPN_SORT;
        }
        auto node = std::make_shared<PhysicalNode>();
        node->type = PhysicalNode::Type::LIMIT;
        node->left = child;
        size_t child_rows = child ? child->est_rows : 0;
        node->est_rows = (logical->limit_n >= 0)
            ? std::min<size_t>(static_cast<size_t>(logical->limit_n), child_rows)
            : child_rows;
        node->cost = child ? child->cost : CostEstimate{};
        return node;
    }

    case LogicalNode::Type::WINDOW: {
        auto child = plan(logical->left, stats);
        auto node = std::make_shared<PhysicalNode>();
        node->type = PhysicalNode::Type::PROJECT;  // window treated as project
        node->left = child;
        node->est_rows = child ? child->est_rows : 0;
        node->cost = child ? child->cost : CostEstimate{};
        return node;
    }
    }
    return nullptr;
}

// ============================================================================
// PhysicalPlanner::plan_scan — choose scan method
// ============================================================================
std::shared_ptr<PhysicalNode> PhysicalPlanner::plan_scan(
    const std::shared_ptr<LogicalNode>& node,
    const TableStatistics& stats) {
    auto phys = std::make_shared<PhysicalNode>();
    phys->table_name = node->table_name;

    size_t rows = stats.estimate_rows(node->table_name);
    if (rows == 0) rows = 1;  // avoid zero-division

    size_t num_cols = node->scan_columns.empty() ? 1 : node->scan_columns.size();

    // Default: sequential scan
    phys->type = PhysicalNode::Type::SEQ_SCAN;
    phys->cost = CostModel::estimate_seq_scan(rows, num_cols);
    phys->est_rows = rows;
    phys->selectivity = 1.0;

    return phys;
}

// ============================================================================
// PhysicalPlanner::plan_join — choose join algorithm and build side
// ============================================================================
std::shared_ptr<PhysicalNode> PhysicalPlanner::plan_join(
    const std::shared_ptr<LogicalNode>& node,
    const std::shared_ptr<PhysicalNode>& left,
    const std::shared_ptr<PhysicalNode>& right) {
    auto phys = std::make_shared<PhysicalNode>();

    // Choose algorithm based on join type
    if (node->join_type == sql::JoinClause::Type::ASOF ||
        node->join_type == sql::JoinClause::Type::AJ0) {
        phys->type = PhysicalNode::Type::ASOF_JOIN;
    } else if (node->join_type == sql::JoinClause::Type::WINDOW) {
        phys->type = PhysicalNode::Type::WINDOW_JOIN;
    } else {
        phys->type = PhysicalNode::Type::HASH_JOIN;
    }

    size_t left_rows = left ? left->est_rows : 0;
    size_t right_rows = right ? right->est_rows : 0;

    // For HASH_JOIN, pick smaller side as build
    if (phys->type == PhysicalNode::Type::HASH_JOIN && right_rows < left_rows) {
        phys->build_right = true;
        phys->left = left;
        phys->right = right;
        phys->cost = CostModel::estimate_hash_join(right_rows, left_rows);
    } else {
        phys->build_right = false;
        phys->left = left;
        phys->right = right;
        if (phys->type == PhysicalNode::Type::HASH_JOIN) {
            phys->cost = CostModel::estimate_hash_join(left_rows, right_rows);
        } else {
            // ASOF/WINDOW: use hash join cost as approximation
            phys->cost = CostModel::estimate_hash_join(left_rows, right_rows);
        }
    }

    // Add child costs
    if (left) {
        phys->cost.io_cost += left->cost.io_cost;
        phys->cost.cpu_cost += left->cost.cpu_cost;
    }
    if (right) {
        phys->cost.io_cost += right->cost.io_cost;
        phys->cost.cpu_cost += right->cost.cpu_cost;
    }

    phys->est_rows = std::min(left_rows, right_rows);
    // ASOF JOIN: output = left_rows (each left row matches at most one right)
    if (phys->type == PhysicalNode::Type::ASOF_JOIN)
        phys->est_rows = left_rows;
    return phys;
}

// ============================================================================
// PhysicalNode::describe — single-line description
// ============================================================================
std::string PhysicalNode::describe() const {
    char buf[256];
    const char* type_str = "";
    switch (type) {
    case Type::SEQ_SCAN:          type_str = "SeqScan"; break;
    case Type::INDEX_SCAN:        type_str = "IndexScan"; break;
    case Type::SORTED_RANGE_SCAN: type_str = "SortedRangeScan"; break;
    case Type::HASH_JOIN:         type_str = "HashJoin"; break;
    case Type::ASOF_JOIN:         type_str = "AsofJoin"; break;
    case Type::WINDOW_JOIN:       type_str = "WindowJoin"; break;
    case Type::HASH_AGGREGATE:    type_str = "HashAggregate"; break;
    case Type::SORT_AGGREGATE:    type_str = "SortAggregate"; break;
    case Type::FULL_SORT:         type_str = "FullSort"; break;
    case Type::TOPN_SORT:         type_str = "TopNSort"; break;
    case Type::FILTER:            type_str = "Filter"; break;
    case Type::PROJECT:           type_str = "Project"; break;
    case Type::LIMIT:             type_str = "Limit"; break;
    }

    if (type == Type::SEQ_SCAN || type == Type::INDEX_SCAN || type == Type::SORTED_RANGE_SCAN) {
        std::snprintf(buf, sizeof(buf), "%s [%s] (cost: %.2f, rows: %zu)",
                      type_str, table_name.c_str(), cost.total(), est_rows);
    } else if (type == Type::HASH_JOIN || type == Type::ASOF_JOIN || type == Type::WINDOW_JOIN) {
        std::snprintf(buf, sizeof(buf), "%s [build=%s] (cost: %.2f, rows: %zu)",
                      type_str, build_right ? "right" : "left", cost.total(), est_rows);
    } else if (type == Type::FILTER) {
        std::snprintf(buf, sizeof(buf), "%s (sel: %.2f, rows: %zu)",
                      type_str, selectivity, est_rows);
    } else {
        std::snprintf(buf, sizeof(buf), "%s (cost: %.2f, rows: %zu)",
                      type_str, cost.total(), est_rows);
    }
    return buf;
}

// ============================================================================
// format_explain_tree — indented tree output for EXPLAIN
// ============================================================================
void format_explain_tree(const std::shared_ptr<PhysicalNode>& node,
                         std::vector<std::string>& out,
                         const std::string& prefix,
                         bool is_last) {
    if (!node) return;

    std::string connector = prefix.empty() ? "" : (is_last ? "└── " : "├── ");
    out.push_back(prefix + connector + node->describe());

    std::string child_prefix = prefix.empty() ? "" : prefix + (is_last ? "    " : "│   ");

    bool has_left = (node->left != nullptr);
    bool has_right = (node->right != nullptr);

    if (has_left)
        format_explain_tree(node->left, out, child_prefix, !has_right);
    if (has_right)
        format_explain_tree(node->right, out, child_prefix, true);
}

} // namespace zeptodb::execution
