// ============================================================================
// Phase C-3: QueryCoordinator implementation
// ============================================================================

#include "apex/cluster/query_coordinator.h"
#include "apex/sql/parser.h"
#include "apex/sql/ast.h"

#include <algorithm>
#include <cctype>
#include <future>
#include <optional>
#include <set>
#include <sstream>

namespace apex::cluster {

// ============================================================================
// Node registration
// ============================================================================

void QueryCoordinator::add_remote_node(NodeAddress addr) {
    std::unique_lock lock(mutex_);
    for (const auto& ep : endpoints_) {
        if (ep->addr.id == addr.id) return;  // already registered
    }
    auto ep     = std::make_shared<NodeEndpoint>();
    ep->addr     = addr;
    ep->rpc      = std::make_unique<TcpRpcClient>(addr.host, addr.port);
    ep->pipeline = nullptr;
    ep->is_local = false;
    endpoints_.push_back(std::move(ep));
    router_.add_node(addr.id);
}

void QueryCoordinator::add_local_node(NodeAddress addr,
                                       apex::core::ApexPipeline& pipeline) {
    std::unique_lock lock(mutex_);
    for (const auto& ep : endpoints_) {
        if (ep->addr.id == addr.id) return;
    }
    auto ep     = std::make_shared<NodeEndpoint>();
    ep->addr     = addr;
    ep->rpc      = nullptr;
    ep->pipeline = &pipeline;
    ep->is_local = true;
    endpoints_.push_back(std::move(ep));
    router_.add_node(addr.id);
}

void QueryCoordinator::remove_node(NodeId id) {
    std::unique_lock lock(mutex_);
    endpoints_.erase(
        std::remove_if(endpoints_.begin(), endpoints_.end(),
                       [id](const std::shared_ptr<NodeEndpoint>& ep) {
                           return ep->addr.id == id;
                       }),
        endpoints_.end());
    router_.remove_node(id);
}

void QueryCoordinator::connect_health_monitor(HealthMonitor& hm) {
    hm.on_state_change([this](NodeId id, NodeState /*old_s*/, NodeState new_s) {
        if (new_s == NodeState::DEAD) {
            remove_node(id);
        }
    });
}

// ============================================================================
// Execution helpers
// ============================================================================

apex::sql::QueryResultSet QueryCoordinator::exec_on(NodeEndpoint&      ep,
                                                     const std::string& sql) {
    if (ep.is_local && ep.pipeline != nullptr) {
        apex::sql::QueryExecutor ex(*ep.pipeline);
        return ex.execute(sql);
    }
    if (ep.rpc) {
        return ep.rpc->execute_sql(sql);
    }
    apex::sql::QueryResultSet err;
    err.error = "QueryCoordinator::exec_on: endpoint has no RPC or pipeline";
    return err;
}

std::vector<apex::sql::QueryResultSet> QueryCoordinator::scatter(
    const std::string& sql)
{
    // Snapshot shared_ptrs under the lock — NodeEndpoints stay alive even if
    // remove_node() is called concurrently during the async fan-out below.
    std::vector<std::shared_ptr<NodeEndpoint>> eps;
    {
        std::shared_lock lock(mutex_);
        eps = endpoints_;  // copy of shared_ptrs (cheap, reference-counted)
    }

    // Launch parallel queries — no lock held during blocking I/O
    std::vector<std::future<apex::sql::QueryResultSet>> futures;
    futures.reserve(eps.size());
    for (auto& ep : eps) {
        futures.push_back(std::async(std::launch::async,
            [this, ep, &sql]() { return exec_on(*ep, sql); }));
    }

    std::vector<apex::sql::QueryResultSet> results;
    results.reserve(futures.size());
    for (auto& f : futures) {
        results.push_back(f.get());
    }
    return results;
}

// ============================================================================
// SQL-based merge strategy + per-column agg extraction
// (Column names from the executor are raw field names, not "sum(col)" style,
//  so result-based detection is unreliable.  Parse the SQL instead.)
// ============================================================================

// Per-AVG-column bookkeeping for the SQL rewrite path.
struct AvgColInfo {
    size_t orig_idx;  // position in the original (final) SELECT list
    size_t sum_idx;   // position in the REWRITTEN scatter result (SUM column)
    size_t cnt_idx;   // position in the REWRITTEN scatter result (COUNT column)
};

struct MergePlan {
    MergeStrategy                    strategy;
    // SCALAR_AGG: one entry per REWRITTEN column (SUM/COUNT replace each AVG)
    // MERGE_GROUP_BY: one entry per original column
    std::vector<apex::sql::AggFunc>  col_aggs;
    // MERGE_GROUP_BY: true for GROUP BY key columns
    std::vector<bool>                col_is_key;
    // Non-empty when the original SQL contains AVG — scatter uses this instead
    std::string                      rewritten_sql;
    // Empty when no AVG
    std::vector<AvgColInfo>          avg_cols;
    // Final output column names (used for AVG reconstruction)
    std::vector<std::string>         orig_col_names;
    // orig_to_rewritten[i] = index in rewritten result, -1 for AVG cols
    std::vector<int>                 orig_to_rewritten;
};

// ============================================================================
// SQL unparser helpers (for building the AVG-rewritten SELECT list)
// ============================================================================

static std::string unparse_arith(const apex::sql::ArithExpr& e) {
    switch (e.kind) {
        case apex::sql::ArithExpr::Kind::COLUMN:
            return e.table_alias.empty() ? e.column
                                         : (e.table_alias + "." + e.column);
        case apex::sql::ArithExpr::Kind::LITERAL:
            return std::to_string(e.literal);
        case apex::sql::ArithExpr::Kind::BINARY: {
            char op_ch = '+';
            switch (e.arith_op) {
                case apex::sql::ArithOp::ADD: op_ch = '+'; break;
                case apex::sql::ArithOp::SUB: op_ch = '-'; break;
                case apex::sql::ArithOp::MUL: op_ch = '*'; break;
                case apex::sql::ArithOp::DIV: op_ch = '/'; break;
            }
            return "(" + unparse_arith(*e.left) + " "
                       + op_ch + " "
                       + unparse_arith(*e.right) + ")";
        }
        default:
            return e.func_name.empty() ? "" : (e.func_name + "(...)");
    }
}

static std::string unparse_expr_inner(const apex::sql::SelectExpr& col) {
    if (col.arith_expr) return unparse_arith(*col.arith_expr);
    if (col.is_star)    return "*";
    return col.column;
}

static std::string unparse_select_expr(const apex::sql::SelectExpr& col) {
    using AF = apex::sql::AggFunc;
    switch (col.agg) {
        case AF::NONE:  return unparse_expr_inner(col);
        case AF::COUNT:
            return (col.is_star || col.column == "*") ? "count(*)"
                   : ("count(" + unparse_expr_inner(col) + ")");
        case AF::SUM:   return "sum("   + unparse_expr_inner(col) + ")";
        case AF::AVG:   return "avg("   + unparse_expr_inner(col) + ")";
        case AF::MIN:   return "min("   + unparse_expr_inner(col) + ")";
        case AF::MAX:   return "max("   + unparse_expr_inner(col) + ")";
        case AF::VWAP:  return "vwap("  + col.column + ", " + col.agg_arg2 + ")";
        case AF::FIRST: return "first(" + unparse_expr_inner(col) + ")";
        case AF::LAST:  return "last("  + unparse_expr_inner(col) + ")";
        case AF::XBAR:  return "xbar("  + col.column + ", "
                               + std::to_string(col.xbar_bucket) + ")";
    }
    return unparse_expr_inner(col);
}

// Return " FROM ..." suffix from the original SQL (case-insensitive FROM search).
static std::string extract_from_suffix(const std::string& sql) {
    std::string lo;
    lo.reserve(sql.size());
    for (char c : sql)
        lo += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto pos = lo.find(" from ");
    return pos == std::string::npos ? "" : sql.substr(pos);
}

// Build plan.rewritten_sql and related fields when the query contains AVG.
// AVG(expr) → SUM(expr), COUNT(expr) in the scatter query; reconstruct after merge.
static void build_avg_rewrite(const apex::sql::SelectStmt& stmt,
                               const std::string& original_sql,
                               MergePlan& plan)
{
    std::string from_suffix = extract_from_suffix(original_sql);

    std::vector<std::string>         rewritten_exprs;
    std::vector<apex::sql::AggFunc>  rewritten_aggs;
    size_t rewritten_idx = 0;

    plan.orig_col_names.reserve(stmt.columns.size());
    plan.orig_to_rewritten.reserve(stmt.columns.size());

    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        const auto& col = stmt.columns[i];
        plan.orig_col_names.push_back(col.alias.empty() ? col.column : col.alias);

        if (col.agg == apex::sql::AggFunc::AVG) {
            std::string inner = unparse_expr_inner(col);
            rewritten_exprs.push_back("sum(" + inner + ")");
            rewritten_aggs.push_back(apex::sql::AggFunc::SUM);
            rewritten_exprs.push_back("count(" + inner + ")");
            rewritten_aggs.push_back(apex::sql::AggFunc::COUNT);
            plan.avg_cols.push_back({i, rewritten_idx, rewritten_idx + 1});
            plan.orig_to_rewritten.push_back(-1);
            rewritten_idx += 2;
        } else {
            plan.orig_to_rewritten.push_back(static_cast<int>(rewritten_idx));
            rewritten_exprs.push_back(unparse_select_expr(col));
            rewritten_aggs.push_back(col.agg);
            ++rewritten_idx;
        }
    }

