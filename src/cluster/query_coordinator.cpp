// ============================================================================
// Phase C-3: QueryCoordinator implementation
// ============================================================================

#include "zeptodb/cluster/query_coordinator.h"
#include "zeptodb/sql/parser.h"
#include "zeptodb/sql/ast.h"

#include <algorithm>
#include <cctype>
#include <future>
#include <optional>
#include <set>
#include <sstream>

namespace zeptodb::cluster {

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
    auto rlock = router_write_lock();
    router().add_node(addr.id);
}

void QueryCoordinator::add_local_node(NodeAddress addr,
                                       zeptodb::core::ZeptoPipeline& pipeline) {
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
    auto rlock = router_write_lock();
    router().add_node(addr.id);
}

void QueryCoordinator::remove_node(NodeId id) {
    std::unique_lock lock(mutex_);
    endpoints_.erase(
        std::remove_if(endpoints_.begin(), endpoints_.end(),
                       [id](const std::shared_ptr<NodeEndpoint>& ep) {
                           return ep->addr.id == id;
                       }),
        endpoints_.end());
    auto rlock = router_write_lock();
    router().remove_node(id);
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

zeptodb::sql::QueryResultSet QueryCoordinator::exec_on(NodeEndpoint&      ep,
                                                     const std::string& sql) {
    if (ep.is_local && ep.pipeline != nullptr) {
        zeptodb::sql::QueryExecutor ex(*ep.pipeline);
        return ex.execute(sql);
    }
    if (ep.rpc) {
        return ep.rpc->execute_sql(sql);
    }
    zeptodb::sql::QueryResultSet err;
    err.error = "QueryCoordinator::exec_on: endpoint has no RPC or pipeline";
    return err;
}

std::vector<zeptodb::sql::QueryResultSet> QueryCoordinator::scatter(
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
    std::vector<std::future<zeptodb::sql::QueryResultSet>> futures;
    futures.reserve(eps.size());
    for (auto& ep : eps) {
        futures.push_back(std::async(std::launch::async,
            [this, ep, &sql]() { return exec_on(*ep, sql); }));
    }

    std::vector<zeptodb::sql::QueryResultSet> results;
    results.reserve(futures.size());
    for (auto& f : futures) {
        results.push_back(f.get());
    }

    // P0-1: Partial failure policy — if any node returned an error, fail the
    // entire query.  Partial results are dangerous for aggregates (wrong SUM,
    // wrong COUNT, etc.).  Callers see a clear error instead of silent data loss.
    for (size_t i = 0; i < results.size(); ++i) {
        if (!results[i].ok()) {
            zeptodb::sql::QueryResultSet err;
            err.error = "node " + eps[i]->addr.host + ":"
                      + std::to_string(eps[i]->addr.port) + " failed: "
                      + results[i].error;
            return {err};
        }
    }

    return results;
}

// scatter_to: send query only to specific node indices (for WHERE IN routing)
std::vector<zeptodb::sql::QueryResultSet> QueryCoordinator::scatter_to(
    const std::string& sql,
    const std::unordered_set<size_t>& node_indices)
{
    std::vector<std::shared_ptr<NodeEndpoint>> eps;
    {
        std::shared_lock lock(mutex_);
        for (size_t idx : node_indices) {
            if (idx < endpoints_.size()) eps.push_back(endpoints_[idx]);
        }
    }
    if (eps.empty()) return {};

    std::vector<std::future<zeptodb::sql::QueryResultSet>> futures;
    futures.reserve(eps.size());
    for (auto& ep : eps) {
        futures.push_back(std::async(std::launch::async,
            [this, ep, &sql]() { return exec_on(*ep, sql); }));
    }

    std::vector<zeptodb::sql::QueryResultSet> results;
    results.reserve(futures.size());
    for (auto& f : futures) results.push_back(f.get());

    for (size_t i = 0; i < results.size(); ++i) {
        if (!results[i].ok()) {
            zeptodb::sql::QueryResultSet err;
            err.error = "node " + eps[i]->addr.host + ":"
                      + std::to_string(eps[i]->addr.port) + " failed: "
                      + results[i].error;
            return {err};
        }
    }
    return results;
}

// ============================================================================
// SQL-based merge strategy + per-column agg extraction
// (Column names from the executor are raw field names, not "sum(col)" style,
//  so result-based detection is unreliable.  Parse the SQL instead.)
// ============================================================================

// Per-AVG/VWAP-column bookkeeping for the SQL rewrite path.
// AVG(x)          → SUM(x), COUNT(x)       → reconstruct: SUM/COUNT
// VWAP(price,vol) → SUM(price*vol), SUM(vol) → reconstruct: SUM_PV/SUM_V
struct AvgColInfo {
    size_t orig_idx;  // position in the original (final) SELECT list
    size_t sum_idx;   // position in the REWRITTEN scatter result (SUM column)
    size_t cnt_idx;   // position in the REWRITTEN scatter result (COUNT or SUM(vol) column)
    bool   is_vwap = false;  // true: SUM_PV/SUM_V, false: SUM/COUNT
};

struct MergePlan {
    MergeStrategy                    strategy;
    std::vector<zeptodb::sql::AggFunc>  col_aggs;
    std::vector<bool>                col_is_key;
    std::string                      rewritten_sql;
    std::vector<AvgColInfo>          avg_cols;
    std::vector<std::string>         orig_col_names;
    std::vector<int>                 orig_to_rewritten;
    // HAVING: stored from original SQL, applied after merge
    bool                             has_having = false;
    std::string                      having_col;   // column name to filter on
    zeptodb::sql::CompareOp             having_op = zeptodb::sql::CompareOp::EQ;
    int64_t                          having_val = 0;
};

// ============================================================================
// SQL unparser helpers (for building the AVG-rewritten SELECT list)
// ============================================================================

static std::string unparse_arith(const zeptodb::sql::ArithExpr& e) {
    switch (e.kind) {
        case zeptodb::sql::ArithExpr::Kind::COLUMN:
            return e.table_alias.empty() ? e.column
                                         : (e.table_alias + "." + e.column);
        case zeptodb::sql::ArithExpr::Kind::LITERAL:
            return std::to_string(e.literal);
        case zeptodb::sql::ArithExpr::Kind::BINARY: {
            char op_ch = '+';
            switch (e.arith_op) {
                case zeptodb::sql::ArithOp::ADD: op_ch = '+'; break;
                case zeptodb::sql::ArithOp::SUB: op_ch = '-'; break;
                case zeptodb::sql::ArithOp::MUL: op_ch = '*'; break;
                case zeptodb::sql::ArithOp::DIV: op_ch = '/'; break;
            }
            return "(" + unparse_arith(*e.left) + " "
                       + op_ch + " "
                       + unparse_arith(*e.right) + ")";
        }
        default:
            return e.func_name.empty() ? "" : (e.func_name + "(...)");
    }
}

static std::string unparse_case_when(const zeptodb::sql::CaseWhenExpr& cwe);

static std::string unparse_expr_inner(const zeptodb::sql::SelectExpr& col) {
    if (col.case_when) return unparse_case_when(*col.case_when);
    if (col.arith_expr) return unparse_arith(*col.arith_expr);
    if (col.is_star)    return "*";
    return col.column;
}

static std::string unparse_select_expr(const zeptodb::sql::SelectExpr& col) {
    using AF = zeptodb::sql::AggFunc;
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
        case AF::STDDEV:    return "stddev("    + unparse_expr_inner(col) + ")";
        case AF::VARIANCE:  return "variance("  + unparse_expr_inner(col) + ")";
        case AF::MEDIAN:    return "median("    + unparse_expr_inner(col) + ")";
        case AF::PERCENTILE: return "percentile(" + unparse_expr_inner(col) + ", "
                               + std::to_string(col.percentile_value) + ")";
    }
    return unparse_expr_inner(col);
}

