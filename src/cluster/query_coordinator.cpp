// ============================================================================
// Phase C-3: QueryCoordinator implementation
// ============================================================================

#include "zeptodb/cluster/query_coordinator.h"
#include "zeptodb/sql/parser.h"
#include "zeptodb/sql/ast.h"
#include "zeptodb/common/logger.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <future>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>

namespace zeptodb::cluster {

static constexpr size_t kSmallTableBroadcastJoinRowLimit = 4096;

static zeptodb::sql::QueryResultSet make_query_error(std::string error) {
    zeptodb::sql::QueryResultSet result;
    result.error = std::move(error);
    return result;
}

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
// DDL replication (fire-and-forget — devlog 112)
// ============================================================================
// After a successful local DDL execution, HttpServer calls this to replicate
// the same SQL string to every remote pod so the cluster converges on a
// shared schema.  Remote failures are logged but never thrown — DDL is
// idempotent (CREATE/DROP ... IF [NOT] EXISTS) and a down pod will re-sync
// on its next schema-touching op.  Strong consistency is out of scope.
// ============================================================================
void QueryCoordinator::forward_ddl_to_remotes(const std::string& sql) {
    // Snapshot remote endpoints under the lock — keep shared_ptrs alive so
    // a concurrent remove_node() cannot invalidate our iterator.
    std::vector<std::shared_ptr<NodeEndpoint>> remotes;
    {
        std::shared_lock lock(mutex_);
        for (const auto& ep : endpoints_) {
            if (!ep->is_local && ep->rpc) remotes.push_back(ep);
        }
    }
    for (const auto& ep : remotes) {
        try {
            auto result = ep->rpc->execute_sql(sql);
            if (!result.ok()) {
                ZEPTO_WARN("DDL replication to node {} ({}:{}) failed: {}",
                           ep->addr.id, ep->addr.host, ep->addr.port,
                           result.error);
            }
        } catch (const std::exception& e) {
            ZEPTO_WARN("DDL replication to node {} ({}:{}) exception: {}",
                       ep->addr.id, ep->addr.host, ep->addr.port, e.what());
        }
    }
}

bool QueryCoordinator::set_table_placement(const std::string& table_name,
                                           TablePlacementPolicy policy,
                                           NodeId node_id,
                                           std::string* error) {
    std::optional<zeptodb::storage::TableSchema> schema;
    {
        std::shared_lock lock(mutex_);
        schema = schema_snapshot_locked(table_name);
    }
    if (!schema) {
        if (error) *error = "table '" + table_name + "' is not declared";
        return false;
    }
    try {
        auto rlock = router_write_lock();
        if (policy == TablePlacementPolicy::PinnedNode) {
            const auto nodes = router().all_nodes();
            if (std::find(nodes.begin(), nodes.end(), node_id) == nodes.end()) {
                if (error) {
                    *error = "pinned table placement node " +
                             std::to_string(node_id) +
                             " is not registered";
                }
                return false;
            }
        }
        router().set_table_placement(schema->table_id, policy, node_id);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    return true;
}

bool QueryCoordinator::clear_table_placement(const std::string& table_name,
                                             std::string* error) {
    std::optional<zeptodb::storage::TableSchema> schema;
    {
        std::shared_lock lock(mutex_);
        schema = schema_snapshot_locked(table_name);
    }
    if (!schema) {
        if (error) *error = "table '" + table_name + "' is not declared";
        return false;
    }
    auto rlock = router_write_lock();
    router().clear_table_placement(schema->table_id);
    return true;
}

QueryCoordinator::SmallTableJoinTelemetrySnapshot
QueryCoordinator::small_table_join_stats() const {
    SmallTableJoinTelemetrySnapshot snapshot;
    snapshot.candidates = small_table_join_stats_.candidates.load(
        std::memory_order_relaxed);
    snapshot.accepted = small_table_join_stats_.accepted.load(
        std::memory_order_relaxed);
    snapshot.rejected_row_cap = small_table_join_stats_.rejected_row_cap.load(
        std::memory_order_relaxed);
    snapshot.errors = small_table_join_stats_.errors.load(
        std::memory_order_relaxed);
    snapshot.rows_materialized = small_table_join_stats_.rows_materialized.load(
        std::memory_order_relaxed);
    snapshot.last_left_rows = small_table_join_stats_.last_left_rows.load(
        std::memory_order_relaxed);
    snapshot.last_right_rows = small_table_join_stats_.last_right_rows.load(
        std::memory_order_relaxed);
    return snapshot;
}

void QueryCoordinator::reset_small_table_join_stats() {
    small_table_join_stats_.candidates.store(0, std::memory_order_relaxed);
    small_table_join_stats_.accepted.store(0, std::memory_order_relaxed);
    small_table_join_stats_.rejected_row_cap.store(0, std::memory_order_relaxed);
    small_table_join_stats_.errors.store(0, std::memory_order_relaxed);
    small_table_join_stats_.rows_materialized.store(0, std::memory_order_relaxed);
    small_table_join_stats_.last_left_rows.store(0, std::memory_order_relaxed);
    small_table_join_stats_.last_right_rows.store(0, std::memory_order_relaxed);
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

std::optional<std::string> QueryCoordinator::extract_table_name(
    const std::string& sql) const
{
    try {
        zeptodb::sql::Parser parser;
        auto stmt = parser.parse_statement(sql);
        switch (stmt.kind) {
            case zeptodb::sql::ParsedStatement::Kind::SELECT:
                if (stmt.select && !stmt.select->from_table.empty())
                    return stmt.select->from_table;
                break;
            case zeptodb::sql::ParsedStatement::Kind::INSERT:
                if (stmt.insert) return stmt.insert->table_name;
                break;
            case zeptodb::sql::ParsedStatement::Kind::UPDATE:
                if (stmt.update) return stmt.update->table_name;
                break;
            case zeptodb::sql::ParsedStatement::Kind::DELETE:
                if (stmt.del) return stmt.del->table_name;
                break;
            default:
                break;
        }
    } catch (...) {
        // Parse error — fall back to legacy symbol-only routing.
    }
    return std::nullopt;
}

uint16_t QueryCoordinator::resolve_routing_table_id_locked(
    const std::optional<std::string>& table_name) const
{
    if (!table_name || table_name->empty()) return 0;
    auto schema = schema_snapshot_locked(*table_name);
    return schema ? schema->table_id : 0;
}

std::optional<zeptodb::storage::TableSchema>
QueryCoordinator::schema_snapshot_locked(const std::string& table_name) const
{
    if (table_name.empty()) return std::nullopt;
    for (const auto& ep : endpoints_) {
        if (!ep->is_local || ep->pipeline == nullptr) continue;
        auto schema = ep->pipeline->schema_registry().get(table_name);
        if (schema) return schema;
    }
    return std::nullopt;
}

static std::string lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static std::optional<SymbolId> insert_routing_symbol(
    const zeptodb::sql::InsertStmt& stmt,
    const zeptodb::storage::TableSchema& schema)
{
    std::vector<std::string> column_order = stmt.columns;
    if (column_order.empty()) {
        column_order.reserve(schema.columns.size());
        for (const auto& column : schema.columns) {
            column_order.push_back(column.name);
        }
    }

    std::optional<size_t> symbol_col;
    for (size_t i = 0; i < column_order.size(); ++i) {
        if (lower_ascii(column_order[i]) == "symbol") {
            symbol_col = i;
            break;
        }
    }

    if (!symbol_col) {
        return SymbolId{0};
    }
    if (stmt.value_rows.empty()) {
        return std::nullopt;
    }

    std::optional<SymbolId> route_symbol;
    for (const auto& row : stmt.value_rows) {
        if (*symbol_col >= row.size()) {
            return std::nullopt;
        }
        const auto& value = row[*symbol_col];
        if (value.type == zeptodb::sql::InsertValue::STRING) {
            return std::nullopt;
        }
        const int64_t raw = value.type == zeptodb::sql::InsertValue::FLOAT
            ? static_cast<int64_t>(value.f)
            : value.i;
        if (raw < 0 ||
            raw > static_cast<int64_t>(std::numeric_limits<SymbolId>::max())) {
            return std::nullopt;
        }
        const auto symbol = static_cast<SymbolId>(raw);
        if (!route_symbol) {
            route_symbol = symbol;
        } else if (*route_symbol != symbol) {
            return std::nullopt;
        }
    }
    return route_symbol;
}

static bool is_string_encoded_type(zeptodb::storage::ColumnType type) {
    return type == zeptodb::storage::ColumnType::SYMBOL ||
           type == zeptodb::storage::ColumnType::STRING;
}

static double result_cell_as_f64(const zeptodb::sql::QueryResultSet& result,
                                 size_t row_idx,
                                 size_t col_idx)
{
    if (row_idx < result.typed_rows.size() &&
        col_idx < result.typed_rows[row_idx].size()) {
        return result.typed_rows[row_idx][col_idx].f;
    }
    if (row_idx >= result.rows.size() || col_idx >= result.rows[row_idx].size()) {
        return 0.0;
    }
    return std::bit_cast<double>(result.rows[row_idx][col_idx]);
}

static std::optional<std::string> decoded_string_cell(
    const zeptodb::sql::QueryResultSet& result,
    size_t row_idx,
    size_t col_idx)
{
    if (col_idx >= result.column_types.size() ||
        !is_string_encoded_type(result.column_types[col_idx])) {
        return std::nullopt;
    }

    size_t strings_per_row = 0;
    size_t string_pos_in_row = 0;
    bool found = false;
    for (size_t ci = 0; ci < result.column_types.size(); ++ci) {
        if (!is_string_encoded_type(result.column_types[ci])) continue;
        if (ci == col_idx) {
            string_pos_in_row = strings_per_row;
            found = true;
        }
        ++strings_per_row;
    }
    if (!found || strings_per_row == 0) return std::nullopt;

    const size_t offset = row_idx * strings_per_row + string_pos_in_row;
    if (offset < result.string_rows.size()) return result.string_rows[offset];

    if (result.symbol_dict != nullptr &&
        row_idx < result.rows.size() &&
        col_idx < result.rows[row_idx].size()) {
        const int64_t raw = result.rows[row_idx][col_idx];
        if (raw >= 0 &&
            raw <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
            return std::string(
                result.symbol_dict->lookup(static_cast<uint32_t>(raw)));
        }
    }
    return std::nullopt;
}

static void snapshot_decoded_string_rows(zeptodb::sql::QueryResultSet& result) {
    if (!result.string_rows.empty()) {
        result.symbol_dict = nullptr;
        return;
    }
    if (result.symbol_dict == nullptr) return;

    size_t strings_per_row = 0;
    for (const auto type : result.column_types) {
        if (is_string_encoded_type(type)) ++strings_per_row;
    }
    if (strings_per_row == 0) {
        result.symbol_dict = nullptr;
        return;
    }

    result.string_rows.reserve(result.rows.size() * strings_per_row);
    for (const auto& row : result.rows) {
        for (size_t ci = 0; ci < result.column_types.size() && ci < row.size(); ++ci) {
            if (!is_string_encoded_type(result.column_types[ci])) continue;
            const int64_t raw = row[ci];
            if (raw < 0 ||
                raw > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                result.string_rows.emplace_back();
                continue;
            }
            result.string_rows.emplace_back(
                result.symbol_dict->lookup(static_cast<uint32_t>(raw)));
        }
    }
    result.symbol_dict = nullptr;
}

static std::optional<zeptodb::storage::TableSchema> infer_schema_from_result(
    const std::string& table_name,
    const zeptodb::sql::QueryResultSet& result)
{
    if (table_name.empty() || result.column_names.empty()) return std::nullopt;
    zeptodb::storage::TableSchema schema;
    schema.table_name = table_name;
    schema.columns.reserve(result.column_names.size());
    for (size_t ci = 0; ci < result.column_names.size(); ++ci) {
        zeptodb::storage::ColumnDef col;
        col.name = result.column_names[ci];
        col.type = ci < result.column_types.size()
            ? result.column_types[ci]
            : zeptodb::storage::ColumnType::INT64;
        schema.columns.push_back(std::move(col));
    }
    schema.table_id = zeptodb::storage::SchemaRegistry::stable_table_id(table_name);
    return schema;
}

static int find_timestamp_sort_column(const zeptodb::sql::QueryResultSet& result) {
    for (size_t ci = 0; ci < result.column_names.size(); ++ci) {
        if (lower_ascii(result.column_names[ci]) == "timestamp") {
            return static_cast<int>(ci);
        }
    }
    for (size_t ci = 0; ci < result.column_names.size(); ++ci) {
        if (lower_ascii(result.column_names[ci]) == "timestamp_ns") {
            return static_cast<int>(ci);
        }
    }
    for (size_t ci = 0; ci < result.column_types.size(); ++ci) {
        if (result.column_types[ci] == zeptodb::storage::ColumnType::TIMESTAMP_NS) {
            return static_cast<int>(ci);
        }
    }
    return -1;
}

static void sort_result_by_column(zeptodb::sql::QueryResultSet& result,
                                  size_t sort_col)
{
    if (result.rows.size() < 2) return;

    std::vector<size_t> order(result.rows.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
        [&](size_t lhs, size_t rhs) {
            const int64_t lv = sort_col < result.rows[lhs].size()
                ? result.rows[lhs][sort_col]
                : 0;
            const int64_t rv = sort_col < result.rows[rhs].size()
                ? result.rows[rhs][sort_col]
                : 0;
            return lv < rv;
        });

    auto old_rows = std::move(result.rows);
    result.rows.reserve(old_rows.size());
    for (size_t idx : order) result.rows.push_back(std::move(old_rows[idx]));

    if (result.typed_rows.size() == order.size()) {
        auto old_typed_rows = std::move(result.typed_rows);
        result.typed_rows.reserve(old_typed_rows.size());
        for (size_t idx : order) {
            result.typed_rows.push_back(std::move(old_typed_rows[idx]));
        }
    }

    const size_t strings_per_row = static_cast<size_t>(
        std::count_if(result.column_types.begin(), result.column_types.end(),
            [](zeptodb::storage::ColumnType type) {
                return type == zeptodb::storage::ColumnType::SYMBOL ||
                       type == zeptodb::storage::ColumnType::STRING;
            }));
    if (strings_per_row > 0 &&
        result.string_rows.size() >= order.size() * strings_per_row) {
        auto old_string_rows = std::move(result.string_rows);
        result.string_rows.reserve(old_string_rows.size());
        for (size_t idx : order) {
            const size_t base = idx * strings_per_row;
            for (size_t si = 0; si < strings_per_row; ++si) {
                result.string_rows.push_back(std::move(old_string_rows[base + si]));
            }
        }
    }
}

static bool materialize_result_into_typed_table(
    zeptodb::core::ZeptoPipeline& tmp,
    const zeptodb::storage::TableSchema& schema,
    const zeptodb::sql::QueryResultSet& result,
    std::string& error)
{
    if (schema.table_name.empty() || schema.columns.empty()) return false;

    if (!tmp.schema_registry().exists(schema.table_name) &&
        !tmp.schema_registry().create(schema.table_name, schema.columns)) {
        error = "failed to create temporary table '" + schema.table_name + "'";
        return false;
    }
    const uint16_t table_id = tmp.schema_registry().get_table_id(schema.table_name);
    if (table_id == 0) {
        error = "temporary table '" + schema.table_name + "' has no table id";
        return false;
    }

    std::unordered_map<std::string, size_t> source_cols;
    source_cols.reserve(result.column_names.size());
    for (size_t ci = 0; ci < result.column_names.size(); ++ci) {
        source_cols.emplace(lower_ascii(result.column_names[ci]), ci);
    }

    std::vector<int> source_index;
    source_index.reserve(schema.columns.size());
    for (const auto& col : schema.columns) {
        const auto it = source_cols.find(lower_ascii(col.name));
        source_index.push_back(it == source_cols.end()
            ? -1
            : static_cast<int>(it->second));
    }

    for (size_t ri = 0; ri < result.rows.size(); ++ri) {
        zeptodb::core::TypedRowMessage row;
        row.table_id = table_id;
        row.timestamp = 0;
        row.symbol_id = 0;
        row.columns.reserve(schema.columns.size());

        bool has_timestamp = false;
        for (size_t ci = 0; ci < schema.columns.size(); ++ci) {
            const auto& schema_col = schema.columns[ci];
            const int src_idx = source_index[ci];
            const int64_t raw = (src_idx >= 0 &&
                                 static_cast<size_t>(src_idx) < result.rows[ri].size())
                ? result.rows[ri][static_cast<size_t>(src_idx)]
                : 0;

            zeptodb::core::TypedColumnValue value;
            value.name = schema_col.name;
            value.type = schema_col.type;

            switch (schema_col.type) {
                case zeptodb::storage::ColumnType::INT32:
                case zeptodb::storage::ColumnType::INT64:
                case zeptodb::storage::ColumnType::TIMESTAMP_NS:
                    value.i64 = raw;
                    break;
                case zeptodb::storage::ColumnType::FLOAT32:
                case zeptodb::storage::ColumnType::FLOAT64:
                    value.f64 = src_idx >= 0
                        ? result_cell_as_f64(result, ri, static_cast<size_t>(src_idx))
                        : 0.0;
                    break;
                case zeptodb::storage::ColumnType::SYMBOL:
                case zeptodb::storage::ColumnType::STRING:
                    if (raw < 0 ||
                        raw > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                        error = "temporary table string code out of range for column '" +
                                schema_col.name + "'";
                        return false;
                    }
                    value.u32 = static_cast<uint32_t>(raw);
                    if (src_idx >= 0) {
                        auto decoded = decoded_string_cell(
                            result, ri, static_cast<size_t>(src_idx));
                        if (decoded) {
                            value.has_string_value = true;
                            value.string_value = std::move(*decoded);
                        }
                    }
                    break;
                case zeptodb::storage::ColumnType::BOOL:
                    value.u8 = raw == 0 ? uint8_t{0} : uint8_t{1};
                    break;
            }

            const std::string lower_name = lower_ascii(schema_col.name);
            if (lower_name == "symbol") {
                int64_t sym = raw;
                if (schema_col.type == zeptodb::storage::ColumnType::SYMBOL ||
                    schema_col.type == zeptodb::storage::ColumnType::STRING) {
                    sym = static_cast<int64_t>(value.u32);
                }
                if (sym >= 0 &&
                    sym <= static_cast<int64_t>(
                        std::numeric_limits<zeptodb::SymbolId>::max())) {
                    row.symbol_id = static_cast<zeptodb::SymbolId>(sym);
                }
            }
            if (lower_name == "timestamp" || lower_name == "timestamp_ns" ||
                (!has_timestamp &&
                 schema_col.type == zeptodb::storage::ColumnType::TIMESTAMP_NS)) {
                row.timestamp = raw;
                has_timestamp = true;
            }

            row.columns.push_back(std::move(value));
        }

        if (!tmp.ingest_typed_row(std::move(row))) {
            error = "failed to materialize row " + std::to_string(ri) +
                    " into temporary table '" + schema.table_name + "'";
            return false;
        }
    }

    if (!result.rows.empty()) {
        tmp.schema_registry().mark_has_data(table_id);
    }
    return true;
}

static bool is_small_table_hash_join_candidate(
    const zeptodb::sql::SelectStmt& stmt)
{
    if (!stmt.join || stmt.from_table.empty() || stmt.join->table.empty()) {
        return false;
    }
    if (stmt.from_subquery || !stmt.cte_defs.empty() ||
        stmt.set_op != zeptodb::sql::SelectStmt::SetOp::NONE || stmt.rhs) {
        return false;
    }

    const auto join_type = stmt.join->type;
    const bool hash_join_type =
        join_type == zeptodb::sql::JoinClause::Type::INNER ||
        join_type == zeptodb::sql::JoinClause::Type::LEFT ||
        join_type == zeptodb::sql::JoinClause::Type::RIGHT ||
        join_type == zeptodb::sql::JoinClause::Type::FULL;
    if (!hash_join_type) return false;

    for (const auto& cond : stmt.join->on_conditions) {
        if (cond.op == zeptodb::sql::CompareOp::EQ &&
            !cond.left_col.empty() && !cond.right_col.empty()) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Public execute_sql
// ============================================================================

zeptodb::sql::QueryResultSet QueryCoordinator::execute_sql_for_symbol(
    const std::string& sql, SymbolId symbol_id)
{
    // Resolve the owning endpoint under the lock, then release before I/O.
    std::shared_ptr<NodeEndpoint> target;
    const auto table_name = extract_table_name(sql);
    {
        std::shared_lock lock(mutex_);
        if (endpoints_.empty()) {
            zeptodb::sql::QueryResultSet err;
            err.error = "QueryCoordinator: no nodes registered";
            return err;
        }
        const uint16_t table_id = resolve_routing_table_id_locked(table_name);
        auto rlock = router_read_lock();
        NodeId owner = (table_id == 0)
            ? router().route(symbol_id)
            : router().route(table_id, symbol_id);
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

zeptodb::sql::QueryResultSet QueryCoordinator::execute_small_table_hash_join(
    const std::string& sql,
    const zeptodb::sql::SelectStmt& stmt)
{
    small_table_join_stats_.candidates.fetch_add(1, std::memory_order_relaxed);

    auto fetch_small_table = [this](const std::string& table_name) {
        const std::string fetch_sql = "SELECT * FROM " + table_name +
            " LIMIT " + std::to_string(kSmallTableBroadcastJoinRowLimit + 1);
        auto results = scatter(fetch_sql);
        auto merged = merge_concat_results(results);
        if (!merged.ok()) {
            small_table_join_stats_.errors.fetch_add(1, std::memory_order_relaxed);
            return merged;
        }
        if (merged.rows.size() > kSmallTableBroadcastJoinRowLimit) {
            small_table_join_stats_.rejected_row_cap.fetch_add(
                1, std::memory_order_relaxed);
            return make_query_error(
                "small-table broadcast JOIN row limit exceeded for table '" +
                table_name + "' (limit " +
                std::to_string(kSmallTableBroadcastJoinRowLimit) + ")");
        }
        return merged;
    };

    std::optional<zeptodb::storage::TableSchema> left_schema;
    std::optional<zeptodb::storage::TableSchema> right_schema;
    {
        std::shared_lock lock(mutex_);
        left_schema = schema_snapshot_locked(stmt.from_table);
        right_schema = schema_snapshot_locked(stmt.join->table);
    }

    auto left_rows = fetch_small_table(stmt.from_table);
    if (!left_rows.ok()) return left_rows;
    auto right_rows = stmt.join->table == stmt.from_table
        ? left_rows
        : fetch_small_table(stmt.join->table);
    if (!right_rows.ok()) return right_rows;
    small_table_join_stats_.last_left_rows.store(left_rows.rows.size(),
                                                std::memory_order_relaxed);
    small_table_join_stats_.last_right_rows.store(right_rows.rows.size(),
                                                 std::memory_order_relaxed);

    if (!left_schema) {
        left_schema = infer_schema_from_result(stmt.from_table, left_rows);
    }
    if (!right_schema) {
        right_schema = infer_schema_from_result(stmt.join->table, right_rows);
    }
    if (!left_schema) {
        small_table_join_stats_.errors.fetch_add(1, std::memory_order_relaxed);
        return make_query_error(
            "small-table broadcast JOIN could not resolve schema for table '" +
            stmt.from_table + "'");
    }
    if (!right_schema) {
        small_table_join_stats_.errors.fetch_add(1, std::memory_order_relaxed);
        return make_query_error(
            "small-table broadcast JOIN could not resolve schema for table '" +
            stmt.join->table + "'");
    }

    zeptodb::core::PipelineConfig tcfg;
    tcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline tmp(tcfg);
    tmp.start();

    std::string replay_error;
    if (!materialize_result_into_typed_table(
            tmp, *left_schema, left_rows, replay_error)) {
        tmp.stop();
        small_table_join_stats_.errors.fetch_add(1, std::memory_order_relaxed);
        return make_query_error(
            replay_error.empty()
                ? "failed to materialize small-table broadcast JOIN left side"
                : replay_error);
    }
    if (stmt.join->table != stmt.from_table &&
        !materialize_result_into_typed_table(
            tmp, *right_schema, right_rows, replay_error)) {
        tmp.stop();
        small_table_join_stats_.errors.fetch_add(1, std::memory_order_relaxed);
        return make_query_error(
            replay_error.empty()
                ? "failed to materialize small-table broadcast JOIN right side"
                : replay_error);
    }

    zeptodb::sql::QueryExecutor local_ex(tmp);
    auto result = local_ex.execute(sql);
    snapshot_decoded_string_rows(result);
    tmp.stop();
    if (result.ok()) {
        small_table_join_stats_.accepted.fetch_add(1, std::memory_order_relaxed);
        small_table_join_stats_.rows_materialized.fetch_add(
            left_rows.rows.size() + right_rows.rows.size(),
            std::memory_order_relaxed);
    } else {
        small_table_join_stats_.errors.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
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
            try {
                zeptodb::sql::Parser parser;
                auto parsed = parser.parse_statement(sql);
                if (parsed.insert) {
                    std::optional<zeptodb::storage::TableSchema> schema;
                    {
                        std::shared_lock lock(mutex_);
                        schema = schema_snapshot_locked(parsed.insert->table_name);
                    }
                    if (schema) {
                        auto route_symbol =
                            insert_routing_symbol(*parsed.insert, *schema);
                        if (route_symbol) {
                            return execute_sql_for_symbol(sql, *route_symbol);
                        }
                    }
                }
            } catch (...) {
                // Fall through to the legacy route below so the executor
                // surfaces the parse/materialization error.
            }
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

    // Tier A-3: bounded small-table hash JOIN.
    // Operational control tables (runbooks, recommendations, suppressions)
    // are usually tiny compared with hot time-series partitions.  For simple
    // equi hash JOINs, gather both sides under a strict row cap, materialize
    // them into a coordinator-local typed pipeline, then reuse local hash JOIN
    // semantics.  Broader cost-based distributed JOIN planning remains future
    // work.
    {
        try {
            zeptodb::sql::Parser parser;
            auto stmt = parser.parse(sql);
            if (is_small_table_hash_join_candidate(stmt)) {
                return execute_small_table_hash_join(sql, stmt);
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
                std::optional<zeptodb::storage::TableSchema> schema_snapshot;
                {
                    std::shared_lock lock(mutex_);
                    for (const auto& ep : endpoints_) {
                        if (!ep->is_local || ep->pipeline == nullptr) continue;
                        schema_snapshot =
                            ep->pipeline->schema_registry().get(real_table);
                        if (schema_snapshot) break;
                    }
                }
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

                bool needs_timestamp_order = false;
                for (const auto& col : wstmt.columns) {
                    if (col.agg == zeptodb::sql::AggFunc::FIRST ||
                        col.agg == zeptodb::sql::AggFunc::LAST) {
                        needs_timestamp_order = true;
                        break;
                    }
                }
                if (needs_timestamp_order) {
                    const int ts_col = find_timestamp_sort_column(base_merged);
                    if (ts_col >= 0) {
                        sort_result_by_column(base_merged,
                            static_cast<size_t>(ts_col));
                    }
                }

                // Ingest into a temporary local pipeline
                zeptodb::core::PipelineConfig tcfg;
                tcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
                zeptodb::core::ZeptoPipeline tmp(tcfg);
                tmp.start();

                std::optional<zeptodb::storage::TableSchema> replay_schema =
                    schema_snapshot
                        ? schema_snapshot
                        : infer_schema_from_result(real_table, base_merged);

                if (replay_schema) {
                    std::string replay_error;
                    if (!materialize_result_into_typed_table(
                            tmp, *replay_schema, base_merged, replay_error)) {
                        tmp.stop();
                        zeptodb::sql::QueryResultSet err;
                        err.error = replay_error.empty()
                            ? "failed to materialize temporary table '" +
                              replay_schema->table_name + "'"
                            : replay_error;
                        return err;
                    }
                } else {
                    // Legacy fallback for pre-DDL tick-shaped data.
                    // Map columns: find symbol/price/volume/timestamp indices.
                    int ci_sym = -1, ci_price = -1, ci_vol = -1, ci_ts = -1;
                    for (size_t i = 0; i < base_merged.column_names.size(); ++i) {
                        const auto& n = base_merged.column_names[i];
                        if (n == "symbol")    ci_sym   = static_cast<int>(i);
                        if (n == "price")     ci_price = static_cast<int>(i);
                        if (n == "volume")    ci_vol   = static_cast<int>(i);
                        if (n == "timestamp") ci_ts    = static_cast<int>(i);
                    }
                    for (auto& row : base_merged.rows) {
                        zeptodb::ingestion::TickMessage msg{};
                        if (ci_sym >= 0)   msg.symbol_id = static_cast<uint32_t>(row[ci_sym]);
                        if (ci_price >= 0) msg.price     = row[ci_price];
                        if (ci_vol >= 0)   msg.volume    = row[ci_vol];
                        if (ci_ts >= 0)    msg.recv_ts   = row[ci_ts];
                        tmp.store_tick_direct(msg);
                    }
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