    plan.col_aggs = std::move(rewritten_aggs);

    std::string select_list;
    for (size_t i = 0; i < rewritten_exprs.size(); ++i) {
        if (i > 0) select_list += ", ";
        select_list += rewritten_exprs[i];
    }
    plan.rewritten_sql = "SELECT " + select_list + from_suffix;
}

// Reconstruct the final result (original column layout) from the merged
// rewritten scatter result (which has SUM/COUNT columns instead of AVG).
static apex::sql::QueryResultSet reconstruct_avg_result(
    const apex::sql::QueryResultSet& merged_rewritten,
    const MergePlan& plan)
{
    if (!merged_rewritten.ok()) return merged_rewritten;

    apex::sql::QueryResultSet out;
    out.column_names = plan.orig_col_names;
    out.column_types.resize(plan.orig_col_names.size(),
                            apex::storage::ColumnType::INT64);
    out.rows_scanned = merged_rewritten.rows_scanned;

    for (const auto& src : merged_rewritten.rows) {
        std::vector<int64_t> row(plan.orig_col_names.size(), 0);

        for (size_t i = 0; i < plan.orig_col_names.size(); ++i) {
            bool is_avg = false;
            for (const auto& ai : plan.avg_cols) {
                if (ai.orig_idx == i) {
                    int64_t s = ai.sum_idx < src.size() ? src[ai.sum_idx] : 0;
                    int64_t c = ai.cnt_idx < src.size() ? src[ai.cnt_idx] : 1;
                    row[i] = (c != 0) ? (s / c) : 0;
                    is_avg = true;
                    break;
                }
            }
            if (!is_avg) {
                int ri = i < plan.orig_to_rewritten.size()
                         ? plan.orig_to_rewritten[i] : -1;
                row[i] = (ri >= 0 && static_cast<size_t>(ri) < src.size())
                         ? src[ri] : 0;
            }
        }
        out.rows.push_back(std::move(row));
    }
    return out;
}