static std::string unparse_case_when(const zeptodb::sql::CaseWhenExpr& cwe) {
    std::string s = "CASE";
    auto op_str = [](zeptodb::sql::CompareOp op) -> const char* {
        switch (op) {
            case zeptodb::sql::CompareOp::EQ: return "=";
            case zeptodb::sql::CompareOp::NE: return "!=";
            case zeptodb::sql::CompareOp::GT: return ">";
            case zeptodb::sql::CompareOp::LT: return "<";
            case zeptodb::sql::CompareOp::GE: return ">=";
            case zeptodb::sql::CompareOp::LE: return "<=";
        }
        return "=";
    };
    for (const auto& b : cwe.branches) {
        s += " WHEN ";
        if (b.when_cond) {
            s += b.when_cond->column + " " + op_str(b.when_cond->op) + " "
               + std::to_string(b.when_cond->value);
        }
        s += " THEN ";
        s += b.then_val ? unparse_arith(*b.then_val) : "0";
    }
    if (cwe.else_val) {
        s += " ELSE " + unparse_arith(*cwe.else_val);
    }
    s += " END";
    return s;
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

// Remove HAVING clause from SQL string (case-insensitive).
// Returns the SQL without HAVING so nodes don't pre-filter partial results.
static std::string strip_having(const std::string& sql) {
    std::string lo;
    lo.reserve(sql.size());
    for (char c : sql)
        lo += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto pos = lo.find(" having ");
    if (pos == std::string::npos) return sql;
    // Find end of HAVING clause: next ORDER BY, LIMIT, UNION, or end of string
    size_t end = std::string::npos;
    for (const char* kw : {" order ", " limit ", " union ", " intersect ", " except "}) {
        auto p = lo.find(kw, pos + 8);
        if (p != std::string::npos && (end == std::string::npos || p < end))
            end = p;
    }
    if (end == std::string::npos)
        return sql.substr(0, pos);
    return sql.substr(0, pos) + sql.substr(end);
}

// Build plan.rewritten_sql and related fields when the query contains AVG.
// AVG(expr) → SUM(expr), COUNT(expr) in the scatter query; reconstruct after merge.
static void build_avg_rewrite(const zeptodb::sql::SelectStmt& stmt,
                               const std::string& original_sql,
                               MergePlan& plan)
{
    std::string from_suffix = extract_from_suffix(original_sql);

    std::vector<std::string>         rewritten_exprs;
    std::vector<zeptodb::sql::AggFunc>  rewritten_aggs;
    size_t rewritten_idx = 0;

    plan.orig_col_names.reserve(stmt.columns.size());
    plan.orig_to_rewritten.reserve(stmt.columns.size());

    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        const auto& col = stmt.columns[i];
        plan.orig_col_names.push_back(col.alias.empty() ? col.column : col.alias);

        if (col.agg == zeptodb::sql::AggFunc::AVG) {
            std::string inner = unparse_expr_inner(col);
            rewritten_exprs.push_back("sum(" + inner + ")");
            rewritten_aggs.push_back(zeptodb::sql::AggFunc::SUM);
            rewritten_exprs.push_back("count(" + inner + ")");
            rewritten_aggs.push_back(zeptodb::sql::AggFunc::COUNT);
            plan.avg_cols.push_back({i, rewritten_idx, rewritten_idx + 1, false});
            plan.orig_to_rewritten.push_back(-1);
            rewritten_idx += 2;
        } else if (col.agg == zeptodb::sql::AggFunc::VWAP) {
            // VWAP(price, vol) → SUM(price * vol), SUM(vol)
            std::string price_col = col.column;
            std::string vol_col   = col.agg_arg2;
            rewritten_exprs.push_back("sum(" + price_col + " * " + vol_col + ")");
            rewritten_aggs.push_back(zeptodb::sql::AggFunc::SUM);
            rewritten_exprs.push_back("sum(" + vol_col + ")");
            rewritten_aggs.push_back(zeptodb::sql::AggFunc::SUM);
            plan.avg_cols.push_back({i, rewritten_idx, rewritten_idx + 1, true});
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
static zeptodb::sql::QueryResultSet reconstruct_avg_result(
    const zeptodb::sql::QueryResultSet& merged_rewritten,
    const MergePlan& plan)
{
    if (!merged_rewritten.ok()) return merged_rewritten;

    zeptodb::sql::QueryResultSet out;
    out.column_names = plan.orig_col_names;
    out.column_types.resize(plan.orig_col_names.size(),
                            zeptodb::storage::ColumnType::INT64);
    out.rows_scanned = merged_rewritten.rows_scanned;

    for (const auto& src : merged_rewritten.rows) {
        std::vector<int64_t> row(plan.orig_col_names.size(), 0);

        for (size_t i = 0; i < plan.orig_col_names.size(); ++i) {
            bool is_avg = false;
            for (const auto& ai : plan.avg_cols) {
                if (ai.orig_idx == i) {
                    int64_t s = ai.sum_idx < src.size() ? src[ai.sum_idx] : 0;
                    int64_t c = ai.cnt_idx < src.size() ? src[ai.cnt_idx] : 1;
                    // Rounding division to minimize truncation error
                    if (c != 0) {
                        row[i] = (s >= 0) ? (s + c / 2) / c
                                          : (s - c / 2) / c;
                    }
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
        zeptodb::sql::Parser parser;
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
                plan.col_aggs.push_back(zeptodb::sql::AggFunc::NONE);
            }
            // M aggregate entries (non-NONE SELECT columns in order)
            for (const auto& col : stmt.columns) {
                if (col.agg == zeptodb::sql::AggFunc::NONE) continue;
                plan.col_is_key.push_back(false);
                plan.col_aggs.push_back(col.agg);
            }

            // Extract HAVING for post-merge filtering
            if (stmt.having && stmt.having->expr) {
                const auto& h = stmt.having->expr;
                if (h->kind == zeptodb::sql::Expr::Kind::COMPARE) {
                    plan.has_having = true;
                    plan.having_col = h->column;
                    plan.having_op  = h->op;
                    plan.having_val = h->value;
                }
            }

            return plan;
        }

        // Scalar aggregate: no GROUP BY, every SELECT column is an aggregate.
        if (!stmt.columns.empty()) {
            bool all_agg = true;
            bool has_avg = false;
            for (const auto& col : stmt.columns) {
                if (col.agg == zeptodb::sql::AggFunc::NONE) {
                    all_agg = false; break;
                }
                if (col.agg == zeptodb::sql::AggFunc::AVG) has_avg = true;
                if (col.agg == zeptodb::sql::AggFunc::VWAP) has_avg = true;
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
        zeptodb::sql::Parser parser;
        auto stmt = parser.parse(sql);
        if (!stmt.where || !stmt.where->expr) return std::nullopt;

        const auto& w = stmt.where->expr;
        // Match: WHERE symbol = <integer literal>
        // Expr::Kind::COMPARE node stores column name + value directly
        if (w->kind   == zeptodb::sql::Expr::Kind::COMPARE &&
            w->op     == zeptodb::sql::CompareOp::EQ       &&
            w->column == "symbol")
        {
            if (w->is_string) {
                // Resolve string symbol via each node's local executor
                // For now, fall through to Tier B (scatter-gather)
                // The per-node executor will resolve the string locally
                return std::nullopt;
            }
            if (!w->is_float)
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

zeptodb::sql::QueryResultSet QueryCoordinator::execute_sql_for_symbol(
    const std::string& sql, SymbolId symbol_id)
{
    // Resolve the owning endpoint under the lock, then release before I/O.
    std::shared_ptr<NodeEndpoint> target;
    {
        std::shared_lock lock(mutex_);
        if (endpoints_.empty()) {
            zeptodb::sql::QueryResultSet err;
            err.error = "QueryCoordinator: no nodes registered";
            return err;
        }
        auto rlock = router_read_lock();
        NodeId owner = router().route(symbol_id);
        for (auto& ep : endpoints_) {
            if (ep->addr.id == owner) { target = ep; break; }
        }
    }
    if (!target) {
        zeptodb::sql::QueryResultSet err;
        err.error = "QueryCoordinator: owning node not found for symbol "
                  + std::to_string(symbol_id);
        return err;
    }
    return exec_on(*target, sql);
}

zeptodb::sql::QueryResultSet QueryCoordinator::execute_sql(const std::string& sql) {
    // ---- DML routing: INSERT/UPDATE/DELETE are not scatter queries ----
    // Parse to detect DML and route appropriately.
    {
        std::string upper;
        for (size_t i = 0; i < sql.size() && i < 10; ++i)
            upper += static_cast<char>(std::toupper(static_cast<unsigned char>(sql[i])));

        if (upper.rfind("INSERT", 0) == 0) {
            // INSERT: route to the node that owns the symbol
            auto sym = extract_symbol_filter(sql);
            if (sym) return execute_sql_for_symbol(sql, *sym);
            // No symbol filter — execute on first node (single-writer)
            std::shared_lock lock(mutex_);
            if (!endpoints_.empty()) return exec_on(*endpoints_[0], sql);
        }
        if (upper.rfind("UPDATE", 0) == 0 || upper.rfind("DELETE", 0) == 0) {
            // UPDATE/DELETE: if symbol filter, route to that node; else broadcast
            auto sym = extract_symbol_filter(sql);
            if (sym) return execute_sql_for_symbol(sql, *sym);
            // No symbol filter — broadcast to all nodes, sum affected rows
            auto results = scatter(sql);
            zeptodb::sql::QueryResultSet merged;
            int64_t total = 0;
            for (auto& r : results) {
                if (!r.ok()) return r;
                if (!r.rows.empty()) total += r.rows[0][0];
            }
            merged.column_names = results.empty() ? std::vector<std::string>{"affected"}
                                                  : results[0].column_names;
            merged.column_types = {zeptodb::storage::ColumnType::INT64};
            merged.rows = {{total}};
            return merged;
        }
        if (upper.rfind("CREATE", 0) == 0 || upper.rfind("DROP", 0) == 0 ||
            upper.rfind("ALTER", 0) == 0) {
            // DDL: broadcast to all nodes
            auto results = scatter(sql);
            for (auto& r : results) {
                if (!r.ok()) return r;
            }
            return results.empty() ? zeptodb::sql::QueryResultSet{} : results[0];
        }
        if (upper.rfind("SHOW", 0) == 0 || upper.rfind("DESC", 0) == 0) {
            // Catalog queries: schema is replicated via DDL broadcast.
            // SHOW TABLES: scatter to all nodes, sum row counts.
            // DESCRIBE: execute on first node only (schema is identical).
            if (upper.rfind("SHOW", 0) == 0) {
                auto results = scatter(sql);
                if (results.empty()) return zeptodb::sql::QueryResultSet{};
                // Use first result as base, sum row counts from others
                auto merged = std::move(results[0]);
                if (!merged.ok()) return merged;
                for (size_t n = 1; n < results.size(); ++n) {
                    if (!results[n].ok()) continue;
                    for (size_t i = 0; i < merged.rows.size() && i < results[n].rows.size(); ++i) {
                        for (size_t c = 0; c < merged.rows[i].size() && c < results[n].rows[i].size(); ++c) {
                            merged.rows[i][c] += results[n].rows[i][c];
                        }
                    }
                }
                return merged;
            }
            // DESCRIBE: any single node suffices
            std::shared_lock lock(mutex_);
            if (endpoints_.empty()) {
                zeptodb::sql::QueryResultSet err;
                err.error = "QueryCoordinator: no nodes registered";
                return err;
            }
            auto first = endpoints_[0];
            lock.unlock();
            return exec_on(*first, sql);
        }
    }

    // ---- SELECT path (existing logic) ----
    // Extract what we need under the lock, then release before any blocking I/O.
    std::shared_ptr<NodeEndpoint> single_ep;
    {
        std::shared_lock lock(mutex_);
        if (endpoints_.empty()) {
            zeptodb::sql::QueryResultSet err;
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

    // Tier A-1: WHERE symbol IN (1,2,3) → each node filters locally
    // Broadcast to all nodes; each node's eval_where handles IN correctly.
    // Future optimization: use partition router to target only owning nodes.
    {
        try {
            zeptodb::sql::Parser in_parser;
            auto in_stmt = in_parser.parse(sql);
            if (in_stmt.where && in_stmt.where->expr &&
                in_stmt.where->expr->kind == zeptodb::sql::Expr::Kind::IN &&
                in_stmt.where->expr->column == "symbol" &&
                !in_stmt.where->expr->negated)
            {
                auto plan = merge_plan_from_sql(sql);
                std::string base_sql = plan.rewritten_sql.empty() ? sql : plan.rewritten_sql;
                if (plan.has_having) base_sql = strip_having(base_sql);
                auto results = scatter(base_sql);

                zeptodb::sql::QueryResultSet merged;
                if (plan.strategy == MergeStrategy::MERGE_GROUP_BY)
                    merged = merge_group_by_results(results, plan.col_is_key, plan.col_aggs);
                else if (plan.strategy == MergeStrategy::SCALAR_AGG && !plan.col_aggs.empty())
                    merged = merge_scalar_with_sql_aggs(results, plan.col_aggs);
                else
                    merged = merge_concat_results(results);

                // Post-merge ORDER BY + LIMIT
                if (merged.ok() && merged.rows.size() > 1) {
                    try {
                        zeptodb::sql::Parser pp;
                        auto ps = pp.parse(sql);
                        if (ps.order_by && !ps.order_by->items.empty()) {
                            const auto& items = ps.order_by->items;
                            int idx = -1;
                            for (size_t ci = 0; ci < merged.column_names.size(); ++ci) {
                                if (merged.column_names[ci] == items[0].column) {
                                    idx = static_cast<int>(ci); break;
                                }
                            }
                            if (idx >= 0) {
                                bool asc = items[0].asc;
                                std::sort(merged.rows.begin(), merged.rows.end(),
                                    [idx, asc](const std::vector<int64_t>& a,
                                               const std::vector<int64_t>& b) {
                                        return asc ? a[idx] < b[idx] : a[idx] > b[idx];
                                    });
                            }
                        }
                        if (ps.limit && *ps.limit < merged.rows.size())
                            merged.rows.resize(*ps.limit);
                    } catch (...) {}
                }
                return merged;
            }
        } catch (...) {}
    }

    // Tier A-2: ASOF JOIN with symbol filter → route to symbol's node
    // Most HFT ASOF JOINs filter by symbol, so both tables are on the same node.
    {
        try {
            zeptodb::sql::Parser parser;
            auto stmt = parser.parse(sql);
            if (stmt.join && (stmt.join->type == zeptodb::sql::JoinClause::Type::ASOF ||
                              stmt.join->type == zeptodb::sql::JoinClause::Type::WINDOW)) {
                // Check if WHERE has symbol = X
                if (stmt.where && stmt.where->expr &&
                    stmt.where->expr->kind == zeptodb::sql::Expr::Kind::COMPARE &&
                    stmt.where->expr->op == zeptodb::sql::CompareOp::EQ &&
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

    // Window function / FIRST / LAST detection: fetch all base data, compute locally.
    // FIRST/LAST need global timestamp ordering which partial merge can't provide.
    {
        try {
            zeptodb::sql::Parser wparser;
            auto wstmt = wparser.parse(sql);
            bool needs_full_data = false;
            for (const auto& col : wstmt.columns) {
                if (col.window_spec) { needs_full_data = true; break; }
                if (col.agg == zeptodb::sql::AggFunc::FIRST ||
                    col.agg == zeptodb::sql::AggFunc::LAST) {
                    needs_full_data = true; break;
                }
                if (col.agg == zeptodb::sql::AggFunc::COUNT && col.agg_distinct) {
                    needs_full_data = true; break;
                }
                if (col.agg == zeptodb::sql::AggFunc::STDDEV ||
                    col.agg == zeptodb::sql::AggFunc::VARIANCE ||
                    col.agg == zeptodb::sql::AggFunc::MEDIAN ||
                    col.agg == zeptodb::sql::AggFunc::PERCENTILE) {
                    needs_full_data = true; break;
                }
            }
            // CTE or FROM subquery: must execute on full dataset
            if (!wstmt.cte_defs.empty() || wstmt.from_subquery)
                needs_full_data = true;
            if (needs_full_data) {
                // Build base data query: SELECT * FROM trades [WHERE ...]
                // For CTE/subquery, from_table may be a CTE name, so use "trades"
                std::string real_table = wstmt.from_table;
                if (!wstmt.cte_defs.empty() || wstmt.from_subquery)
                    real_table = "trades";
                std::string base_q = "SELECT * FROM " + real_table;
                if (wstmt.where) {
                    // Re-extract WHERE from original SQL
                    std::string lo;
                    for (char c : sql)
                        lo += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    auto wpos = lo.find(" where ");
                    if (wpos != std::string::npos) {
                        // Find end of WHERE: GROUP BY, ORDER BY, LIMIT, or end
                        size_t wend = std::string::npos;
                        for (const char* kw : {" group ", " order ", " limit ",
                                               " having ", " union "}) {
                            auto p = lo.find(kw, wpos + 7);
                            if (p != std::string::npos &&
                                (wend == std::string::npos || p < wend))
                                wend = p;
                        }
                        if (wend == std::string::npos)
                            base_q += sql.substr(wpos);
                        else
                            base_q += sql.substr(wpos, wend - wpos);
                    }
                }

                // Scatter base query, concat all rows
                auto base_results = scatter(base_q);
                auto base_merged = merge_concat_results(base_results);
                if (!base_merged.ok()) return base_merged;

                // Ingest into a temporary local pipeline
                zeptodb::core::PipelineConfig tcfg;
                tcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
                zeptodb::core::ZeptoPipeline tmp(tcfg);
                tmp.start();

                // Map columns: find symbol/price/volume/timestamp indices
                int ci_sym = -1, ci_price = -1, ci_vol = -1, ci_ts = -1;
                for (size_t i = 0; i < base_merged.column_names.size(); ++i) {
                    const auto& n = base_merged.column_names[i];
                    if (n == "symbol")    ci_sym   = static_cast<int>(i);
                    if (n == "price")     ci_price = static_cast<int>(i);
                    if (n == "volume")    ci_vol   = static_cast<int>(i);
                    if (n == "timestamp") ci_ts    = static_cast<int>(i);
                }
                // Sort by timestamp for correct FIRST/LAST ordering
                if (ci_ts >= 0) {
                    std::sort(base_merged.rows.begin(), base_merged.rows.end(),
                        [ci_ts](const std::vector<int64_t>& a,
                                const std::vector<int64_t>& b) {
                            return a[ci_ts] < b[ci_ts];
                        });
                }
                for (auto& row : base_merged.rows) {
                    zeptodb::ingestion::TickMessage msg{};
                    if (ci_sym >= 0)   msg.symbol_id = static_cast<uint32_t>(row[ci_sym]);
                    if (ci_price >= 0) msg.price     = row[ci_price];
                    if (ci_vol >= 0)   msg.volume    = row[ci_vol];
                    if (ci_ts >= 0)    msg.recv_ts   = row[ci_ts];
                    tmp.store_tick_direct(msg);
                }

                // Execute original SQL on the complete local dataset
                zeptodb::sql::QueryExecutor local_ex(tmp);
                auto result = local_ex.execute(sql);
                tmp.stop();
                return result;
            }
        } catch (...) {}
    }

    // Tier B: scatter-gather
    auto plan = merge_plan_from_sql(sql);
    // Strip HAVING from scatter SQL so nodes don't pre-filter partial results
    std::string base_sql = plan.rewritten_sql.empty() ? sql : plan.rewritten_sql;
    if (plan.has_having) base_sql = strip_having(base_sql);
    auto node_results = scatter(base_sql);

    zeptodb::sql::QueryResultSet merged;
    if (plan.strategy == MergeStrategy::MERGE_GROUP_BY) {
        merged = merge_group_by_results(node_results, plan.col_is_key, plan.col_aggs);
    } else if (plan.strategy == MergeStrategy::SCALAR_AGG && !plan.col_aggs.empty()) {
        merged = merge_scalar_with_sql_aggs(node_results, plan.col_aggs);
        if (!plan.avg_cols.empty()) {
            merged = reconstruct_avg_result(merged, plan);
        }
    } else {
        merged = merge_concat_results(node_results);
    }

    // Post-merge HAVING filter (must come before ORDER BY/LIMIT)
    if (plan.has_having && merged.ok() && !merged.rows.empty()) {
        int hcol = -1;
        for (size_t i = 0; i < merged.column_names.size(); ++i) {
            if (merged.column_names[i] == plan.having_col) { hcol = static_cast<int>(i); break; }
        }
        if (hcol >= 0) {
            auto& rows = merged.rows;
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                [hcol, &plan](const std::vector<int64_t>& r) {
                    int64_t v = r[hcol];
                    switch (plan.having_op) {
                        case zeptodb::sql::CompareOp::GT:  return !(v > plan.having_val);
                        case zeptodb::sql::CompareOp::GE:  return !(v >= plan.having_val);
                        case zeptodb::sql::CompareOp::LT:  return !(v < plan.having_val);
                        case zeptodb::sql::CompareOp::LE:  return !(v <= plan.having_val);
                        case zeptodb::sql::CompareOp::EQ:  return !(v == plan.having_val);
                        case zeptodb::sql::CompareOp::NE:  return !(v != plan.having_val);
                    }
                    return false;
                }), rows.end());
        }
    }

    // Post-merge DISTINCT + ORDER BY + LIMIT (parsed from original SQL)
    if (merged.ok() && merged.rows.size() > 1) {
        try {
            zeptodb::sql::Parser post_parser;
            auto post_stmt = post_parser.parse(sql);

            // DISTINCT: remove duplicate rows
            if (post_stmt.distinct) {
                std::set<std::vector<int64_t>> seen;
                auto& rows = merged.rows;
                rows.erase(std::remove_if(rows.begin(), rows.end(),
                    [&seen](const std::vector<int64_t>& r) {
                        return !seen.insert(r).second;
                    }), rows.end());
            }

            if (post_stmt.order_by && !post_stmt.order_by->items.empty()) {
                // Resolve all ORDER BY columns to indices
                struct SortKey { int col; bool asc; };
                std::vector<SortKey> keys;
                for (const auto& ob : post_stmt.order_by->items) {
                    for (size_t i = 0; i < merged.column_names.size(); ++i) {
                        if (merged.column_names[i] == ob.column) {
                            keys.push_back({static_cast<int>(i), ob.asc});
                            break;
                        }
                    }
                }
                if (!keys.empty()) {
                    std::sort(merged.rows.begin(), merged.rows.end(),
                        [&keys](const std::vector<int64_t>& a,
                                const std::vector<int64_t>& b) {
                            for (const auto& k : keys) {
                                if (a[k.col] != b[k.col])
                                    return k.asc ? a[k.col] < b[k.col]
                                                 : a[k.col] > b[k.col];
                            }
                            return false;
                        });
                }
            }

            if (post_stmt.limit && *post_stmt.limit >= 0) {
                size_t lim = static_cast<size_t>(*post_stmt.limit);
                if (merged.rows.size() > lim)
                    merged.rows.resize(lim);
            }
        } catch (...) {}
    }

    return merged;
}

} // namespace zeptodb::cluster