static MergePlan merge_plan_from_sql(const std::string& sql) {
    try {
        apex::sql::Parser parser;
        auto stmt = parser.parse(sql);

        bool has_group_by = stmt.group_by && !stmt.group_by->columns.empty();

        if (has_group_by) {
            // Check if "symbol" is a GROUP BY key — symbol affinity guarantees
            // each group key lives on exactly one node, so CONCAT is correct.
            bool has_symbol_key = false;
            for (const auto& gb_col : stmt.group_by->columns) {
                if (gb_col == "symbol") { has_symbol_key = true; break; }
            }
            if (has_symbol_key) {
                MergePlan p; p.strategy = MergeStrategy::CONCAT; return p;
            }

            // Non-symbol GROUP BY: the same key bucket may appear on multiple
            // nodes (e.g. xbar time bucket).  Use MERGE_GROUP_BY to re-aggregate.
            //
            // The executor outputs columns in this layout (same for path 2 and
            // the general multi-column path):
            //   [gb.columns[0], ..., gb.columns[N-1],  <- N GROUP BY key cols
            //    non-NONE SELECT cols in SELECT order]  <- M aggregate cols
            MergePlan plan;
            plan.strategy = MergeStrategy::MERGE_GROUP_BY;

            // N key entries (one per GROUP BY column)
            for (size_t gi = 0; gi < stmt.group_by->columns.size(); ++gi) {
                plan.col_is_key.push_back(true);
                plan.col_aggs.push_back(apex::sql::AggFunc::NONE);
            }
            // M aggregate entries (non-NONE SELECT columns in order)
            for (const auto& col : stmt.columns) {
                if (col.agg == apex::sql::AggFunc::NONE) continue;
                plan.col_is_key.push_back(false);
                plan.col_aggs.push_back(col.agg);
            }
            return plan;
        }

        // Scalar aggregate: no GROUP BY, every SELECT column is an aggregate.
        if (!stmt.columns.empty()) {
            bool all_agg = true;
            bool has_avg = false;
            for (const auto& col : stmt.columns) {
                if (col.agg == apex::sql::AggFunc::NONE && !col.is_star) {
                    all_agg = false; break;
                }
                if (col.agg == apex::sql::AggFunc::AVG) has_avg = true;
            }

            if (all_agg) {
                MergePlan plan;
                plan.strategy = MergeStrategy::SCALAR_AGG;

                if (has_avg) {
                    build_avg_rewrite(stmt, sql, plan);
                } else {
                    plan.col_aggs.reserve(stmt.columns.size());
                    for (const auto& col : stmt.columns)
                        plan.col_aggs.push_back(col.agg);
                }
                return plan;
            }
        }
    } catch (...) {}
    MergePlan fallback;
    fallback.strategy = MergeStrategy::CONCAT;
    return fallback;
}

// ============================================================================
// Tier A symbol filter extraction
// ============================================================================

std::optional<SymbolId> QueryCoordinator::extract_symbol_filter(
    const std::string& sql) const
{
    try {
        apex::sql::Parser parser;
        auto stmt = parser.parse(sql);
        if (!stmt.where || !stmt.where->expr) return std::nullopt;

        const auto& w = stmt.where->expr;
        // Match: WHERE symbol = <integer literal>
        // Expr::Kind::COMPARE node stores column name + value directly
        if (w->kind   == apex::sql::Expr::Kind::COMPARE &&
            w->op     == apex::sql::CompareOp::EQ       &&
            w->column == "symbol"                       &&
            !w->is_float)
        {
            return static_cast<SymbolId>(w->value);
        }
    } catch (...) {
        // Parse error — fall through to Tier B
    }
    return std::nullopt;
}

// ============================================================================
// Public execute_sql
// ============================================================================

apex::sql::QueryResultSet QueryCoordinator::execute_sql_for_symbol(
    const std::string& sql, SymbolId symbol_id)
{
    // Resolve the owning endpoint under the lock, then release before I/O.
    std::shared_ptr<NodeEndpoint> target;
    {
        std::shared_lock lock(mutex_);
        if (endpoints_.empty()) {
            apex::sql::QueryResultSet err;
            err.error = "QueryCoordinator: no nodes registered";
            return err;
        }
        NodeId owner = router_.route(symbol_id);
        for (auto& ep : endpoints_) {
            if (ep->addr.id == owner) { target = ep; break; }
        }
    }
    if (!target) {
        apex::sql::QueryResultSet err;
        err.error = "QueryCoordinator: owning node not found for symbol "
                  + std::to_string(symbol_id);
        return err;
    }
    return exec_on(*target, sql);
}

apex::sql::QueryResultSet QueryCoordinator::execute_sql(const std::string& sql) {
    // Extract what we need under the lock, then release before any blocking I/O.
    std::shared_ptr<NodeEndpoint> single_ep;
    {
        std::shared_lock lock(mutex_);
        if (endpoints_.empty()) {
            apex::sql::QueryResultSet err;
            err.error = "QueryCoordinator: no nodes registered";
            return err;
        }
        // Single node: bypass scatter/merge overhead
        if (endpoints_.size() == 1) {
            single_ep = endpoints_[0];
        }
    }
    if (single_ep) {
        return exec_on(*single_ep, sql);
    }

    // Tier A: single-symbol query → direct routing
    auto sym = extract_symbol_filter(sql);
    if (sym) {
        return execute_sql_for_symbol(sql, *sym);
    }

    // Tier A-2: ASOF JOIN with symbol filter → route to symbol's node
    // Most HFT ASOF JOINs filter by symbol, so both tables are on the same node.
    {
        try {
            apex::sql::Parser parser;
            auto stmt = parser.parse(sql);
            if (stmt.join && (stmt.join->type == apex::sql::JoinClause::Type::ASOF ||
                              stmt.join->type == apex::sql::JoinClause::Type::WINDOW)) {
                // Check if WHERE has symbol = X
                if (stmt.where && stmt.where->expr &&
                    stmt.where->expr->kind == apex::sql::Expr::Kind::COMPARE &&
                    stmt.where->expr->op == apex::sql::CompareOp::EQ &&
                    stmt.where->expr->column == "symbol" &&
                    !stmt.where->expr->is_float)
                {
                    auto sid = static_cast<SymbolId>(stmt.where->expr->value);
                    return execute_sql_for_symbol(sql, sid);
                }
                // No symbol filter: scatter to all nodes, each executes locally,
                // concat results (each node only has its own symbols' data)
                auto results = scatter(sql);
                return merge_concat_results(results);
            }
        } catch (...) {}
    }

    // Tier B: scatter-gather
    auto plan               = merge_plan_from_sql(sql);
    const std::string& scatter_sql = plan.rewritten_sql.empty() ? sql
                                                                 : plan.rewritten_sql;
    auto node_results = scatter(scatter_sql);

    if (plan.strategy == MergeStrategy::MERGE_GROUP_BY) {
        return merge_group_by_results(node_results, plan.col_is_key, plan.col_aggs);
    }
    if (plan.strategy == MergeStrategy::SCALAR_AGG && !plan.col_aggs.empty()) {
        auto merged = merge_scalar_with_sql_aggs(node_results, plan.col_aggs);
        if (!plan.avg_cols.empty()) {
            return reconstruct_avg_result(merged, plan);
        }
        return merged;
    }
    return merge_concat_results(node_results);
}

} // namespace apex::cluster
