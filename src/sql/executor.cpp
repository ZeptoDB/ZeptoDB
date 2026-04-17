// ============================================================================
// ZeptoDB: SQL Query Executor Implementation
// ============================================================================
// SelectStmt AST를 ZeptoPipeline API로 변환 실행
// ============================================================================

#include "zeptodb/sql/executor.h"
#include "zeptodb/sql/parser.h"
#include "zeptodb/sql/mv_rewriter.h"
#include "zeptodb/execution/join_operator.h"
#include "zeptodb/execution/query_planner.h"
#include "zeptodb/execution/window_function.h"
#include "zeptodb/execution/vectorized_engine.h"
#include "zeptodb/execution/parallel_scan.h"
#include "zeptodb/execution/local_scheduler.h"
#include "zeptodb/common/logger.h"

#ifdef ZEPTO_ENABLE_DUCKDB
#include "zeptodb/execution/duckdb_engine.h"
#include "zeptodb/execution/arrow_bridge.h"
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

namespace zeptodb::sql {

// Thread-local cancellation token — set before execute(), cleared after.
// Checked at partition scan boundaries without changing any inner function signatures.
static thread_local zeptodb::auth::CancellationToken* tl_cancel_token = nullptr;

// Helper: returns true if the current query should be aborted
static inline bool is_cancelled() {
    return tl_cancel_token && tl_cancel_token->is_cancelled();
}

// Thread-local CTE map — populated in execute() before exec_select(), cleared after.
// Makes CTEs visible to recursive exec_select() calls (set operations, subqueries).
static thread_local std::unordered_map<std::string, QueryResultSet> tl_cte_map;

// ── Resolve value_expr in WHERE Expr tree ──────────────────────────────────
// Pre-evaluates constant ArithExpr (NOW(), INTERVAL) into int64 values
// so all downstream comparison code works unchanged.
// Forward-declared; implemented after eval_arith.
static void resolve_value_exprs(std::shared_ptr<Expr>& expr);
static void resolve_where_exprs(std::optional<WhereClause>& wc) {
    if (wc && wc->expr) resolve_value_exprs(wc->expr);
}

// ── OFFSET + LIMIT helper ──────────────────────────────────────────────────
// Apply OFFSET then LIMIT to result rows (works on both rows and typed_rows).
static void apply_offset_limit(QueryResultSet& r, const SelectStmt& stmt) {
    if (!stmt.offset && !stmt.limit) return;
    size_t off = stmt.offset.value_or(0);
    if (off > 0 && off < r.rows.size()) {
        r.rows.erase(r.rows.begin(), r.rows.begin() + static_cast<ptrdiff_t>(off));
        if (r.typed_rows.size() > off)
            r.typed_rows.erase(r.typed_rows.begin(), r.typed_rows.begin() + static_cast<ptrdiff_t>(off));
    } else if (off >= r.rows.size()) {
        r.rows.clear();
        r.typed_rows.clear();
    }
    if (stmt.limit && r.rows.size() > static_cast<size_t>(*stmt.limit)) {
        r.rows.resize(static_cast<size_t>(*stmt.limit));
        if (r.typed_rows.size() > static_cast<size_t>(*stmt.limit))
            r.typed_rows.resize(static_cast<size_t>(*stmt.limit));
    }
}

// ── Template helpers for type-dispatched comparisons ────────────────────────

// ── SAMPLE helper: filter row indices by deterministic hash ─────────────────
// Uses a fast integer hash (splitmix64-style) so results are reproducible for
// the same data.  Threshold = rate * 2^32 — avoids floating-point per row.
static void apply_sample(std::vector<uint32_t>& indices, double rate) {
    if (rate >= 1.0) return;
    const uint32_t threshold = static_cast<uint32_t>(rate * 4294967296.0);
    size_t out = 0;
    for (size_t i = 0; i < indices.size(); ++i) {
        uint64_t h = indices[i];
        h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
        h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
        h ^= (h >> 31);
        if (static_cast<uint32_t>(h) < threshold)
            indices[out++] = indices[i];
    }
    indices.resize(out);
}

template<typename T>
static bool compare_val(T v, CompareOp op, T cmp) {
    switch (op) {
        case CompareOp::EQ: return v == cmp;
        case CompareOp::NE: return v != cmp;
        case CompareOp::GT: return v >  cmp;
        case CompareOp::LT: return v <  cmp;
        case CompareOp::GE: return v >= cmp;
        case CompareOp::LE: return v <= cmp;
    }
    return false;
}
static inline int64_t store_double(double v) { return std::bit_cast<int64_t>(v); }

// ============================================================================
// Virtual table helpers (CTE / FROM-subquery execution path)
// ============================================================================

// Retrieve a column value from a virtual row by column name.
// Ignores table_alias — virtual tables are a flat namespace.
static inline int64_t vt_col_val(
    const std::string& col,
    const QueryResultSet& src,
    const std::unordered_map<std::string, size_t>& col_idx,
    size_t row_idx)
{
    auto it = col_idx.find(col);
    if (it == col_idx.end()) return 0;
    return src.rows[row_idx][it->second];
}

// Float-aware: read native double from typed_rows if available
static inline double vt_col_dval(
    const std::string& col,
    const QueryResultSet& src,
    const std::unordered_map<std::string, size_t>& col_idx,
    size_t row_idx)
{
    auto it = col_idx.find(col);
    if (it == col_idx.end()) return 0.0;
    size_t ci = it->second;
    if (row_idx < src.typed_rows.size())
        return src.typed_rows[row_idx][ci].f;
    return static_cast<double>(src.rows[row_idx][ci]);
}

// Evaluate an ArithExpr against a virtual-table row.
static int64_t eval_arith_vt(const ArithExpr& e,
                              const QueryResultSet& src,
                              const std::unordered_map<std::string, size_t>& col_idx,
                              size_t row_idx)
{
    switch (e.kind) {
        case ArithExpr::Kind::LITERAL:
            return e.literal;
        case ArithExpr::Kind::COLUMN:
            return vt_col_val(e.column, src, col_idx, row_idx);
        case ArithExpr::Kind::BINARY: {
            int64_t l = eval_arith_vt(*e.left,  src, col_idx, row_idx);
            int64_t r = eval_arith_vt(*e.right, src, col_idx, row_idx);
            switch (e.arith_op) {
                case ArithOp::ADD: return l + r;
                case ArithOp::SUB: return l - r;
                case ArithOp::MUL: return l * r;
                case ArithOp::DIV: return (r == 0) ? 0 : l / r;
            }
            break;
        }
        case ArithExpr::Kind::FUNC:
            if (e.func_name == "substr") {
                int64_t arg = e.func_arg ? eval_arith_vt(*e.func_arg, src, col_idx, row_idx) : 0;
                std::string s = std::to_string(arg);
                int64_t start = e.func_unit.empty() ? 1 : std::stoll(e.func_unit);
                if (start < 1) start = 1;
                size_t pos = static_cast<size_t>(start - 1);
                if (pos >= s.size()) return 0;
                int64_t len = e.func_arg2
                    ? eval_arith_vt(*e.func_arg2, src, col_idx, row_idx)
                    : static_cast<int64_t>(s.size() - pos);
                std::string sub = s.substr(pos, static_cast<size_t>(len));
                try { return std::stoll(sub); } catch (...) { return 0; }
            }
            if (e.func_arg) return eval_arith_vt(*e.func_arg, src, col_idx, row_idx);
            return 0;
    }
    return 0;
}

// Evaluate a WHERE Expr condition against a single virtual-table row.
static bool eval_expr_vt(const std::shared_ptr<Expr>& expr,
                          const QueryResultSet& src,
                          const std::unordered_map<std::string, size_t>& col_idx,
                          size_t row_idx)
{
    if (!expr) return true;
    switch (expr->kind) {
        case Expr::Kind::AND:
            return eval_expr_vt(expr->left,  src, col_idx, row_idx)
                && eval_expr_vt(expr->right, src, col_idx, row_idx);
        case Expr::Kind::OR:
            return eval_expr_vt(expr->left,  src, col_idx, row_idx)
                || eval_expr_vt(expr->right, src, col_idx, row_idx);
        case Expr::Kind::NOT:
            return !eval_expr_vt(expr->left, src, col_idx, row_idx);
        case Expr::Kind::COMPARE: {
            if (expr->is_float)
                return compare_val(vt_col_dval(expr->column, src, col_idx, row_idx),
                                   expr->op, expr->value_f);
            int64_t v = vt_col_val(expr->column, src, col_idx, row_idx);
            return compare_val(v, expr->op, expr->value);
        }
        case Expr::Kind::BETWEEN: {
            int64_t v = vt_col_val(expr->column, src, col_idx, row_idx);
            return v >= expr->lo && v <= expr->hi;
        }
        case Expr::Kind::IN: {
            int64_t v = vt_col_val(expr->column, src, col_idx, row_idx);
            for (int64_t iv : expr->in_values)
                if (v == iv) return true;
            return false;
        }
        case Expr::Kind::IS_NULL: {
            int64_t v = vt_col_val(expr->column, src, col_idx, row_idx);
            bool is_null = (v == INT64_MIN);
            return expr->negated ? !is_null : is_null;
        }
        case Expr::Kind::LIKE: {
            int64_t v = vt_col_val(expr->column, src, col_idx, row_idx);
            std::string s = std::to_string(v);
            const std::string& pat = expr->like_pattern;
            size_t m = s.size(), n = pat.size();
            std::vector<std::vector<bool>> dp(m+1, std::vector<bool>(n+1, false));
            dp[0][0] = true;
            for (size_t j = 1; j <= n; ++j)
                dp[0][j] = dp[0][j-1] && pat[j-1] == '%';
            for (size_t i = 1; i <= m; ++i)
                for (size_t j = 1; j <= n; ++j) {
                    if (pat[j-1] == '%')                      dp[i][j] = dp[i-1][j] || dp[i][j-1];
                    else if (pat[j-1] == '_' || pat[j-1] == s[i-1]) dp[i][j] = dp[i-1][j-1];
                }
            bool matched = dp[m][n];
            return expr->negated ? !matched : matched;
        }
    }
    return true;
}

// Resolve a SELECT column's output value from a virtual-table row.
static int64_t sel_val_vt(const SelectExpr& sel,
                           const QueryResultSet& src,
                           const std::unordered_map<std::string, size_t>& col_idx,
                           size_t row_idx)
{
    if (sel.case_when) {
        // CASE WHEN on virtual table: evaluate branches against virtual row
        for (const auto& branch : sel.case_when->branches) {
            // Simplified: check if the column value matches the condition
            // Full virtual-table CASE WHEN evaluation would need eval_expr_single_vt
            // For now, fall through to arith_expr or column
        }
    }
    if (sel.arith_expr) return eval_arith_vt(*sel.arith_expr, src, col_idx, row_idx);
    return vt_col_val(sel.column, src, col_idx, row_idx);
}

// ============================================================================
// VectorHash: composite GROUP BY key hashing
// ============================================================================
struct VectorHash {
    size_t operator()(const std::vector<int64_t>& v) const noexcept {
        size_t seed = v.size();
        for (int64_t x : v) {
            seed ^= static_cast<size_t>(x) + 0x9e3779b9u
                  + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};

using namespace zeptodb::core;
using namespace zeptodb::storage;
using namespace zeptodb::execution;

// ============================================================================
// 고해상도 타이머
// ============================================================================
static inline double now_us() {
    return std::chrono::duration<double, std::micro>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// 생성자
// ============================================================================
QueryExecutor::QueryExecutor(ZeptoPipeline& pipeline)
    : pipeline_(pipeline)
{
    // 기본: LocalQueryScheduler (hardware_concurrency 스레드)
    auto local = std::make_unique<zeptodb::execution::LocalQueryScheduler>(pipeline);
    pool_raw_  = &local->pool();
    scheduler_ = std::move(local);
}

QueryExecutor::QueryExecutor(ZeptoPipeline& pipeline,
                             std::unique_ptr<zeptodb::execution::QueryScheduler> sched)
    : pipeline_(pipeline)
    , scheduler_(std::move(sched))
    , pool_raw_(nullptr)  // 비-로컬 스케줄러: 직렬 폴백
{}

QueryExecutor::~QueryExecutor() = default;

void QueryExecutor::enable_parallel(size_t num_threads, size_t row_threshold) {
    par_opts_.enabled      = true;
    par_opts_.num_threads  = num_threads;
    par_opts_.row_threshold = row_threshold;
    // 지정된 스레드 수로 LocalQueryScheduler 재생성
    auto local = std::make_unique<zeptodb::execution::LocalQueryScheduler>(
        pipeline_, num_threads);
    pool_raw_  = &local->pool();
    scheduler_ = std::move(local);
}

void QueryExecutor::disable_parallel() {
    par_opts_.enabled = false;
    // 단일 스레드 스케줄러로 교체
    auto local = std::make_unique<zeptodb::execution::LocalQueryScheduler>(
        pipeline_, 1);
    pool_raw_  = &local->pool();
    scheduler_ = std::move(local);
}

const PipelineStats& QueryExecutor::stats() const {
    return pipeline_.stats();
}

// ============================================================================
// Main execution entry points
// ============================================================================
// Build a human-readable query plan for EXPLAIN without executing the query.
static QueryResultSet build_explain_plan(const SelectStmt& stmt,
                                          ZeptoPipeline& pipeline)
{
    QueryResultSet result;
    result.column_names = {"plan"};
    result.column_types = {ColumnType::INT64};

    auto line = [&](std::string s) {
        result.string_rows.push_back(std::move(s));
    };

    // Determine which plan path would be chosen
    bool has_agg = false;
    bool has_group = stmt.group_by.has_value();
    bool has_join  = stmt.join.has_value();
    for (auto& col : stmt.columns)
        if (col.agg != AggFunc::NONE) { has_agg = true; break; }

    std::string operation;
    std::string path_detail;
    if (has_group) {
        const auto& gb = stmt.group_by.value();
        if (gb.columns.size() == 1) {
            const std::string& gc = gb.columns[0];
            bool is_sym = (gc == "symbol");
            operation = is_sym ? "GroupAggregation/SymbolPartition" : "GroupAggregation/SingleCol";
            if (!is_sym)
                path_detail = "flat_hash(int64) + sorted-scan-cache + flat_GroupState";
        } else {
            operation = "GroupAggregation/MultiCol";
            path_detail = "VectorHash<vector<int64_t>>";
        }
    } else if (has_agg) {
        operation = "Aggregation";
        path_detail = "single-pass column scan";
    } else if (has_join) {
        operation = "Join";
        switch (stmt.join->type) {
            case JoinClause::Type::ASOF:       path_detail = "AsOf binary-search"; break;
            case JoinClause::Type::AJ0:        path_detail = "AsOf binary-search (left-cols only)"; break;
            case JoinClause::Type::WINDOW:     path_detail = "Window binary-search O(n log m)"; break;
            case JoinClause::Type::FULL:       path_detail = "full outer hash join"; break;
            case JoinClause::Type::UNION_JOIN: path_detail = "union join (kdb+ uj)"; break;
            case JoinClause::Type::PLUS:       path_detail = "plus join (kdb+ pj)"; break;
            default:                           path_detail = "hash join"; break;
        }
    } else {
        operation = "TableScan";
        path_detail = "sequential column read";
    }

    // Partition info
    auto& pm = pipeline.partition_manager();
    size_t total_parts = pm.partition_count();
    size_t total_rows  = 0;
    for (auto* p : pm.get_all_partitions())
        total_rows += p->num_rows();

    line("Operation:  " + operation);
    if (!path_detail.empty())
        line("Path:       " + path_detail);
    line("Table:      " + (stmt.from_table.empty() ? "(subquery)" : stmt.from_table));
    if (stmt.where.has_value())
        line("Filter:     (WHERE clause present)");
    if (has_group) {
        std::string gb_str;
        for (auto& c : stmt.group_by->columns) {
            if (!gb_str.empty()) gb_str += ", ";
            gb_str += c;
        }
        line("GroupBy:    " + gb_str);
    }
    std::string agg_list;
    for (auto& col : stmt.columns) {
        if (col.agg == AggFunc::NONE) continue;
        if (!agg_list.empty()) agg_list += ", ";
        if (!col.alias.empty()) agg_list += col.alias;
        else if (!col.column.empty()) agg_list += col.column;
        else agg_list += "*";
    }
    if (!agg_list.empty())
        line("Aggregates: " + agg_list);
    if (stmt.order_by.has_value())
        line("OrderBy:    (ORDER BY clause present)");
    if (stmt.limit.has_value())
        line("Limit:      " + std::to_string(stmt.limit.value()));
    if (stmt.offset.has_value())
        line("Offset:     " + std::to_string(stmt.offset.value()));
    if (stmt.sample_rate.has_value()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4g", *stmt.sample_rate);
        line(std::string("Sample:     ") + buf + " (" +
             std::to_string(static_cast<int>(*stmt.sample_rate * 100)) + "% of rows)");
    }
    line("Partitions: " + std::to_string(total_parts));
    line("TotalRows:  " + std::to_string(total_rows));
    if (stmt.group_by.has_value() && stmt.group_by->columns.size() == 1
        && !stmt.group_by->xbar_buckets.empty())
        line("XbarBucket: " + std::to_string(stmt.group_by->xbar_buckets[0])
             + " ns  (sorted-scan-cache active for monotonic timestamps)");

    return result;
}

// ============================================================================
// DDL helpers: map type string → ColumnType
// ============================================================================
static storage::ColumnType ddl_type_from_str(const std::string& t) {
    if (t == "INT64"  || t == "BIGINT" || t == "LONG")  return storage::ColumnType::INT64;
    if (t == "INT32"  || t == "INT"    || t == "INTEGER") return storage::ColumnType::INT32;
    if (t == "FLOAT64"|| t == "DOUBLE")                  return storage::ColumnType::FLOAT64;
    if (t == "FLOAT32"|| t == "FLOAT"  || t == "REAL")   return storage::ColumnType::FLOAT32;
    if (t == "TIMESTAMP" || t == "TIMESTAMP_NS")         return storage::ColumnType::TIMESTAMP_NS;
    if (t == "SYMBOL")                                   return storage::ColumnType::SYMBOL;
    if (t == "STRING" || t == "VARCHAR" || t == "TEXT")   return storage::ColumnType::STRING;
    if (t == "BOOL"   || t == "BOOLEAN")                 return storage::ColumnType::BOOL;
    throw std::runtime_error("Unknown DDL column type: " + t);
}

static QueryResultSet ddl_ok(const std::string& msg) {
    QueryResultSet r;
    r.column_names = {"result"};
    r.string_rows  = {msg};
    return r;
}

// ============================================================================
// exec_create_table
// ============================================================================
QueryResultSet QueryExecutor::exec_create_table(const CreateTableStmt& stmt) {
    std::vector<storage::ColumnDef> cols;
    cols.reserve(stmt.columns.size());
    for (auto& c : stmt.columns)
        cols.push_back({c.column, ddl_type_from_str(c.type_str)});

    bool ok = pipeline_.schema_registry().create(stmt.table_name, std::move(cols));
    if (!ok) {
        if (stmt.if_not_exists)
            return ddl_ok("Table '" + stmt.table_name + "' already exists (IF NOT EXISTS)");
        QueryResultSet err;
        err.error = "Table '" + stmt.table_name + "' already exists";
        return err;
    }
    return ddl_ok("Table '" + stmt.table_name + "' created");
}

// ============================================================================
// exec_drop_table
// ============================================================================
QueryResultSet QueryExecutor::exec_drop_table(const DropTableStmt& stmt) {
    bool ok = pipeline_.schema_registry().drop(stmt.table_name);
    if (!ok) {
        if (stmt.if_exists)
            return ddl_ok("Table '" + stmt.table_name + "' does not exist (IF EXISTS)");
        QueryResultSet err;
        err.error = "Table '" + stmt.table_name + "' does not exist";
        return err;
    }
    return ddl_ok("Table '" + stmt.table_name + "' dropped");
}

// ============================================================================
// exec_alter_table
// ============================================================================
QueryResultSet QueryExecutor::exec_alter_table(const AlterTableStmt& stmt) {
    auto& reg = pipeline_.schema_registry();

    if (!reg.exists(stmt.table_name)) {
        QueryResultSet err;
        err.error = "Table '" + stmt.table_name + "' does not exist";
        return err;
    }

    if (stmt.action == AlterTableStmt::Action::ADD_COLUMN) {
        storage::ColumnDef col{stmt.col_def.column,
                               ddl_type_from_str(stmt.col_def.type_str)};
        reg.add_column(stmt.table_name, std::move(col));
        return ddl_ok("Column '" + stmt.col_def.column + "' added to '" + stmt.table_name + "'");
    }

    if (stmt.action == AlterTableStmt::Action::DROP_COLUMN) {
        bool ok = reg.drop_column(stmt.table_name, stmt.col_name);
        if (!ok) {
            QueryResultSet err;
            err.error = "Column '" + stmt.col_name + "' not found in '" + stmt.table_name + "'";
            return err;
        }
        return ddl_ok("Column '" + stmt.col_name + "' dropped from '" + stmt.table_name + "'");
    }

    if (stmt.action == AlterTableStmt::Action::SET_ATTRIBUTE) {
        // Apply attribute to all partitions for this table's symbol
        auto partitions = pipeline_.partition_manager().get_all_partitions();
        size_t applied = 0;
        for (auto* part : partitions) {
            if (!part->get_column(stmt.col_name)) continue;
            if (stmt.attr_type == "SORTED")       part->set_sorted(stmt.col_name);
            else if (stmt.attr_type == "GROUPED")  part->set_grouped(stmt.col_name);
            else if (stmt.attr_type == "PARTED")   part->set_parted(stmt.col_name);
            ++applied;
        }
        return ddl_ok("Attribute " + stmt.attr_type + " set on column '" + stmt.col_name
                      + "' (" + std::to_string(applied) + " partitions)");
    }

    if (stmt.action == AlterTableStmt::Action::SET_STORAGE_POLICY) {
        if (pipeline_.flush_manager()) {
            auto& fm = *pipeline_.flush_manager();
            // Update tiering policy on the FlushManager's config
            // We need mutable access — use a setter method
            fm.set_tiering_policy(stmt.warm_after_ns, stmt.cold_after_ns, stmt.drop_after_ns);

            std::string desc;
            if (stmt.warm_after_ns > 0)
                desc += "WARM=" + std::to_string(stmt.warm_after_ns / 3'600'000'000'000LL) + "h ";
            if (stmt.cold_after_ns > 0)
                desc += "COLD=" + std::to_string(stmt.cold_after_ns / 3'600'000'000'000LL) + "h ";
            if (stmt.drop_after_ns > 0)
                desc += "DROP=" + std::to_string(stmt.drop_after_ns / 86'400'000'000'000LL) + "d";
            return ddl_ok("Storage policy set for '" + stmt.table_name + "': " + desc);
        }
        return ddl_ok("Storage policy configured (FlushManager not active)");
    }

    // SET TTL
    const int64_t multiplier = (stmt.ttl_unit == "HOURS")
        ? 3'600'000'000'000LL    // ns per hour
        : 86'400'000'000'000LL;  // ns per day (default DAYS)
    const int64_t ttl_ns = stmt.ttl_value * multiplier;
    reg.set_ttl(stmt.table_name, ttl_ns);

    // Apply immediately: evict partitions already beyond TTL
    if (ttl_ns > 0) {
        const int64_t cutoff = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()) - ttl_ns;
        pipeline_.evict_older_than_ns(cutoff);

        // Also update FlushManager so future ticks are auto-evicted
        if (pipeline_.flush_manager()) {
            // Use the minimum TTL across all tables
            const int64_t min_ttl = reg.min_ttl_ns();
            pipeline_.flush_manager()->set_ttl(min_ttl > 0 ? min_ttl : ttl_ns);
        }
    }

    return ddl_ok("TTL set to " + std::to_string(stmt.ttl_value) + " " + stmt.ttl_unit
                  + " for table '" + stmt.table_name + "'");
}

// ============================================================================
// exec_alter_table — SET STORAGE POLICY
// ============================================================================
// (handled inside exec_alter_table, but split here for clarity)
// The actual dispatch is in exec_alter_table above; we add the case there.

// ============================================================================
// exec_create_mv — CREATE MATERIALIZED VIEW name AS SELECT ...
// Parses the SELECT to extract GROUP BY + aggregations, registers with
// MaterializedViewManager for incremental updates on each tick.
// ============================================================================
QueryResultSet QueryExecutor::exec_create_mv(const CreateMVStmt& stmt) {
    auto& mgr = pipeline_.mat_view_manager();

    if (mgr.exists(stmt.view_name)) {
        QueryResultSet err;
        err.error = "Materialized view '" + stmt.view_name + "' already exists";
        return err;
    }

    // Extract MVDef from the SELECT statement
    storage::MVDef def;
    def.view_name    = stmt.view_name;
    def.source_table = stmt.query->from_table;

    // Extract xbar bucket from GROUP BY
    if (stmt.query->group_by.has_value()) {
        for (size_t i = 0; i < stmt.query->group_by->columns.size(); ++i) {
            if (stmt.query->group_by->xbar_buckets.size() > i &&
                stmt.query->group_by->xbar_buckets[i] > 0) {
                def.xbar_bucket = stmt.query->group_by->xbar_buckets[i];
            } else {
                def.group_by.push_back(stmt.query->group_by->columns[i]);
            }
        }
    }

    // Extract aggregation columns from SELECT list
    for (auto& col : stmt.query->columns) {
        if (col.agg == sql::AggFunc::NONE) continue;

        storage::MVColumnDef mc;
        mc.name = col.alias.empty() ? col.column : col.alias;

        switch (col.agg) {
            case sql::AggFunc::SUM:   mc.agg = storage::MVAggType::SUM;   break;
            case sql::AggFunc::COUNT: mc.agg = storage::MVAggType::COUNT; break;
            case sql::AggFunc::MIN:   mc.agg = storage::MVAggType::MIN;   break;
            case sql::AggFunc::MAX:   mc.agg = storage::MVAggType::MAX;   break;
            case sql::AggFunc::FIRST: mc.agg = storage::MVAggType::FIRST; break;
            case sql::AggFunc::LAST:  mc.agg = storage::MVAggType::LAST;  break;
            default: continue;
        }
        mc.source_col = col.column;
        def.columns.push_back(std::move(mc));
    }

    if (def.columns.empty()) {
        QueryResultSet err;
        err.error = "Materialized view must have at least one aggregation column";
        return err;
    }

    mgr.create_view(std::move(def));
    return ddl_ok("Materialized view '" + stmt.view_name + "' created");
}

// ============================================================================
// exec_drop_mv — DROP MATERIALIZED VIEW name
// ============================================================================
QueryResultSet QueryExecutor::exec_drop_mv(const DropMVStmt& stmt) {
    auto& mgr = pipeline_.mat_view_manager();
    if (!mgr.drop_view(stmt.view_name)) {
        QueryResultSet err;
        err.error = "Materialized view '" + stmt.view_name + "' not found";
        return err;
    }
    return ddl_ok("Materialized view '" + stmt.view_name + "' dropped");
}

// ============================================================================
// exec_insert — INSERT INTO table VALUES (...)
// Maps column values to TickMessage fields and calls pipeline_.ingest_tick().
// Default column order: symbol, price, volume, timestamp
// ============================================================================
QueryResultSet QueryExecutor::exec_insert(const InsertStmt& stmt) {
    // Resolve column order: explicit columns or default
    std::vector<std::string> cols = stmt.columns;
    if (cols.empty()) {
        cols = {"symbol", "price", "volume", "timestamp"};
    }

    // Build column-name → index map
    std::unordered_map<std::string, size_t> col_idx;
    for (size_t i = 0; i < cols.size(); ++i) {
        std::string lower = cols[i];
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        col_idx[lower] = i;
    }

    size_t inserted = 0;
    for (auto& row : stmt.value_rows) {
        if (row.size() != cols.size()) {
            QueryResultSet err;
            err.error = "Column count mismatch: expected " +
                        std::to_string(cols.size()) + ", got " +
                        std::to_string(row.size());
            return err;
        }

        zeptodb::ingestion::TickMessage msg{};

        auto get_i = [&](const std::string& name, int64_t def) -> int64_t {
            auto it = col_idx.find(name);
            if (it == col_idx.end()) return def;
            auto& v = row[it->second];
            if (v.type == InsertValue::STRING)
                return static_cast<int64_t>(pipeline_.symbol_dict().intern(v.s));
            return v.type == InsertValue::FLOAT ? static_cast<int64_t>(v.f) : v.i;
        };

        // Check if price is float
        auto price_it = col_idx.find("price");
        bool price_float = (price_it != col_idx.end()) && row[price_it->second].type == InsertValue::FLOAT;

        msg.symbol_id = static_cast<int32_t>(get_i("symbol", 0));
        if (price_float) {
            msg.price_f = row[price_it->second].f;
            msg.price_is_float = 1;
        } else {
            msg.price = get_i("price", 0);
            msg.price_is_float = 0;
        }
        msg.volume    = get_i("volume", 0);
        msg.recv_ts   = get_i("timestamp", static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));
        msg.seq_num   = static_cast<uint64_t>(pipeline_.stats().ticks_ingested.load());
        msg.msg_type  = 0;  // Trade

        pipeline_.ingest_tick(msg);
        ++inserted;
    }

    // Drain to ensure data is stored before returning
    pipeline_.drain_sync();

    QueryResultSet result;
    result.column_names = {"inserted"};
    result.column_types = {storage::ColumnType::INT64};
    result.rows = {{static_cast<int64_t>(inserted)}};
    return result;
}

// ============================================================================
// exec_update — UPDATE table SET col = val [, ...] WHERE ...
// In-place modification of matching rows via as_span().
// ============================================================================
QueryResultSet QueryExecutor::exec_update(const UpdateStmt& stmt) {
    SelectStmt sel;
    sel.from_table = stmt.table_name;
    sel.from_alias = "";
    sel.where = stmt.where;

    auto partitions = find_partitions(stmt.table_name);

    // Filter partitions by symbol if WHERE has symbol = X
    int64_t sym_filter = -1;
    has_where_symbol(sel, sym_filter, "");

    size_t updated = 0;
    for (auto* part : partitions) {
        if (sym_filter >= 0 && part->key().symbol_id != static_cast<int32_t>(sym_filter))
            continue;

        size_t nrows = part->num_rows();
        if (nrows == 0) continue;

        auto matching = eval_where(sel, *part, nrows);

        for (auto& assign : stmt.assignments) {
            auto* col = part->get_column(assign.column);
            if (!col) continue;
            auto span = col->as_span<int64_t>();
            for (uint32_t idx : matching) {
                if (idx < span.size()) {
                    span[idx] = assign.value;
                }
            }
        }
        updated += matching.size();
    }

    QueryResultSet result;
    result.column_names = {"updated"};
    result.column_types = {storage::ColumnType::INT64};
    result.rows = {{static_cast<int64_t>(updated)}};
    return result;
}

// ============================================================================
// exec_delete — DELETE FROM table WHERE ...
// In-place compaction: shift kept rows down, then shrink column size.
// ============================================================================
QueryResultSet QueryExecutor::exec_delete(const DeleteStmt& stmt) {
    SelectStmt sel;
    sel.from_table = stmt.table_name;
    sel.from_alias = "";
    sel.where = stmt.where;

    auto partitions = find_partitions(stmt.table_name);

    int64_t sym_filter = -1;
    has_where_symbol(sel, sym_filter, "");

    size_t deleted = 0;
    for (auto* part : partitions) {
        if (sym_filter >= 0 && part->key().symbol_id != static_cast<int32_t>(sym_filter))
            continue;

        size_t nrows = part->num_rows();
        if (nrows == 0) continue;

        auto matching = eval_where(sel, *part, nrows);
        if (matching.empty()) continue;

        std::vector<bool> del_mask(nrows, false);
        for (uint32_t idx : matching) del_mask[idx] = true;

        size_t new_size = nrows - matching.size();
        for (auto& col_ptr : part->columns()) {
            auto span = col_ptr->as_span<int64_t>();
            size_t write = 0;
            for (size_t read = 0; read < nrows; ++read) {
                if (!del_mask[read]) {
                    span[write++] = span[read];
                }
            }
            col_ptr->set_size(new_size);
        }

        deleted += matching.size();
    }

    QueryResultSet result;
    result.column_names = {"deleted"};
    result.column_types = {storage::ColumnType::INT64};
    result.rows = {{static_cast<int64_t>(deleted)}};
    return result;
}

QueryResultSet QueryExecutor::execute(const std::string& sql) {
    tl_cte_map.clear();
    double t0 = now_us();
    try {
        // ── Prepared statement cache: reuse parsed AST ───────────────────────
        size_t h = sql_hash(sql);
        ParsedStatement ps;
        {
            std::lock_guard<std::mutex> lk(stmt_cache_mu_);
            auto it = stmt_cache_.find(h);
            if (it != stmt_cache_.end()) {
                ps = it->second;
            } else {
                Parser parser;
                ps = parser.parse_statement(sql);
                if (stmt_cache_.size() < 4096) // cap cache size
                    stmt_cache_.emplace(h, ps);
            }
        }

        // ── DML/DDL: not cacheable, invalidate result cache ──────────────────
        if (ps.kind == ParsedStatement::Kind::CREATE_TABLE) {
            auto r = exec_create_table(*ps.create_table);
            r.execution_time_us = now_us() - t0;
            return r;
        }
        if (ps.kind == ParsedStatement::Kind::DROP_TABLE) {
            invalidate_result_cache(ps.drop_table->table_name);
            auto r = exec_drop_table(*ps.drop_table);
            r.execution_time_us = now_us() - t0;
            return r;
        }
        if (ps.kind == ParsedStatement::Kind::ALTER_TABLE) {
            invalidate_result_cache(ps.alter_table->table_name);
            auto r = exec_alter_table(*ps.alter_table);
            r.execution_time_us = now_us() - t0;
            return r;
        }
        if (ps.kind == ParsedStatement::Kind::INSERT) {
            invalidate_result_cache(ps.insert->table_name);
            auto r = exec_insert(*ps.insert);
            r.execution_time_us = now_us() - t0;
            return r;
        }
        if (ps.kind == ParsedStatement::Kind::UPDATE) {
            invalidate_result_cache(ps.update->table_name);
            auto r = exec_update(*ps.update);
            r.execution_time_us = now_us() - t0;
            return r;
        }
        if (ps.kind == ParsedStatement::Kind::DELETE) {
            invalidate_result_cache(ps.del->table_name);
            auto r = exec_delete(*ps.del);
            r.execution_time_us = now_us() - t0;
            return r;
        }
        if (ps.kind == ParsedStatement::Kind::CREATE_MV) {
            auto r = exec_create_mv(*ps.create_mv);
            r.execution_time_us = now_us() - t0;
            return r;
        }
        if (ps.kind == ParsedStatement::Kind::DROP_MV) {
            auto r = exec_drop_mv(*ps.drop_mv);
            r.execution_time_us = now_us() - t0;
            return r;
        }
        if (ps.kind == ParsedStatement::Kind::SHOW_TABLES) {
            QueryResultSet r;
            r.column_names = {"name", "rows"};
            r.column_types = {ColumnType::SYMBOL, ColumnType::INT64};
            auto tables = pipeline_.schema_registry().list_tables();
            std::sort(tables.begin(), tables.end());
            auto all_parts = pipeline_.partition_manager().get_all_partitions();
            for (auto& tbl : tables) {
                size_t cnt = 0;
                for (auto* p : all_parts) cnt += p->num_rows();
                r.string_rows.push_back(tbl);
                r.rows.push_back({static_cast<int64_t>(cnt)});
            }
            r.execution_time_us = now_us() - t0;
            return r;
        }
        if (ps.kind == ParsedStatement::Kind::DESCRIBE_TABLE) {
            QueryResultSet r;
            auto schema = pipeline_.schema_registry().get(ps.describe_table_name);
            if (!schema) {
                r.error = "Table not found: " + ps.describe_table_name;
                return r;
            }
            r.column_names = {"column", "type"};
            r.column_types = {ColumnType::SYMBOL, ColumnType::SYMBOL};
            for (auto& col : schema->columns) {
                const char* ts;
                switch (col.type) {
                    case ColumnType::INT64:     ts = "INT64"; break;
                    case ColumnType::INT32:     ts = "INT32"; break;
                    case ColumnType::FLOAT64:   ts = "FLOAT64"; break;
                    case ColumnType::FLOAT32:   ts = "FLOAT32"; break;
                    case ColumnType::TIMESTAMP_NS: ts = "TIMESTAMP"; break;
                    case ColumnType::SYMBOL:    ts = "SYMBOL"; break;
                    case ColumnType::BOOL:      ts = "BOOL"; break;
                    default:                    ts = "UNKNOWN"; break;
                }
                r.string_rows.push_back(col.name);
                r.string_rows.push_back(ts);
                r.rows.push_back({});  // one row per column
            }
            r.execution_time_us = now_us() - t0;
            return r;
        }

        // SELECT path
        const SelectStmt& stmt = *ps.select;

        // EXPLAIN: return query plan without executing
        if (stmt.explain) {
            // For complex queries, build cost-based physical plan
            if (needs_cost_planning(stmt)) {
                for (auto* p : pipeline_.partition_manager().get_all_partitions()) {
                    if (!table_stats_.has_partition(p))
                        table_stats_.update_partition(p);
                }

                auto logical = execution::LogicalPlan::build(stmt);
                execution::LogicalPlan::optimize(logical);
                auto physical = execution::PhysicalPlanner::plan(logical, table_stats_);

                QueryResultSet result;
                result.column_names = {"plan"};
                result.column_types = {ColumnType::INT64};
                execution::format_explain_tree(physical, result.string_rows);
                result.execution_time_us = now_us() - t0;
                return result;
            }
            // Simple queries: use existing text-based EXPLAIN
            auto result = build_explain_plan(stmt, pipeline_);
            result.execution_time_us = now_us() - t0;
            return result;
        }

        // ── Query result cache: check for cached SELECT result ───────────────
        bool cacheable = result_cache_max_ > 0 && stmt.cte_defs.empty();
        if (cacheable) {
            std::lock_guard<std::mutex> lk(result_cache_mu_);
            auto it = result_cache_.find(h);
            if (it != result_cache_.end()) {
                double now_mono = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
                if (now_mono < it->second.expire_time) {
                    auto cached = it->second.result;
                    cached.execution_time_us = now_us() - t0;
                    return cached;
                }
                result_cache_.erase(it); // expired
            }
        }

        // Execute each CTE definition and store result in the thread-local map.
        for (auto& cte : stmt.cte_defs) {
            auto res = exec_select(*cte.stmt);
            if (!res.ok()) { tl_cte_map.clear(); return res; }
            tl_cte_map[cte.name] = std::move(res);
        }

        // ── MV query rewrite: skip full scan if a matching MV exists ─────
        auto mv_hit = MVRewriter::try_rewrite(stmt, pipeline_.mat_view_manager());
        if (mv_hit) {
            QueryResultSet result;
            result.column_names = std::move(mv_hit->view_result.column_names);
            result.rows = std::move(mv_hit->view_result.rows);
            result.column_types.resize(result.column_names.size(),
                                       storage::ColumnType::INT64);
            result.execution_time_us = now_us() - t0;
            result.symbol_dict = &pipeline_.symbol_dict();
            if (stmt.order_by) apply_order_by(result, stmt);
            if (stmt.limit) {
                int64_t lim = *stmt.limit;
                int64_t off = stmt.offset.value_or(0);
                if (off < static_cast<int64_t>(result.rows.size())) {
                    auto b = result.rows.begin() + off;
                    auto e = (off + lim < static_cast<int64_t>(result.rows.size()))
                             ? b + lim : result.rows.end();
                    result.rows = {b, e};
                } else {
                    result.rows.clear();
                }
            }
            tl_cte_map.clear();
            return result;
        }

        auto result = exec_select(stmt);
        result.execution_time_us = now_us() - t0;
        result.symbol_dict = &pipeline_.symbol_dict();
        tl_cte_map.clear();

        // ── Store in result cache ────────────────────────────────────────────
        if (cacheable && result.ok()) {
            std::lock_guard<std::mutex> lk(result_cache_mu_);
            if (result_cache_.size() >= result_cache_max_) {
                // Evict oldest entry
                auto oldest = result_cache_.begin();
                for (auto it = result_cache_.begin(); it != result_cache_.end(); ++it)
                    if (it->second.expire_time < oldest->second.expire_time) oldest = it;
                result_cache_.erase(oldest);
            }
            double now_mono = static_cast<double>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
            result_cache_[h] = {result, now_mono + result_cache_ttl_s_};
        }

        return result;
    } catch (const std::exception& e) {
        tl_cte_map.clear();
        QueryResultSet err;
        err.error = e.what();
        err.execution_time_us = now_us() - t0;
        return err;
    }
}

QueryResultSet QueryExecutor::execute(const std::string& sql,
                                       zeptodb::auth::CancellationToken* token)
{
    tl_cancel_token = token;
    auto result = execute(sql);
    tl_cancel_token = nullptr;
    return result;
}

// ============================================================================
// SELECT 실행 디스패처
// ============================================================================
QueryResultSet QueryExecutor::exec_select(const SelectStmt& stmt_in) {
    // Pre-resolve WHERE value_expr (NOW(), INTERVAL, etc.) into int64 values
    SelectStmt stmt = stmt_in;
    resolve_where_exprs(stmt.where);
    resolve_where_exprs(stmt.having);

    // Pre-resolve uncorrelated scalar/IN subqueries into literal values
    if (stmt.where && stmt.where->expr) {
        try {
            resolve_subqueries(stmt.where->expr);
        } catch (const std::exception& e) {
            QueryResultSet r;
            r.error = e.what();
            return r;
        }
    }

    // ── Cost-based planner: build physical plan for complex queries ──────────
    std::shared_ptr<execution::PhysicalNode> physical_plan;
    if (needs_cost_planning(stmt)) {
        // Lazily update statistics only for partitions not yet tracked
        for (auto* p : pipeline_.partition_manager().get_all_partitions()) {
            if (!table_stats_.has_partition(p))
                table_stats_.update_partition(p);
        }

        auto logical = execution::LogicalPlan::build(stmt);
        execution::LogicalPlan::optimize(logical);
        physical_plan = execution::PhysicalPlanner::plan(logical, table_stats_);
        ZEPTO_INFO("QueryPlanner: built physical plan for complex query");
    }

    // Set operations: UNION [ALL] / INTERSECT / EXCEPT
    if (stmt.set_op != SelectStmt::SetOp::NONE && stmt.rhs) {
        // Strip set_op from left side before executing
        SelectStmt left_stmt = stmt;
        left_stmt.set_op = SelectStmt::SetOp::NONE;
        left_stmt.rhs    = nullptr;

        QueryResultSet left  = exec_select(left_stmt);
        QueryResultSet right = exec_select(*stmt.rhs);

        if (!left.ok())  return left;
        if (!right.ok()) return right;

        QueryResultSet result;
        result.column_names = left.column_names;
        result.column_types = left.column_types;

        if (stmt.set_op == SelectStmt::SetOp::UNION_ALL) {
            result.rows = left.rows;
            result.rows.insert(result.rows.end(), right.rows.begin(), right.rows.end());
        } else if (stmt.set_op == SelectStmt::SetOp::UNION_DISTINCT) {
            // Deduplicate rows from both sides
            std::set<std::vector<int64_t>> seen;
            for (auto& row : left.rows)
                if (seen.insert(row).second) result.rows.push_back(row);
            for (auto& row : right.rows)
                if (seen.insert(row).second) result.rows.push_back(row);
        } else if (stmt.set_op == SelectStmt::SetOp::INTERSECT) {
            std::set<std::vector<int64_t>> right_set(right.rows.begin(), right.rows.end());
            for (auto& row : left.rows) {
                if (right_set.count(row))
                    result.rows.push_back(row);
            }
        } else if (stmt.set_op == SelectStmt::SetOp::EXCEPT) {
            std::set<std::vector<int64_t>> right_set(right.rows.begin(), right.rows.end());
            for (auto& row : left.rows) {
                if (!right_set.count(row))
                    result.rows.push_back(row);
            }
        }
        result.rows_scanned = left.rows_scanned + right.rows_scanned;
        return result;
    }

    // FROM (subquery): execute the inner query and use the result as the source.
    if (stmt.from_subquery) {
        auto sub_res = exec_select(*stmt.from_subquery);
        if (!sub_res.ok()) return sub_res;
        return exec_select_virtual(stmt, sub_res, stmt.from_alias);
    }

    // Materialized view reference: check if from_table names a registered MV.
    {
        auto& mgr = pipeline_.mat_view_manager();
        if (mgr.exists(stmt.from_table)) {
            auto vr = mgr.query(stmt.from_table);
            QueryResultSet mv_result;
            mv_result.column_names = std::move(vr.column_names);
            mv_result.column_types.resize(mv_result.column_names.size(),
                                          storage::ColumnType::INT64);
            mv_result.rows = std::move(vr.rows);
            // Apply WHERE/ORDER BY/LIMIT on the MV result
            if (stmt.where || stmt.order_by || stmt.limit) {
                const std::string& alias = stmt.from_alias.empty()
                                           ? stmt.from_table : stmt.from_alias;
                return exec_select_virtual(stmt, mv_result, alias);
            }
            return mv_result;
        }
    }

    // CTE reference: check if from_table names a CTE in the thread-local map.
    {
        auto cte_it = tl_cte_map.find(stmt.from_table);
        if (cte_it != tl_cte_map.end()) {
            const std::string& alias = stmt.from_alias.empty()
                                       ? stmt.from_table : stmt.from_alias;
            return exec_select_virtual(stmt, cte_it->second, alias);
        }
    }

#ifdef ZEPTO_ENABLE_DUCKDB
    // DuckDB offload: explicit duckdb('path') table function
    {
        std::string parquet_path;
        if (enable_duckdb_offload_ && is_duckdb_table_func(stmt.from_table, parquet_path)) {
            // Reject whitespace-only paths
            bool all_space = true;
            for (char c : parquet_path) if (c != ' ' && c != '\t') { all_space = false; break; }
            if (parquet_path.empty() || all_space) {
                QueryResultSet err;
                err.error = "DuckDB: empty parquet path";
                return err;
            }
            // Path traversal validation
            if (parquet_path.find("..") != std::string::npos || (!parquet_path.empty() && parquet_path[0] == '/')) {
                ZEPTO_WARN("DuckDB offload: rejected unsafe path '{}'", parquet_path);
                QueryResultSet err;
                err.error = "DuckDB: path traversal or absolute path not allowed";
                return err;
            }
            ZEPTO_INFO("DuckDB offload: explicit duckdb('{}') table function", parquet_path);
            // Escape path for SQL string literal
            std::string escaped_path;
            escaped_path.reserve(parquet_path.size());
            for (char c : parquet_path) {
                if (c == '\'') escaped_path += "''";
                else escaped_path += c;
            }
            std::string duckdb_sql = "SELECT * FROM read_parquet('" + escaped_path + "')";
            auto duckdb_result = exec_via_duckdb(duckdb_sql, {parquet_path});
            if (!duckdb_result.ok()) return duckdb_result;
            // Apply WHERE/ORDER BY/LIMIT/column selection from original AST
            const std::string& alias = stmt.from_alias.empty() ? stmt.from_table : stmt.from_alias;
            return exec_select_virtual(stmt, duckdb_result, alias);
        }
    }
#endif

    // Extract time range hint from WHERE for partition-level pruning.
    // This runs before any exec function so partitions outside the range are
    // never passed downstream — O(partitions) key comparison, no data access.
    int64_t ts_lo_filter = INT64_MIN, ts_hi_filter = INT64_MAX;
    bool has_ts_range = extract_time_range(stmt, ts_lo_filter, ts_hi_filter);

    // WHERE symbol = N 조건 추출 (파티션 레벨 필터링)
    int64_t sym_filter = -1;
    if (has_where_symbol(stmt, sym_filter, stmt.from_alias)) {
        // symbol 기반 파티션 필터링
        auto& pm = pipeline_.partition_manager();
        auto sym_parts = pm.get_partitions_for_symbol(
            static_cast<zeptodb::SymbolId>(sym_filter));

        // Further narrow by time range if present (avoids passing entire symbol
        // history to exec functions when a tight timestamp window is queried).
        std::vector<Partition*> left_parts;
        if (has_ts_range) {
            left_parts.reserve(sym_parts.size());
            for (auto* p : sym_parts) {
                if (p->overlaps_time_range(ts_lo_filter, ts_hi_filter))
                    left_parts.push_back(p);
            }
        } else {
            left_parts = std::move(sym_parts);
        }

        // ASOF JOIN
        if (stmt.join.has_value() && stmt.join->type == JoinClause::Type::ASOF) {
            auto right_parts = find_partitions(stmt.join->table);
            return exec_asof_join(stmt, left_parts, right_parts);
        }

        // AJ0 JOIN (left-columns-only asof join)
        if (stmt.join.has_value() && stmt.join->type == JoinClause::Type::AJ0) {
            auto right_parts = find_partitions(stmt.join->table);
            return exec_asof_join(stmt, left_parts, right_parts);
        }

        // Hash JOIN (INNER / LEFT / RIGHT / FULL)
        if (stmt.join.has_value() && (stmt.join->type == JoinClause::Type::INNER
                                   || stmt.join->type == JoinClause::Type::LEFT
                                   || stmt.join->type == JoinClause::Type::RIGHT
                                   || stmt.join->type == JoinClause::Type::FULL)) {
            auto right_parts = find_partitions(stmt.join->table);
            return exec_hash_join(stmt, left_parts, right_parts, physical_plan);
        }

        // UNION JOIN (kdb+ uj — merge columns from both tables)
        if (stmt.join.has_value() && stmt.join->type == JoinClause::Type::UNION_JOIN) {
            auto right_parts = find_partitions(stmt.join->table);
            return exec_union_join(stmt, left_parts, right_parts);
        }

        // PLUS JOIN (kdb+ pj — additive join)
        if (stmt.join.has_value() && stmt.join->type == JoinClause::Type::PLUS) {
            auto right_parts = find_partitions(stmt.join->table);
            return exec_plus_join(stmt, left_parts, right_parts);
        }

        // WINDOW JOIN (kdb+ wj 스타일)
        if (stmt.join.has_value() && stmt.join->type == JoinClause::Type::WINDOW) {
            auto right_parts = find_partitions(stmt.join->table);
            return exec_window_join(stmt, left_parts, right_parts);
        }

        bool has_agg = false;
        for (const auto& col : stmt.columns) {
            if (col.agg != AggFunc::NONE) { has_agg = true; break; }
        }

        QueryResultSet result;
        if (has_agg && stmt.group_by.has_value()) {
            result = exec_group_agg(stmt, left_parts);
        } else if (has_agg) {
            result = exec_agg(stmt, left_parts);
        } else {
            result = exec_simple_select(stmt, left_parts);
        }

        // 윈도우 함수 적용
        apply_window_functions(stmt, result);
        return result;
    }

    // WHERE symbol IN (1, 2, 3) → multi-partition routing
    std::vector<int64_t> in_syms;
    if (has_where_symbol_in(stmt, in_syms) && !in_syms.empty()) {
        auto& pm = pipeline_.partition_manager();
        std::vector<Partition*> left_parts;
        for (int64_t s : in_syms) {
            auto sp = pm.get_partitions_for_symbol(static_cast<zeptodb::SymbolId>(s));
            if (has_ts_range) {
                for (auto* p : sp)
                    if (p->overlaps_time_range(ts_lo_filter, ts_hi_filter))
                        left_parts.push_back(p);
            } else {
                left_parts.insert(left_parts.end(), sp.begin(), sp.end());
            }
        }

        bool has_agg = false;
        for (const auto& col : stmt.columns)
            if (col.agg != AggFunc::NONE) { has_agg = true; break; }

        QueryResultSet result;
        if (has_agg && stmt.group_by.has_value())
            result = exec_group_agg(stmt, left_parts);
        else if (has_agg)
            result = exec_agg(stmt, left_parts);
        else
            result = exec_simple_select(stmt, left_parts);

        apply_window_functions(stmt, result);
        return result;
    }

    // 심볼 필터 없음 → 전체 파티션 (타임스탬프 범위 있으면 파티션 수준 사전 필터링)
    std::vector<Partition*> left_parts;
    if (has_ts_range) {
        left_parts = pipeline_.partition_manager()
            .get_partitions_for_time_range(ts_lo_filter, ts_hi_filter);
    } else {
        left_parts = find_partitions(stmt.from_table);
    }

    // ASOF JOIN
    if (stmt.join.has_value() && stmt.join->type == JoinClause::Type::ASOF) {
        auto right_parts = find_partitions(stmt.join->table);
        return exec_asof_join(stmt, left_parts, right_parts);
    }

    // AJ0 JOIN
    if (stmt.join.has_value() && stmt.join->type == JoinClause::Type::AJ0) {
        auto right_parts = find_partitions(stmt.join->table);
        return exec_asof_join(stmt, left_parts, right_parts);
    }

    // Hash JOIN (INNER / LEFT / RIGHT / FULL)
    if (stmt.join.has_value() && (stmt.join->type == JoinClause::Type::INNER
                               || stmt.join->type == JoinClause::Type::LEFT
                               || stmt.join->type == JoinClause::Type::RIGHT
                               || stmt.join->type == JoinClause::Type::FULL)) {
        auto right_parts = find_partitions(stmt.join->table);
        return exec_hash_join(stmt, left_parts, right_parts, physical_plan);
    }

    // UNION JOIN
    if (stmt.join.has_value() && stmt.join->type == JoinClause::Type::UNION_JOIN) {
        auto right_parts = find_partitions(stmt.join->table);
        return exec_union_join(stmt, left_parts, right_parts);
    }

    // PLUS JOIN
    if (stmt.join.has_value() && stmt.join->type == JoinClause::Type::PLUS) {
        auto right_parts = find_partitions(stmt.join->table);
        return exec_plus_join(stmt, left_parts, right_parts);
    }

    // WINDOW JOIN (kdb+ wj 스타일)
    if (stmt.join.has_value() && stmt.join->type == JoinClause::Type::WINDOW) {
        auto right_parts = find_partitions(stmt.join->table);
        return exec_window_join(stmt, left_parts, right_parts);
    }

    // 집계 함수가 있는지 체크
    bool has_agg = false;
    for (const auto& col : stmt.columns) {
        if (col.agg != AggFunc::NONE) { has_agg = true; break; }
    }

    QueryResultSet result;
    if (has_agg && stmt.group_by.has_value()) {
        result = exec_group_agg(stmt, left_parts);
    } else if (has_agg) {
        result = exec_agg(stmt, left_parts);
    } else {
        result = exec_simple_select(stmt, left_parts);
    }

    // 윈도우 함수 적용
    apply_window_functions(stmt, result);
    return result;
}

// ============================================================================
// 파티션 목록 조회
// ============================================================================
std::vector<Partition*> QueryExecutor::find_partitions(
    const std::string& /*table_name*/)
{
    auto& pm = pipeline_.partition_manager();
    return pm.get_all_partitions();
}

// ============================================================================
// 컬럼 데이터 포인터 조회
// ============================================================================
const int64_t* QueryExecutor::get_col_data(
    const Partition& part,
    const std::string& col_name) const
{
    const ColumnVector* cv = part.get_column(col_name);
    if (!cv) return nullptr;
    return static_cast<const int64_t*>(cv->raw_data());
}

// Check if a column stores float data (FLOAT32/FLOAT64)
static bool col_is_float(const Partition& part, const std::string& col_name) {
    const ColumnVector* cv = part.get_column(col_name);
    if (!cv) return false;
    return cv->type() == ColumnType::FLOAT64 || cv->type() == ColumnType::FLOAT32;
}

// ── scan_compare: vectorized column scan with type dispatch ─────────────────
template<typename T>
static void scan_compare(const void* raw, size_t n, CompareOp op, T val,
                         std::vector<uint32_t>& out) {
    const T* data = static_cast<const T*>(raw);
    for (size_t i = 0; i < n; ++i) {
        if (compare_val(data[i], op, val))
            out.push_back(static_cast<uint32_t>(i));
    }
}

// ============================================================================
// WHERE Expr 평가 — 행 인덱스 벡터 반환
// ============================================================================
std::vector<uint32_t> QueryExecutor::eval_expr(
    const std::shared_ptr<Expr>& expr,
    const Partition& part,
    size_t num_rows,
    const std::string& default_alias)
{
    if (!expr) {
        std::vector<uint32_t> all(num_rows);
        for (size_t i = 0; i < num_rows; ++i) all[i] = static_cast<uint32_t>(i);
        return all;
    }

    switch (expr->kind) {
        case Expr::Kind::AND: {
            auto left  = eval_expr(expr->left,  part, num_rows, default_alias);
            auto right = eval_expr(expr->right, part, num_rows, default_alias);
            std::vector<uint32_t> result;
            result.reserve(std::min(left.size(), right.size()));
            std::set_intersection(left.begin(), left.end(),
                                  right.begin(), right.end(),
                                  std::back_inserter(result));
            return result;
        }
        case Expr::Kind::OR: {
            auto left  = eval_expr(expr->left,  part, num_rows, default_alias);
            auto right = eval_expr(expr->right, part, num_rows, default_alias);
            std::vector<uint32_t> result;
            result.reserve(left.size() + right.size());
            std::set_union(left.begin(), left.end(),
                           right.begin(), right.end(),
                           std::back_inserter(result));
            return result;
        }
        case Expr::Kind::BETWEEN: {
            if (expr->column == "symbol") {
                std::vector<uint32_t> all(num_rows);
                for (size_t i = 0; i < num_rows; ++i) all[i] = static_cast<uint32_t>(i);
                return all;
            }
            const int64_t* data = get_col_data(part, expr->column);
            if (!data) return {};
            std::vector<uint32_t> result;
            for (size_t i = 0; i < num_rows; ++i) {
                if (data[i] >= expr->lo && data[i] <= expr->hi) {
                    result.push_back(static_cast<uint32_t>(i));
                }
            }
            return result;
        }        case Expr::Kind::COMPARE: {
            if (expr->column == "symbol") {
                std::vector<uint32_t> all(num_rows);
                for (size_t i = 0; i < num_rows; ++i) all[i] = static_cast<uint32_t>(i);
                return all;
            }
            const ColumnVector* cv = part.get_column(expr->column);
            if (!cv) return {};
            std::vector<uint32_t> result;
            result.reserve(num_rows / 4);
            if (expr->is_float) {
                scan_compare<double>(cv->raw_data(), num_rows, expr->op, expr->value_f, result);
            } else {
                scan_compare<int64_t>(cv->raw_data(), num_rows, expr->op, expr->value, result);
            }
            return result;
        }
        case Expr::Kind::NOT: {
            // NOT expr: complement of the inner result set
            auto inner = eval_expr(expr->left, part, num_rows, default_alias);
            std::vector<uint32_t> all(num_rows);
            for (size_t i = 0; i < num_rows; ++i) all[i] = static_cast<uint32_t>(i);
            std::vector<uint32_t> result;
            result.reserve(num_rows - inner.size());
            std::set_difference(all.begin(), all.end(),
                                inner.begin(), inner.end(),
                                std::back_inserter(result));
            return result;
        }
        case Expr::Kind::IN: {
            const auto& vals = expr->in_values;
            const int64_t* data = get_col_data(part, expr->column);
            // symbol column: not stored as data, use partition key
            if (!data && expr->column == "symbol") {
                int64_t sym = static_cast<int64_t>(part.key().symbol_id);
                bool match = false;
                for (int64_t v : vals) { if (sym == v) { match = true; break; } }
                if (match == expr->negated) return {};  // NOT IN inverts
                // All rows in this partition match
                std::vector<uint32_t> result(num_rows);
                for (size_t i = 0; i < num_rows; ++i) result[i] = static_cast<uint32_t>(i);
                return result;
            }
            if (!data) return {};
            std::vector<uint32_t> result;
            result.reserve(num_rows / 4);
            for (size_t i = 0; i < num_rows; ++i) {
                bool found = false;
                for (int64_t v : vals) {
                    if (data[i] == v) { found = true; break; }
                }
                if (found != expr->negated) {
                    result.push_back(static_cast<uint32_t>(i));
                }
            }
            return result;
        }
        case Expr::Kind::IS_NULL: {
            // NULL is represented by INT64_MIN sentinel in ZeptoDB
            const int64_t* data = get_col_data(part, expr->column);
            std::vector<uint32_t> result;
            if (!data) {
                // Column absent → treat all rows as NULL
                if (!expr->negated) {
                    result.resize(num_rows);
                    for (size_t i = 0; i < num_rows; ++i)
                        result[i] = static_cast<uint32_t>(i);
                }
                return result;
            }
            result.reserve(num_rows / 8);
            for (size_t i = 0; i < num_rows; ++i) {
                bool is_null = (data[i] == INT64_MIN);
                if (expr->negated ? !is_null : is_null)
                    result.push_back(static_cast<uint32_t>(i));
            }
            return result;
        }
        case Expr::Kind::LIKE: {
            // Simple glob matching: '%' = any substring, '_' = any single char.
            // Column value is converted to its decimal string representation.
            const auto& pat = expr->like_pattern;
            auto like_match = [&](const std::string& s) -> bool {
                // dp[i][j]: s[0..i-1] matches pat[0..j-1]
                size_t m = s.size(), n = pat.size();
                std::vector<std::vector<bool>> dp(m+1, std::vector<bool>(n+1, false));
                dp[0][0] = true;
                for (size_t j = 1; j <= n; ++j)
                    dp[0][j] = dp[0][j-1] && pat[j-1] == '%';
                for (size_t i = 1; i <= m; ++i)
                    for (size_t j = 1; j <= n; ++j) {
                        if (pat[j-1] == '%')
                            dp[i][j] = dp[i-1][j] || dp[i][j-1];
                        else if (pat[j-1] == '_' || pat[j-1] == s[i-1])
                            dp[i][j] = dp[i-1][j-1];
                    }
                return dp[m][n];
            };
            // Get column data — symbol column is special
            std::vector<uint32_t> result;
            result.reserve(num_rows / 4);
            for (size_t i = 0; i < num_rows; ++i) {
                int64_t v;
                if (expr->column == "symbol") {
                    v = static_cast<int64_t>(part.key().symbol_id);
                } else {
                    const int64_t* data = get_col_data(part, expr->column);
                    v = data ? data[i] : 0;
                }
                std::string s = std::to_string(v);
                bool matched = like_match(s);
                if (expr->negated ? !matched : matched)
                    result.push_back(static_cast<uint32_t>(i));
            }
            return result;
        }
    }
    return {};
}

std::vector<uint32_t> QueryExecutor::eval_where(
    const SelectStmt& stmt,
    const Partition& part,
    size_t num_rows)
{
    if (!stmt.where.has_value()) {
        std::vector<uint32_t> all(num_rows);
        for (size_t i = 0; i < num_rows; ++i) all[i] = static_cast<uint32_t>(i);
        return all;
    }
    return eval_expr(stmt.where->expr, part, num_rows, stmt.from_alias);
}

// ============================================================================
// eval_where_ranged: [row_begin, row_end) 범위만 평가 (타임스탬프 이진탐색 후 사용)
// ============================================================================
std::vector<uint32_t> QueryExecutor::eval_where_ranged(
    const SelectStmt& stmt,
    const Partition& part,
    size_t row_begin,
    size_t row_end)
{
    if (row_begin >= row_end) return {};

    if (!stmt.where.has_value()) {
        // WHERE 없으면 범위 내 전체 행 반환
        std::vector<uint32_t> all;
        all.reserve(row_end - row_begin);
        for (size_t i = row_begin; i < row_end; ++i)
            all.push_back(static_cast<uint32_t>(i));
        return all;
    }

    // 전체 eval_expr를 수행하되, [row_begin, row_end) 범위로 결과 제한
    // (BETWEEN timestamp는 이미 범위로 넘어왔으므로, 해당 조건은 자동으로 통과)
    auto all_indices = eval_expr(stmt.where->expr, part, row_end, stmt.from_alias);

    // row_begin 미만 행 제거
    std::vector<uint32_t> result;
    result.reserve(all_indices.size());
    for (auto idx : all_indices) {
        if (idx >= static_cast<uint32_t>(row_begin)) result.push_back(idx);
    }
    return result;
}

// ============================================================================
// extract_time_range: WHERE 절에서 "timestamp BETWEEN lo AND hi" 추출
// ============================================================================
bool QueryExecutor::extract_time_range(
    const SelectStmt& stmt,
    int64_t& out_lo,
    int64_t& out_hi) const
{
    if (!stmt.where.has_value()) return false;

    // 재귀적으로 BETWEEN timestamp 조건 탐색
    std::function<bool(const std::shared_ptr<Expr>&)> find_ts =
        [&](const std::shared_ptr<Expr>& expr) -> bool {
        if (!expr) return false;
        if (expr->kind == Expr::Kind::BETWEEN &&
            (expr->column == "timestamp" || expr->column == "recv_ts")) {
            out_lo = expr->lo;
            out_hi = expr->hi;
            return true;
        }
        if (expr->kind == Expr::Kind::AND) {
            return find_ts(expr->left) || find_ts(expr->right);
        }
        return false;
    };
    return find_ts(stmt.where->expr);
}

// ============================================================================
// extract_sorted_col_range: WHERE conditions on an s#-sorted column
// ============================================================================
bool QueryExecutor::extract_sorted_col_range(
    const SelectStmt& stmt,
    const Partition& part,
    std::string& out_col,
    int64_t& out_lo,
    int64_t& out_hi) const
{
    if (!stmt.where.has_value()) return false;

    struct Bounds {
        int64_t lo  = INT64_MIN;
        int64_t hi  = INT64_MAX;
        bool    set = false;
    };
    std::unordered_map<std::string, Bounds> col_bounds;

    std::function<void(const std::shared_ptr<Expr>&)> collect =
        [&](const std::shared_ptr<Expr>& e) {
        if (!e) return;

        if (e->kind == Expr::Kind::BETWEEN && part.is_sorted(e->column)) {
            auto& b = col_bounds[e->column];
            b.lo  = std::max(b.lo, e->lo);
            b.hi  = std::min(b.hi, e->hi);
            b.set = true;
            return;
        }

        if (e->kind == Expr::Kind::COMPARE && !e->is_float &&
            part.is_sorted(e->column)) {
            auto& b = col_bounds[e->column];
            switch (e->op) {
                case CompareOp::GE: b.lo = std::max(b.lo, e->value); b.set = true; break;
                case CompareOp::GT: b.lo = std::max(b.lo, e->value + 1); b.set = true; break;
                case CompareOp::LE: b.hi = std::min(b.hi, e->value); b.set = true; break;
                case CompareOp::LT: b.hi = std::min(b.hi, e->value - 1); b.set = true; break;
                case CompareOp::EQ:
                    b.lo = b.hi = e->value;
                    b.set = true;
                    break;
                default: break;  // NE: not optimizable with a single range
            }
            return;
        }

        if (e->kind == Expr::Kind::AND) {
            collect(e->left);
            collect(e->right);
        }
        // OR / NOT: can't reduce to a single contiguous range — skip
    };
    collect(stmt.where->expr);

    for (auto& [col, b] : col_bounds) {
        if (b.set && (b.lo != INT64_MIN || b.hi != INT64_MAX)) {
            out_col = col;
            out_lo  = b.lo;
            out_hi  = b.hi;
            return true;
        }
    }
    return false;
}

// ============================================================================
// extract_index_eq: WHERE col = X on g# or p# indexed column
// ============================================================================
bool QueryExecutor::extract_index_eq(
    const SelectStmt& stmt,
    const Partition& part,
    std::string& out_col,
    int64_t& out_val) const
{
    if (!stmt.where.has_value()) return false;

    std::function<bool(const std::shared_ptr<Expr>&)> find_eq =
        [&](const std::shared_ptr<Expr>& e) -> bool {
        if (!e) return false;
        if (e->kind == Expr::Kind::COMPARE && e->op == CompareOp::EQ &&
            !e->is_float &&
            (part.is_grouped(e->column) || part.is_parted(e->column))) {
            out_col = e->column;
            out_val = e->value;
            return true;
        }
        if (e->kind == Expr::Kind::AND) {
            return find_eq(e->left) || find_eq(e->right);
        }
        return false;
    };
    return find_eq(stmt.where->expr);
}

// ============================================================================
// IndexResult: composite index intersection accumulator
// ============================================================================
void QueryExecutor::IndexResult::set_range(size_t b, size_t e) {
    range_begin = b;
    range_end   = e;
}

void QueryExecutor::IndexResult::intersect_range(size_t b, size_t e) {
    range_begin = std::max(range_begin, b);
    range_end   = std::min(range_end, e);
    if (has_set) {
        // Filter existing row_set to keep only indices within new range
        std::vector<uint32_t> filtered;
        filtered.reserve(row_set.size());
        for (auto idx : row_set) {
            if (idx >= static_cast<uint32_t>(range_begin) &&
                idx < static_cast<uint32_t>(range_end))
                filtered.push_back(idx);
        }
        row_set = std::move(filtered);
    }
}

void QueryExecutor::IndexResult::intersect_set(const std::vector<uint32_t>& s) {
    if (!has_set) {
        // First set: filter by current range
        row_set.reserve(s.size());
        for (auto idx : s) {
            if (idx >= static_cast<uint32_t>(range_begin) &&
                idx < static_cast<uint32_t>(range_end))
                row_set.push_back(idx);
        }
        has_set = true;
    } else {
        // Intersect two sets: put smaller into a set, scan larger
        const auto& smaller = (row_set.size() <= s.size()) ? row_set : s;
        const auto& larger  = (row_set.size() <= s.size()) ? s : row_set;
        std::unordered_set<uint32_t> lookup(smaller.begin(), smaller.end());
        std::vector<uint32_t> result;
        result.reserve(std::min(smaller.size(), larger.size()));
        for (auto idx : larger) {
            if (lookup.count(idx)) result.push_back(idx);
        }
        row_set = std::move(result);
    }
}

std::vector<uint32_t> QueryExecutor::IndexResult::materialize() const {
    if (has_set) return row_set;
    size_t b = range_begin;
    size_t e = (range_end == SIZE_MAX) ? b : range_end;
    if (b >= e) return {};
    std::vector<uint32_t> out(e - b);
    for (size_t i = b; i < e; ++i) out[i - b] = static_cast<uint32_t>(i);
    return out;
}

// ============================================================================
// extract_all_sorted_ranges: collect ALL s#-sorted range predicates
// ============================================================================
std::vector<QueryExecutor::SortedRangePred>
QueryExecutor::extract_all_sorted_ranges(
    const SelectStmt& stmt,
    const Partition& part) const
{
    if (!stmt.where.has_value()) return {};

    std::unordered_map<std::string, SortedRangePred> col_bounds;

    std::function<void(const std::shared_ptr<Expr>&)> collect =
        [&](const std::shared_ptr<Expr>& e) {
        if (!e) return;
        if (e->kind == Expr::Kind::BETWEEN && part.is_sorted(e->column)) {
            auto [it, inserted] = col_bounds.try_emplace(
                e->column, SortedRangePred{e->column, INT64_MIN, INT64_MAX});
            auto& b = it->second;
            b.lo = std::max(b.lo, e->lo);
            b.hi = std::min(b.hi, e->hi);
            return;
        }
        if (e->kind == Expr::Kind::COMPARE && !e->is_float &&
            part.is_sorted(e->column)) {
            auto [it, inserted] = col_bounds.try_emplace(
                e->column, SortedRangePred{e->column, INT64_MIN, INT64_MAX});
            auto& b = it->second;
            switch (e->op) {
                case CompareOp::GE: b.lo = std::max(b.lo, e->value); break;
                case CompareOp::GT: b.lo = std::max(b.lo, e->value + 1); break;
                case CompareOp::LE: b.hi = std::min(b.hi, e->value); break;
                case CompareOp::LT: b.hi = std::min(b.hi, e->value - 1); break;
                case CompareOp::EQ: b.lo = std::max(b.lo, e->value);
                                    b.hi = std::min(b.hi, e->value); break;
                default: break;
            }
            return;
        }
        if (e->kind == Expr::Kind::AND) {
            collect(e->left);
            collect(e->right);
        }
    };
    collect(stmt.where->expr);

    std::vector<SortedRangePred> result;
    for (auto& [col, pred] : col_bounds) {
        result.push_back(std::move(pred));
    }
    return result;
}

// ============================================================================
// extract_all_index_eqs: collect ALL g#/p# equality predicates
// ============================================================================
std::vector<QueryExecutor::IndexEqPred>
QueryExecutor::extract_all_index_eqs(
    const SelectStmt& stmt,
    const Partition& part) const
{
    if (!stmt.where.has_value()) return {};

    std::vector<IndexEqPred> result;

    std::function<void(const std::shared_ptr<Expr>&)> collect =
        [&](const std::shared_ptr<Expr>& e) {
        if (!e) return;
        if (e->kind == Expr::Kind::COMPARE && e->op == CompareOp::EQ &&
            !e->is_float &&
            (part.is_grouped(e->column) || part.is_parted(e->column))) {
            result.push_back({e->column, e->value});
            return;
        }
        if (e->kind == Expr::Kind::AND) {
            collect(e->left);
            collect(e->right);
        }
    };
    collect(stmt.where->expr);
    return result;
}

// ============================================================================
// eval_expr_single_row: evaluate a WHERE Expr for a single row index
// ============================================================================
bool QueryExecutor::eval_expr_single_row(
    const std::shared_ptr<Expr>& expr,
    const Partition& part,
    uint32_t row_idx) const
{
    if (!expr) return true;

    switch (expr->kind) {
        case Expr::Kind::AND:
            return eval_expr_single_row(expr->left, part, row_idx) &&
                   eval_expr_single_row(expr->right, part, row_idx);
        case Expr::Kind::OR:
            return eval_expr_single_row(expr->left, part, row_idx) ||
                   eval_expr_single_row(expr->right, part, row_idx);
        case Expr::Kind::NOT:
            return !eval_expr_single_row(expr->left, part, row_idx);
        case Expr::Kind::BETWEEN: {
            if (expr->column == "symbol") return true;
            const int64_t* data = get_col_data(part, expr->column);
            if (!data) return false;
            return data[row_idx] >= expr->lo && data[row_idx] <= expr->hi;
        }
        case Expr::Kind::COMPARE: {
            if (expr->column == "symbol") return true;
            const ColumnVector* cv = part.get_column(expr->column);
            if (!cv) return false;
            if (expr->is_float) {
                const double* d = static_cast<const double*>(cv->raw_data());
                return compare_val(d[row_idx], expr->op, expr->value_f);
            }
            const int64_t* d = static_cast<const int64_t*>(cv->raw_data());
            return compare_val(d[row_idx], expr->op, expr->value);
        }
        case Expr::Kind::IN: {
            int64_t v;
            if (expr->column == "symbol") {
                v = static_cast<int64_t>(part.key().symbol_id);
            } else {
                const int64_t* data = get_col_data(part, expr->column);
                if (!data) return false;
                v = data[row_idx];
            }
            bool found = false;
            for (int64_t iv : expr->in_values) {
                if (v == iv) { found = true; break; }
            }
            return found != expr->negated;
        }
        case Expr::Kind::IS_NULL: {
            const int64_t* data = get_col_data(part, expr->column);
            bool is_null = !data || (data[row_idx] == INT64_MIN);
            return expr->negated ? !is_null : is_null;
        }
        case Expr::Kind::LIKE: {
            int64_t v;
            if (expr->column == "symbol") {
                v = static_cast<int64_t>(part.key().symbol_id);
            } else {
                const int64_t* data = get_col_data(part, expr->column);
                v = data ? data[row_idx] : 0;
            }
            std::string s = std::to_string(v);
            const auto& pat = expr->like_pattern;
            // Simple dp glob match
            size_t m = s.size(), n = pat.size();
            std::vector<std::vector<bool>> dp(m+1, std::vector<bool>(n+1, false));
            dp[0][0] = true;
            for (size_t j = 1; j <= n; ++j)
                dp[0][j] = dp[0][j-1] && pat[j-1] == '%';
            for (size_t i = 1; i <= m; ++i)
                for (size_t j = 1; j <= n; ++j) {
                    if (pat[j-1] == '%')
                        dp[i][j] = dp[i-1][j] || dp[i][j-1];
                    else if (pat[j-1] == '_' || pat[j-1] == s[i-1])
                        dp[i][j] = dp[i-1][j-1];
                }
            bool matched = dp[m][n];
            return expr->negated ? !matched : matched;
        }
        default:
            return true;
    }
}

// ============================================================================
// eval_remaining_where: evaluate WHERE skipping already-indexed predicates
// ============================================================================
std::vector<uint32_t> QueryExecutor::eval_remaining_where(
    const SelectStmt& stmt,
    const Partition& part,
    const std::vector<uint32_t>& candidates,
    const std::unordered_set<std::string>& indexed_cols)
{
    if (!stmt.where.has_value() || candidates.empty())
        return candidates;

    // Check if a predicate node is fully covered by indexed columns
    std::function<bool(const std::shared_ptr<Expr>&)> all_indexed =
        [&](const std::shared_ptr<Expr>& e) -> bool {
        if (!e) return false;
        if (e->kind == Expr::Kind::COMPARE || e->kind == Expr::Kind::BETWEEN)
            return indexed_cols.count(e->column) > 0;
        if (e->kind == Expr::Kind::AND)
            return all_indexed(e->left) && all_indexed(e->right);
        return false;
    };

    // If entire WHERE is indexed, no remaining filtering needed
    if (all_indexed(stmt.where->expr)) return candidates;

    // Evaluate WHERE on candidate rows only (not full partition)
    std::vector<uint32_t> result;
    result.reserve(candidates.size());
    for (uint32_t idx : candidates) {
        if (eval_expr_single_row(stmt.where->expr, part, idx))
            result.push_back(idx);
    }
    return result;
}

// ============================================================================
// collect_and_intersect: composite index intersection for a single partition
// ============================================================================
std::vector<uint32_t> QueryExecutor::collect_and_intersect(
    const SelectStmt& stmt,
    const Partition& part,
    size_t& rows_scanned)
{
    size_t n = part.num_rows();
    IndexResult combined;
    std::unordered_set<std::string> indexed_cols;
    bool any_index_used = false;

    // Phase 1a: timestamp range
    int64_t ts_lo = INT64_MIN, ts_hi = INT64_MAX;
    if (extract_time_range(stmt, ts_lo, ts_hi)) {
        if (!part.overlaps_time_range(ts_lo, ts_hi)) return {};
        auto [rb, re] = part.timestamp_range(ts_lo, ts_hi);
        combined.set_range(rb, re);
        indexed_cols.insert("timestamp");
        indexed_cols.insert("recv_ts");
        any_index_used = true;
    }

    // Phase 1b: ALL s# sorted column ranges
    for (auto& pred : extract_all_sorted_ranges(stmt, part)) {
        auto [rb, re] = part.sorted_range(pred.col, pred.lo, pred.hi);
        combined.intersect_range(rb, re);
        indexed_cols.insert(pred.col);
        any_index_used = true;
    }

    // Phase 1c: ALL g#/p# equality predicates
    for (auto& pred : extract_all_index_eqs(stmt, part)) {
        if (part.is_grouped(pred.col)) {
            combined.intersect_set(part.grouped_lookup(pred.col, pred.val));
        } else {
            auto [rb, re] = part.parted_range(pred.col, pred.val);
            combined.intersect_range(rb, re);
        }
        indexed_cols.insert(pred.col);
        any_index_used = true;
    }

    // No index applicable → full scan fallback
    if (!any_index_used) {
        rows_scanned += n;
        return eval_where(stmt, part, n);
    }

    // Phase 2: materialize candidate rows
    auto candidates = combined.materialize();
    rows_scanned += candidates.size();

    // Phase 3: evaluate remaining non-indexed predicates
    return eval_remaining_where(stmt, part, candidates, indexed_cols);
}

// ============================================================================
// ============================================================================
// Static helpers: single-row evaluation
// ============================================================================

// Retrieve int64 column pointer from partition (file-scope helper)
static inline const int64_t* col_ptr(const Partition& part,
                                     const std::string& col_name)
{
    const ColumnVector* cv = part.get_column(col_name);
    return cv ? static_cast<const int64_t*>(cv->raw_data()) : nullptr;
}

// date_trunc_bucket: return nanosecond bucket size for a unit string
static int64_t date_trunc_bucket(const std::string& unit) {
    if (unit == "ns")   return 1LL;
    if (unit == "us")   return 1'000LL;
    if (unit == "ms")   return 1'000'000LL;
    if (unit == "s")    return 1'000'000'000LL;
    if (unit == "min")  return 60'000'000'000LL;
    if (unit == "hour") return 3'600'000'000'000LL;
    if (unit == "day")  return 86'400'000'000'000LL;
    if (unit == "week") return 604'800'000'000'000LL;
    return 1LL; // unknown unit: no truncation
}

// eval_arith: evaluate an ArithExpr for a single row
static int64_t eval_arith(const ArithExpr& node,
                          const Partition& part, uint32_t idx)
{
    switch (node.kind) {
        case ArithExpr::Kind::LITERAL:
            return node.literal;
        case ArithExpr::Kind::COLUMN: {
            if (node.column == "symbol")
                return static_cast<int64_t>(part.key().symbol_id);
            const int64_t* d = col_ptr(part, node.column);
            return d ? d[idx] : 0;
        }
        case ArithExpr::Kind::BINARY: {
            int64_t lv = eval_arith(*node.left,  part, idx);
            int64_t rv = eval_arith(*node.right, part, idx);
            switch (node.arith_op) {
                case ArithOp::ADD: return lv + rv;
                case ArithOp::SUB: return lv - rv;
                case ArithOp::MUL: return lv * rv;
                case ArithOp::DIV: return rv != 0 ? lv / rv : 0;
            }
        }
        case ArithExpr::Kind::FUNC: {
            if (node.func_name == "now") {
                return std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            }
            if (node.func_name == "interval") {
                // Parse "N unit" from func_unit, return nanoseconds
                // Supported: seconds/minutes/hours/days/weeks, with or without 's'
                auto& lit = node.func_unit;
                // Split on first space or find where digits end
                size_t i = 0;
                while (i < lit.size() && (std::isdigit(static_cast<unsigned char>(lit[i])) || lit[i] == ' ' || lit[i] == '.'))
                    ++i;
                // If no space found, try splitting at digit/alpha boundary
                size_t split = lit.find(' ');
                int64_t n = 1;
                std::string unit_str;
                if (split != std::string::npos) {
                    n = std::stoll(lit.substr(0, split));
                    unit_str = lit.substr(split + 1);
                } else {
                    // e.g. "5minutes" — find where digits end
                    size_t d = 0;
                    while (d < lit.size() && std::isdigit(static_cast<unsigned char>(lit[d]))) ++d;
                    if (d > 0 && d < lit.size()) {
                        n = std::stoll(lit.substr(0, d));
                        unit_str = lit.substr(d);
                    } else {
                        unit_str = lit;
                    }
                }
                // Normalize: lowercase, strip trailing 's'
                for (auto& c : unit_str) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                while (!unit_str.empty() && unit_str.back() == ' ') unit_str.pop_back();
                if (!unit_str.empty() && unit_str.back() == 's' && unit_str != "ns" && unit_str != "ms")
                    unit_str.pop_back();
                int64_t ns_per = 1;
                if (unit_str == "n" || unit_str == "ns" || unit_str == "nanosecond")  ns_per = 1LL;
                else if (unit_str == "u" || unit_str == "us" || unit_str == "microsecond") ns_per = 1'000LL;
                else if (unit_str == "ms" || unit_str == "millisecond") ns_per = 1'000'000LL;
                else if (unit_str == "second" || unit_str == "sec" || unit_str == "s") ns_per = 1'000'000'000LL;
                else if (unit_str == "minute" || unit_str == "min" || unit_str == "m") ns_per = 60'000'000'000LL;
                else if (unit_str == "hour" || unit_str == "h")   ns_per = 3'600'000'000'000LL;
                else if (unit_str == "day" || unit_str == "d")    ns_per = 86'400'000'000'000LL;
                else if (unit_str == "week" || unit_str == "w")   ns_per = 604'800'000'000'000LL;
                return n * ns_per;
            }
            int64_t arg = node.func_arg ? eval_arith(*node.func_arg, part, idx) : 0;
            if (node.func_name == "date_trunc") {
                int64_t bucket = date_trunc_bucket(node.func_unit);
                return (arg / bucket) * bucket;
            }
            if (node.func_name == "epoch_s")  return arg / 1'000'000'000LL;
            if (node.func_name == "epoch_ms") return arg / 1'000'000LL;
            if (node.func_name == "substr") {
                // SUBSTR on int64 column: convert to string, extract substring, convert back
                std::string s = std::to_string(arg);
                int64_t start = node.func_unit.empty() ? 1 : std::stoll(node.func_unit);
                if (start < 1) start = 1;
                size_t pos = static_cast<size_t>(start - 1); // 1-based → 0-based
                if (pos >= s.size()) return 0;
                int64_t len = node.func_arg2
                    ? eval_arith(*node.func_arg2, part, idx)
                    : static_cast<int64_t>(s.size() - pos);
                std::string sub = s.substr(pos, static_cast<size_t>(len));
                try { return std::stoll(sub); } catch (...) { return 0; }
            }
            return arg;
        }
    }
    return 0;
}

// eval_arith_const: evaluate a constant ArithExpr (no partition context).
// Used to pre-resolve WHERE value_expr like NOW() - INTERVAL '5 minutes'.
static int64_t eval_arith_const(const ArithExpr& node) {
    switch (node.kind) {
        case ArithExpr::Kind::LITERAL: return node.literal;
        case ArithExpr::Kind::COLUMN:  return 0; // columns can't be resolved without data
        case ArithExpr::Kind::BINARY: {
            int64_t lv = eval_arith_const(*node.left);
            int64_t rv = eval_arith_const(*node.right);
            switch (node.arith_op) {
                case ArithOp::ADD: return lv + rv;
                case ArithOp::SUB: return lv - rv;
                case ArithOp::MUL: return lv * rv;
                case ArithOp::DIV: return rv != 0 ? lv / rv : 0;
            }
        }
        case ArithExpr::Kind::FUNC: {
            if (node.func_name == "now")
                return std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            if (node.func_name == "interval") {
                // Reuse the same logic as eval_arith INTERVAL case
                auto& lit = node.func_unit;
                size_t split = lit.find(' ');
                int64_t n = 1;
                std::string unit_str;
                if (split != std::string::npos) {
                    n = std::stoll(lit.substr(0, split));
                    unit_str = lit.substr(split + 1);
                } else {
                    size_t d = 0;
                    while (d < lit.size() && std::isdigit(static_cast<unsigned char>(lit[d]))) ++d;
                    if (d > 0 && d < lit.size()) { n = std::stoll(lit.substr(0, d)); unit_str = lit.substr(d); }
                    else unit_str = lit;
                }
                for (auto& c : unit_str) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                while (!unit_str.empty() && unit_str.back() == ' ') unit_str.pop_back();
                if (!unit_str.empty() && unit_str.back() == 's' && unit_str != "ns" && unit_str != "ms")
                    unit_str.pop_back();
                int64_t ns_per = 1;
                if (unit_str == "n" || unit_str == "ns" || unit_str == "nanosecond")  ns_per = 1LL;
                else if (unit_str == "u" || unit_str == "us" || unit_str == "microsecond") ns_per = 1'000LL;
                else if (unit_str == "ms" || unit_str == "millisecond") ns_per = 1'000'000LL;
                else if (unit_str == "second" || unit_str == "sec" || unit_str == "s") ns_per = 1'000'000'000LL;
                else if (unit_str == "minute" || unit_str == "min" || unit_str == "m") ns_per = 60'000'000'000LL;
                else if (unit_str == "hour" || unit_str == "h")   ns_per = 3'600'000'000'000LL;
                else if (unit_str == "day" || unit_str == "d")    ns_per = 86'400'000'000'000LL;
                else if (unit_str == "week" || unit_str == "w")   ns_per = 604'800'000'000'000LL;
                return n * ns_per;
            }
            int64_t arg = node.func_arg ? eval_arith_const(*node.func_arg) : 0;
            if (node.func_name == "date_trunc") {
                int64_t bucket = date_trunc_bucket(node.func_unit);
                return (arg / bucket) * bucket;
            }
            if (node.func_name == "epoch_s")  return arg / 1'000'000'000LL;
            if (node.func_name == "epoch_ms") return arg / 1'000'000LL;
            return arg;
        }
    }
    return 0;
}

// resolve_value_exprs: walk Expr tree, evaluate value_expr → value
static void resolve_value_exprs(std::shared_ptr<Expr>& expr) {
    if (!expr) return;
    if (expr->kind == Expr::Kind::AND || expr->kind == Expr::Kind::OR) {
        resolve_value_exprs(expr->left);
        resolve_value_exprs(expr->right);
        return;
    }
    if (expr->kind == Expr::Kind::NOT) {
        resolve_value_exprs(expr->left);
        return;
    }
    if (expr->value_expr) {
        expr->value = eval_arith_const(*expr->value_expr);
        expr->value_expr.reset();
    }
}

// ── Resolve uncorrelated scalar/IN subqueries into literals ────────────────
void QueryExecutor::resolve_subqueries(std::shared_ptr<Expr>& expr) {
    if (!expr) return;
    if (expr->kind == Expr::Kind::AND || expr->kind == Expr::Kind::OR) {
        resolve_subqueries(expr->left);
        resolve_subqueries(expr->right);
        return;
    }
    if (expr->kind == Expr::Kind::NOT) {
        resolve_subqueries(expr->left);
        return;
    }
    if (expr->kind == Expr::Kind::SCALAR_SUBQUERY && expr->subquery) {
        auto sub = exec_select(*expr->subquery);
        if (!sub.ok())
            throw std::runtime_error("Scalar subquery error: " + sub.error);
        if (sub.rows.size() != 1 || sub.column_names.size() != 1)
            throw std::runtime_error(
                "Scalar subquery must return exactly 1 row × 1 column, got "
                + std::to_string(sub.rows.size()) + " rows × "
                + std::to_string(sub.column_names.size()) + " columns");
        // Replace with a plain COMPARE node
        expr->kind = Expr::Kind::COMPARE;
        if (!sub.typed_rows.empty()) {
            auto& tv = sub.typed_rows[0][0];
            // Check if the column type is float
            if (!sub.column_types.empty() &&
                (sub.column_types[0] == storage::ColumnType::FLOAT64 ||
                 sub.column_types[0] == storage::ColumnType::FLOAT32)) {
                expr->value_f = tv.f;
                expr->is_float = true;
                expr->value = static_cast<int64_t>(tv.f);
            } else {
                expr->value = tv.i;
            }
        } else {
            expr->value = sub.rows[0][0];
        }
        expr->subquery.reset();
    }
    if (expr->kind == Expr::Kind::IN_SUBQUERY && expr->subquery) {
        auto sub = exec_select(*expr->subquery);
        if (!sub.ok())
            throw std::runtime_error("IN subquery error: " + sub.error);
        if (sub.column_names.size() != 1)
            throw std::runtime_error(
                "IN subquery must return exactly 1 column, got "
                + std::to_string(sub.column_names.size()));
        // Replace with a plain IN node (deduplicated)
        expr->kind = Expr::Kind::IN;
        expr->in_values.clear();
        std::set<int64_t> seen;
        for (auto& row : sub.rows)
            if (seen.insert(row[0]).second)
                expr->in_values.push_back(row[0]);
        expr->subquery.reset();
    }
}

// eval_expr_single: evaluate an Expr condition for one row (used by CASE WHEN)
static bool eval_expr_single(const std::shared_ptr<Expr>& expr,
                              const Partition& part, uint32_t idx)
{
    if (!expr) return true;
    switch (expr->kind) {
        case Expr::Kind::AND:
            return eval_expr_single(expr->left, part, idx)
                && eval_expr_single(expr->right, part, idx);
        case Expr::Kind::OR:
            return eval_expr_single(expr->left, part, idx)
                || eval_expr_single(expr->right, part, idx);
        case Expr::Kind::NOT:
            return !eval_expr_single(expr->left, part, idx);
        case Expr::Kind::COMPARE: {
            const auto* cv = part.get_column(expr->column);
            if (!cv) return false;
            if (expr->is_float) {
                const double* d = static_cast<const double*>(cv->raw_data());
                return compare_val(d[idx], expr->op, expr->value_f);
            }
            const int64_t* d = static_cast<const int64_t*>(cv->raw_data());
            return compare_val(d[idx], expr->op, expr->value);
        }
        case Expr::Kind::BETWEEN: {
            const int64_t* d = col_ptr(part, expr->column);
            return d && d[idx] >= expr->lo && d[idx] <= expr->hi;
        }
        case Expr::Kind::IN: {
            const int64_t* d = col_ptr(part, expr->column);
            if (!d) return false;
            for (int64_t v : expr->in_values)
                if (d[idx] == v) return true;
            return false;
        }
        case Expr::Kind::IS_NULL: {
            const int64_t* d = col_ptr(part, expr->column);
            bool is_null = (!d || d[idx] == INT64_MIN);
            return expr->negated ? !is_null : is_null;
        }
        case Expr::Kind::LIKE: {
            int64_t v = (expr->column == "symbol")
                ? static_cast<int64_t>(part.key().symbol_id)
                : (col_ptr(part, expr->column) ? col_ptr(part, expr->column)[idx] : 0);
            const std::string& pat = expr->like_pattern;
            std::string s = std::to_string(v);
            size_t m = s.size(), n = pat.size();
            std::vector<std::vector<bool>> dp(m+1, std::vector<bool>(n+1, false));
            dp[0][0] = true;
            for (size_t j = 1; j <= n; ++j)
                dp[0][j] = dp[0][j-1] && pat[j-1] == '%';
            for (size_t i = 1; i <= m; ++i)
                for (size_t j = 1; j <= n; ++j) {
                    if (pat[j-1] == '%')         dp[i][j] = dp[i-1][j] || dp[i][j-1];
                    else if (pat[j-1] == '_' || pat[j-1] == s[i-1]) dp[i][j] = dp[i-1][j-1];
                }
            bool matched = dp[m][n];
            return expr->negated ? !matched : matched;
        }
    }
    return true;
}

// compute_stats: STDDEV/VARIANCE/MEDIAN/PERCENTILE from collected values
static int64_t compute_stats(AggFunc func, std::vector<int64_t> vals, int64_t pct) {
    if (vals.empty()) return INT64_MIN;
    size_t n = vals.size();
    if (func == AggFunc::STDDEV || func == AggFunc::VARIANCE) {
        double sum = 0;
        for (int64_t v : vals) sum += static_cast<double>(v);
        double mean = sum / static_cast<double>(n);
        double sq_sum = 0;
        for (int64_t v : vals) {
            double d = static_cast<double>(v) - mean;
            sq_sum += d * d;
        }
        double var = sq_sum / static_cast<double>(n);
        return static_cast<int64_t>(func == AggFunc::VARIANCE ? var : std::sqrt(var));
    }
    std::sort(vals.begin(), vals.end());
    if (func == AggFunc::MEDIAN) pct = 50;
    size_t idx = static_cast<size_t>(pct) * (n - 1) / 100;
    return vals[idx];
}

// eval_case_when: evaluate CASE WHEN expression for one row
static int64_t eval_case_when(const CaseWhenExpr& cwe,
                              const Partition& part, uint32_t idx)
{
    for (const auto& branch : cwe.branches) {
        if (eval_expr_single(branch.when_cond, part, idx)) {
            return branch.then_val ? eval_arith(*branch.then_val, part, idx) : 0;
        }
    }
    return cwe.else_val ? eval_arith(*cwe.else_val, part, idx) : 0;
}

// ============================================================================
// exec_select_virtual: execute a SELECT against an in-memory result set
// (CTE body or FROM-subquery).  Handles WHERE, aggregation, GROUP BY,
// HAVING, ORDER BY, LIMIT, DISTINCT, SELECT *.
// ============================================================================
QueryResultSet QueryExecutor::exec_select_virtual(
    const SelectStmt& stmt,
    const QueryResultSet& src,
    const std::string& src_alias)
{
    // ── Build column-name → index map for the virtual source ──────────────
    std::unordered_map<std::string, size_t> col_idx;
    col_idx.reserve(src.column_names.size());
    for (size_t i = 0; i < src.column_names.size(); ++i)
        col_idx[src.column_names[i]] = i;

    // ── Apply WHERE filter ────────────────────────────────────────────────
    std::vector<uint32_t> passing;
    passing.reserve(src.rows.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(src.rows.size()); ++i) {
        if (!stmt.where || eval_expr_vt(stmt.where->expr, src, col_idx, i))
            passing.push_back(i);
    }

    // ── Detect aggregation ────────────────────────────────────────────────
    bool has_agg = false;
    for (const auto& sel : stmt.columns)
        if (sel.agg != AggFunc::NONE && sel.agg != AggFunc::XBAR) {
            has_agg = true; break;
        }

    // ── CASE 1: Simple projection (no aggregation) ────────────────────────
    if (!has_agg || !stmt.group_by.has_value()) {
        if (!has_agg) {
            QueryResultSet result;
            bool is_star = !stmt.columns.empty() && stmt.columns[0].is_star;
            if (is_star) {
                result.column_names = src.column_names;
                result.column_types = src.column_types;
            } else {
                for (const auto& sel : stmt.columns) {
                    result.column_names.push_back(sel.alias.empty() ? sel.column : sel.alias);
                    result.column_types.push_back(ColumnType::INT64);
                }
            }

            for (uint32_t ri : passing) {
                if (is_star) {
                    result.rows.push_back(src.rows[ri]);
                } else {
                    std::vector<int64_t> row;
                    row.reserve(stmt.columns.size());
                    for (const auto& sel : stmt.columns)
                        row.push_back(sel_val_vt(sel, src, col_idx, ri));
                    result.rows.push_back(std::move(row));
                }
            }

            if (stmt.distinct) {
                std::set<std::vector<int64_t>> seen;
                std::vector<std::vector<int64_t>> deduped;
                for (auto& row : result.rows)
                    if (seen.insert(row).second) deduped.push_back(row);
                result.rows = std::move(deduped);
            }

            result.rows_scanned = src.rows.size();
            apply_order_by(result, stmt);
            if (!stmt.order_by.has_value())
                apply_offset_limit(result, stmt);
            return result;
        }

        // ── CASE 2: Scalar aggregation (no GROUP BY) ──────────────────────
        QueryResultSet result;
        std::vector<int64_t> row;
        row.reserve(stmt.columns.size());

        for (const auto& sel : stmt.columns) {
            result.column_names.push_back(sel.alias.empty() ? sel.column : sel.alias);
            result.column_types.push_back(ColumnType::INT64);

            int64_t cnt   = 0;
            double  sum   = 0.0;
            int64_t mn    = INT64_MAX, mx = INT64_MIN;
            int64_t first = INT64_MIN, last = INT64_MIN;
            std::set<int64_t> distinct_set;

            for (uint32_t ri : passing) {
                int64_t v = sel_val_vt(sel, src, col_idx, ri);
                switch (sel.agg) {
                    case AggFunc::COUNT:
                        if (sel.agg_distinct) distinct_set.insert(v);
                        else cnt++;
                        break;
                    case AggFunc::SUM:   sum += v; break;
                    case AggFunc::AVG:   sum += v; cnt++; break;
                    case AggFunc::MIN:   if (v < mn) mn = v; break;
                    case AggFunc::MAX:   if (v > mx) mx = v; break;
                    case AggFunc::FIRST: if (first == INT64_MIN) first = v; break;
                    case AggFunc::LAST:  last = v; break;
                    default: break;
                }
            }
            int64_t agg_val = 0;
            switch (sel.agg) {
                case AggFunc::COUNT: agg_val = sel.agg_distinct
                    ? static_cast<int64_t>(distinct_set.size()) : cnt; break;
                case AggFunc::SUM:   agg_val = static_cast<int64_t>(sum); break;
                case AggFunc::AVG:   agg_val = cnt ? static_cast<int64_t>(sum / cnt) : INT64_MIN; break;
                case AggFunc::MIN:   agg_val = (mn != INT64_MAX) ? mn : INT64_MIN; break;
                case AggFunc::MAX:   agg_val = mx; break;
                case AggFunc::FIRST: agg_val = (first != INT64_MIN) ? first : 0; break;
                case AggFunc::LAST:  agg_val = (last  != INT64_MIN) ? last  : 0; break;
                default:
                    if (!passing.empty()) agg_val = sel_val_vt(sel, src, col_idx, passing[0]);
                    break;
            }
            row.push_back(agg_val);
        }
        result.rows.push_back(std::move(row));
        result.rows_scanned = src.rows.size();
        return result;
    }

    // ── CASE 3: GROUP BY aggregation ─────────────────────────────────────
    const auto& gb = *stmt.group_by;

    struct AggState {
        std::vector<double>  sums;
        std::vector<int64_t> counts;
        std::vector<int64_t> mins;
        std::vector<int64_t> maxs;
        std::vector<int64_t> firsts; // INT64_MIN = not yet set
        std::vector<int64_t> lasts;
        std::vector<int64_t> first_non_agg;
        std::vector<std::vector<int64_t>> vals; // STDDEV/VARIANCE/MEDIAN/PERCENTILE
    };

    std::unordered_map<std::vector<int64_t>, AggState, VectorHash> groups;
    size_t ncols = stmt.columns.size();

    for (uint32_t ri : passing) {
        // Build composite group key
        std::vector<int64_t> key;
        key.reserve(gb.columns.size());
        for (const auto& gc : gb.columns)
            key.push_back(vt_col_val(gc, src, col_idx, ri));

        auto& state = groups[key];
        if (state.sums.empty()) {
            state.sums.assign(ncols, 0.0);
            state.counts.assign(ncols, 0);
            state.mins.assign(ncols, INT64_MAX);
            state.maxs.assign(ncols, INT64_MIN);
            state.firsts.assign(ncols, INT64_MIN);
            state.lasts.assign(ncols, INT64_MIN);
            state.first_non_agg.assign(ncols, 0);
            state.vals.resize(ncols);
        }

        for (size_t ci = 0; ci < ncols; ++ci) {
            const auto& sel = stmt.columns[ci];
            int64_t v = sel_val_vt(sel, src, col_idx, ri);
            switch (sel.agg) {
                case AggFunc::COUNT: state.counts[ci]++; break;
                case AggFunc::SUM:   state.sums[ci] += v; break;
                case AggFunc::AVG:   state.sums[ci] += v; state.counts[ci]++; break;
                case AggFunc::MIN:   if (v < state.mins[ci]) state.mins[ci] = v; break;
                case AggFunc::MAX:   if (v > state.maxs[ci]) state.maxs[ci] = v; break;
                case AggFunc::FIRST:
                    if (state.firsts[ci] == INT64_MIN) state.firsts[ci] = v;
                    break;
                case AggFunc::LAST:  state.lasts[ci] = v; break;
                case AggFunc::STDDEV: case AggFunc::VARIANCE:
                case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                    state.vals[ci].push_back(v); break;
                default:
                    if (state.counts[ci] == 0) state.first_non_agg[ci] = v;
                    state.counts[ci]++;
                    break;
            }
        }
    }

    QueryResultSet result;
    for (const auto& sel : stmt.columns) {
        result.column_names.push_back(sel.alias.empty() ? sel.column : sel.alias);
        result.column_types.push_back(ColumnType::INT64);
    }

    for (auto& [key, state] : groups) {
        std::vector<int64_t> row;
        row.reserve(ncols);
        for (size_t ci = 0; ci < ncols; ++ci) {
            const auto& sel = stmt.columns[ci];
            int64_t v = 0;
            switch (sel.agg) {
                case AggFunc::COUNT: v = state.counts[ci]; break;
                case AggFunc::SUM:   v = static_cast<int64_t>(state.sums[ci]); break;
                case AggFunc::AVG:   v = state.counts[ci] ? static_cast<int64_t>(state.sums[ci] / state.counts[ci]) : 0; break;
                case AggFunc::MIN:   v = (state.mins[ci] != INT64_MAX) ? state.mins[ci] : INT64_MIN; break;
                case AggFunc::MAX:   v = state.maxs[ci]; break;
                case AggFunc::FIRST: v = (state.firsts[ci] != INT64_MIN) ? state.firsts[ci] : 0; break;
                case AggFunc::LAST:  v = (state.lasts[ci]  != INT64_MIN) ? state.lasts[ci]  : 0; break;
                case AggFunc::STDDEV: case AggFunc::VARIANCE:
                case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                    v = compute_stats(sel.agg, state.vals[ci], sel.percentile_value); break;
                default: {
                    // Non-aggregate column: find its value from the group key or first row
                    bool found = false;
                    for (size_t ki = 0; ki < gb.columns.size(); ++ki) {
                        if (gb.columns[ki] == sel.column) {
                            v = key[ki]; found = true; break;
                        }
                    }
                    if (!found) v = state.first_non_agg[ci];
                    break;
                }
            }
            row.push_back(v);
        }
        result.rows.push_back(std::move(row));
    }

    result.rows_scanned = src.rows.size();

    if (stmt.having)
        result = apply_having_filter(std::move(result), *stmt.having);

    apply_order_by(result, stmt);
    if (!stmt.order_by.has_value())
        apply_offset_limit(result, stmt);
    return result;
}

// apply_having_filter: 집계 결과 행을 HAVING 조건으로 필터링
// ============================================================================
QueryResultSet QueryExecutor::apply_having_filter(
    QueryResultSet result,
    const WhereClause& having) const
{
    if (result.rows.empty() || !having.expr) return result;

    const auto& col_names = result.column_names;

    // Evaluate one HAVING condition against a result row
    std::function<bool(const std::shared_ptr<Expr>&,
                       const std::vector<int64_t>&, size_t)> eval_row;
    eval_row = [&](const std::shared_ptr<Expr>& expr,
                   const std::vector<int64_t>& row, size_t ri) -> bool {
        if (!expr) return true;
        switch (expr->kind) {
            case Expr::Kind::AND:
                return eval_row(expr->left, row, ri) && eval_row(expr->right, row, ri);
            case Expr::Kind::OR:
                return eval_row(expr->left, row, ri) || eval_row(expr->right, row, ri);
            case Expr::Kind::NOT:
                return !eval_row(expr->left, row, ri);
            case Expr::Kind::COMPARE: {
                // Match by column alias or name
                int col_idx = -1;
                for (size_t i = 0; i < col_names.size(); ++i) {
                    if (col_names[i] == expr->column) {
                        col_idx = static_cast<int>(i);
                        break;
                    }
                }
                if (col_idx < 0 || col_idx >= static_cast<int>(row.size()))
                    return false;
                int64_t v   = row[col_idx];
                if (expr->is_float) {
                    double fv = (ri < result.typed_rows.size())
                        ? result.typed_rows[ri][col_idx].f
                        : static_cast<double>(v);
                    return compare_val(fv, expr->op, expr->value_f);
                }
                return compare_val(v, expr->op, expr->value);
            }
            case Expr::Kind::BETWEEN: {
                int col_idx = -1;
                for (size_t i = 0; i < col_names.size(); ++i) {
                    if (col_names[i] == expr->column) { col_idx = static_cast<int>(i); break; }
                }
                if (col_idx < 0 || col_idx >= static_cast<int>(row.size()))
                    return false;
                int64_t v = row[col_idx];
                return v >= expr->lo && v <= expr->hi;
            }
            case Expr::Kind::IN: {
                int col_idx = -1;
                for (size_t i = 0; i < col_names.size(); ++i) {
                    if (col_names[i] == expr->column) { col_idx = static_cast<int>(i); break; }
                }
                if (col_idx < 0) return false;
                int64_t v = row[col_idx];
                for (int64_t iv : expr->in_values)
                    if (v == iv) return true;
                return false;
            }
            case Expr::Kind::IS_NULL: {
                int col_idx = -1;
                for (size_t i = 0; i < col_names.size(); ++i) {
                    if (col_names[i] == expr->column) { col_idx = static_cast<int>(i); break; }
                }
                if (col_idx < 0) return !expr->negated; // unknown column → treat as NULL
                bool is_null = (row[col_idx] == INT64_MIN);
                return expr->negated ? !is_null : is_null;
            }
            case Expr::Kind::LIKE: {
                // LIKE in HAVING: match column value's string repr against pattern
                int col_idx = -1;
                for (size_t i = 0; i < col_names.size(); ++i) {
                    if (col_names[i] == expr->column) { col_idx = static_cast<int>(i); break; }
                }
                if (col_idx < 0) return expr->negated;
                std::string s = std::to_string(row[col_idx]);
                const std::string& pat = expr->like_pattern;
                size_t m = s.size(), n = pat.size();
                std::vector<std::vector<bool>> dp(m+1, std::vector<bool>(n+1, false));
                dp[0][0] = true;
                for (size_t j = 1; j <= n; ++j)
                    dp[0][j] = dp[0][j-1] && pat[j-1] == '%';
                for (size_t i = 1; i <= m; ++i)
                    for (size_t j = 1; j <= n; ++j) {
                        if (pat[j-1] == '%')         dp[i][j] = dp[i-1][j] || dp[i][j-1];
                        else if (pat[j-1] == '_' || pat[j-1] == s[i-1]) dp[i][j] = dp[i-1][j-1];
                    }
                bool matched = dp[m][n];
                return expr->negated ? !matched : matched;
            }
        }
        return true;
    };

    QueryResultSet filtered;
    filtered.column_names    = result.column_names;
    filtered.column_types    = result.column_types;
    filtered.execution_time_us = result.execution_time_us;
    filtered.rows_scanned    = result.rows_scanned;
    filtered.rows.reserve(result.rows.size());

    for (size_t ri = 0; ri < result.rows.size(); ++ri) {
        if (eval_row(having.expr, result.rows[ri], ri)) {
            filtered.rows.push_back(std::move(result.rows[ri]));
            if (ri < result.typed_rows.size())
                filtered.typed_rows.push_back(std::move(result.typed_rows[ri]));
        }
    }
    return filtered;
}

// ============================================================================
// apply_order_by: ORDER BY col [ASC|DESC] LIMIT N — top-N partial sort
// ============================================================================
void QueryExecutor::apply_order_by(QueryResultSet& result, const SelectStmt& stmt)
{
    if (!stmt.order_by.has_value() || result.rows.empty()) return;

    const auto& order_items = stmt.order_by->items;
    if (order_items.empty()) return;

    // ORDER BY 컬럼 인덱스 찾기 (alias 우선, 없으면 column명으로 검색)
    std::vector<std::pair<int, bool>> sort_keys; // (col_index, asc)
    for (const auto& item : order_items) {
        int idx = -1;
        // alias 우선 검색
        for (size_t ci = 0; ci < result.column_names.size(); ++ci) {
            if (result.column_names[ci] == item.column) {
                idx = static_cast<int>(ci);
                break;
            }
        }
        if (idx >= 0) sort_keys.push_back({idx, item.asc});
    }

    if (sort_keys.empty()) return;

    size_t offset = stmt.offset.value_or(0);
    size_t limit = stmt.limit.value_or(result.rows.size());
    size_t need = std::min(offset + limit, result.rows.size());

    if (need < result.rows.size()) {
        // top-N partial sort: std::partial_sort (O(n log k))
        std::partial_sort(
            result.rows.begin(),
            result.rows.begin() + static_cast<ptrdiff_t>(need),
            result.rows.end(),
            [&](const std::vector<int64_t>& a, const std::vector<int64_t>& b) {
                for (auto [ci, asc] : sort_keys) {
                    int64_t va = (ci < (int)a.size()) ? a[ci] : 0;
                    int64_t vb = (ci < (int)b.size()) ? b[ci] : 0;
                    if (va != vb) return asc ? va < vb : va > vb;
                }
                return false;
            }
        );
        result.rows.resize(need);
    } else {
        // 전체 정렬 (std::sort)
        std::sort(
            result.rows.begin(),
            result.rows.end(),
            [&](const std::vector<int64_t>& a, const std::vector<int64_t>& b) {
                for (auto [ci, asc] : sort_keys) {
                    int64_t va = (ci < (int)a.size()) ? a[ci] : 0;
                    int64_t vb = (ci < (int)b.size()) ? b[ci] : 0;
                    if (va != vb) return asc ? va < vb : va > vb;
                }
                return false;
            }
        );
    }

    // Apply OFFSET after sort
    if (stmt.offset && *stmt.offset > 0) {
        size_t off = static_cast<size_t>(*stmt.offset);
        if (off < result.rows.size())
            result.rows.erase(result.rows.begin(), result.rows.begin() + static_cast<ptrdiff_t>(off));
        else
            result.rows.clear();
        if (stmt.limit && result.rows.size() > static_cast<size_t>(*stmt.limit))
            result.rows.resize(static_cast<size_t>(*stmt.limit));
    }
}


QueryResultSet QueryExecutor::exec_simple_select(
    const SelectStmt& stmt,
    const std::vector<Partition*>& partitions)
{
    // Parallel path
    if (par_opts_.enabled && pool_raw_ &&
        pool_raw_->num_threads() > 1 &&
        estimate_total_rows(partitions) >= par_opts_.row_threshold &&
        partitions.size() >= 2)
    {
        try {
            return exec_simple_select_parallel(stmt, partitions);
        } catch (...) {}
    }

    QueryResultSet result;
    size_t rows_scanned = 0;

    bool is_star = stmt.columns.size() == 1 && stmt.columns[0].is_star;

    if (!partitions.empty()) {
        auto* part = partitions[0];
        if (is_star) {
            for (const auto& cv : part->columns()) {
                result.column_names.push_back(cv->name());
                result.column_types.push_back(cv->type());
            }
        } else {
            for (const auto& sel : stmt.columns) {
                if (sel.window_func != WindowFunc::NONE) continue;
                result.column_names.push_back(
                    sel.alias.empty() ? sel.column : sel.alias);
                // Propagate actual column type (float64, etc.)
                auto* cv = part->get_column(sel.column);
                result.column_types.push_back(
                    cv ? cv->type() : ColumnType::INT64);
            }
        }
    }

    for (auto* part : partitions) {
        if (is_cancelled()) { QueryResultSet r; r.error = "Query cancelled"; return r; }

        std::vector<uint32_t> sel_indices = collect_and_intersect(stmt, *part, rows_scanned);
        if (sel_indices.empty() && part->num_rows() > 0) {
            // collect_and_intersect may skip via overlaps_time_range; continue
        }

        // SAMPLE: reduce sel_indices before row collection
        if (stmt.sample_rate.has_value())
            apply_sample(sel_indices, *stmt.sample_rate);

        // ORDER BY가 있으면 LIMIT은 apply_order_by에서 처리 → 여기서 제한 없이 수집
        bool has_order = stmt.order_by.has_value();
        size_t collect_limit = has_order ? SIZE_MAX : stmt.limit.value_or(SIZE_MAX);
        if (!has_order && stmt.offset) collect_limit += static_cast<size_t>(*stmt.offset);

        for (uint32_t idx : sel_indices) {
            if (result.rows.size() >= collect_limit) break;

            std::vector<int64_t> row;
            std::vector<QueryResultSet::Value> trow;
            bool need_typed = false;
            if (is_star) {
                for (const auto& cv : part->columns()) {
                    if (cv->type() == ColumnType::FLOAT64 || cv->type() == ColumnType::FLOAT32) {
                        const double* d = static_cast<const double*>(cv->raw_data());
                        double fv = d ? d[idx] : 0.0;
                        row.push_back(store_double(fv));
                        trow.emplace_back(fv);
                        need_typed = true;
                    } else {
                        const int64_t* d = static_cast<const int64_t*>(cv->raw_data());
                        int64_t iv = d ? d[idx] : 0;
                        row.push_back(iv);
                        trow.emplace_back(iv);
                    }
                }
            } else {
                for (const auto& sel : stmt.columns) {
                    if (sel.window_func != WindowFunc::NONE) continue;
                    int64_t val;
                    if (sel.case_when) {
                        val = eval_case_when(*sel.case_when, *part, idx);
                        trow.emplace_back(val);
                    } else if (sel.arith_expr) {
                        val = eval_arith(*sel.arith_expr, *part, idx);
                        trow.emplace_back(val);
                    } else if (sel.column == "symbol") {
                        val = static_cast<int64_t>(part->key().symbol_id);
                        trow.emplace_back(val);
                    } else {
                        const auto* cv = part->get_column(sel.column);
                        if (cv && (cv->type() == ColumnType::FLOAT64 ||
                                   cv->type() == ColumnType::FLOAT32)) {
                            const double* d = static_cast<const double*>(cv->raw_data());
                            double fv = d ? d[idx] : 0.0;
                            val = store_double(fv);
                            trow.emplace_back(fv);
                            need_typed = true;
                        } else {
                            const int64_t* d = get_col_data(*part, sel.column);
                            val = d ? d[idx] : 0;
                            trow.emplace_back(val);
                        }
                    }
                    row.push_back(val);
                }
            }
            result.rows.push_back(std::move(row));
            if (need_typed)
                result.typed_rows.push_back(std::move(trow));
        }
    }

    result.rows_scanned = rows_scanned;

    // ORDER BY + LIMIT: top-N partial sort
    apply_order_by(result, stmt);
    if (!stmt.order_by.has_value())
        apply_offset_limit(result, stmt);

    return result;
}

// ============================================================================
// 총 행 수 추정
// ============================================================================
size_t QueryExecutor::estimate_total_rows(
    const std::vector<Partition*>& partitions) const
{
    size_t total = 0;
    for (auto* p : partitions) total += p->num_rows();
    return total;
}

// ============================================================================
// 집계 실행 (GROUP BY 없음)
// ============================================================================
QueryResultSet QueryExecutor::exec_agg(
    const SelectStmt& stmt,
    const std::vector<Partition*>& partitions)
{
    // 병렬 경로: 활성화 + pool_raw_ 존재 + 스레드 수 > 1 + 임계값 초과 시
    if (par_opts_.enabled && pool_raw_ &&
        pool_raw_->num_threads() > 1 &&
        estimate_total_rows(partitions) >= par_opts_.row_threshold &&
        partitions.size() >= 2)
    {
        try {
            return exec_agg_parallel(stmt, partitions);
        } catch (...) {
            // 폴백: 직렬 실행
        }
    }

    QueryResultSet result;
    size_t rows_scanned = 0;

    std::vector<double>   d_accum(stmt.columns.size(), 0.0);
    std::vector<int64_t>  cnt(stmt.columns.size(), 0);
    std::vector<double>   minv(stmt.columns.size(), std::numeric_limits<double>::max());
    std::vector<double>   maxv(stmt.columns.size(), std::numeric_limits<double>::lowest());
    std::vector<double>   vwap_pv(stmt.columns.size(), 0.0);
    std::vector<int64_t>  vwap_v(stmt.columns.size(), 0);
    std::vector<int64_t>  first_val(stmt.columns.size(), 0);
    std::vector<int64_t>  last_val(stmt.columns.size(), 0);
    std::vector<bool>     has_first(stmt.columns.size(), false);
    std::vector<std::set<int64_t>> distinct_sets(stmt.columns.size());
    // For STDDEV/VARIANCE/MEDIAN/PERCENTILE: collect all values
    std::vector<std::vector<int64_t>> val_collectors(stmt.columns.size());
    // Track which columns are float
    std::vector<bool> is_fcol(stmt.columns.size(), false);

    // Detect float columns from first partition
    if (!partitions.empty()) {
        for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
            const auto& col = stmt.columns[ci];
            if (!col.column.empty() && col.column != "*")
                is_fcol[ci] = col_is_float(*partitions[0], col.column);
        }
    }

    for (auto* part : partitions) {
        if (is_cancelled()) { QueryResultSet r; r.error = "Query cancelled"; return r; }

        std::vector<uint32_t> sel_indices = collect_and_intersect(stmt, *part, rows_scanned);

        // SAMPLE: reduce sel_indices before aggregation
        if (stmt.sample_rate.has_value())
            apply_sample(sel_indices, *stmt.sample_rate);

        for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
            const auto& col = stmt.columns[ci];
            const void* raw_ptr = nullptr;
            if (!col.arith_expr && !col.case_when) {
                const auto* cv = part->get_column(col.column);
                if (cv) raw_ptr = cv->raw_data();
            }
            auto agg_val = [&](uint32_t row_idx) -> int64_t {
                if (col.case_when) return eval_case_when(*col.case_when, *part, row_idx);
                if (col.arith_expr) return eval_arith(*col.arith_expr, *part, row_idx);
                if (!raw_ptr) return 0;
                if (is_fcol[ci])
                    return store_double(static_cast<const double*>(raw_ptr)[row_idx]);
                return static_cast<const int64_t*>(raw_ptr)[row_idx];
            };
            // Type-aware double reader
            auto agg_dval = [&](uint32_t row_idx) -> double {
                if (col.case_when || col.arith_expr)
                    return static_cast<double>(agg_val(row_idx));
                if (!raw_ptr) return 0.0;
                if (is_fcol[ci])
                    return static_cast<const double*>(raw_ptr)[row_idx];
                return static_cast<double>(static_cast<const int64_t*>(raw_ptr)[row_idx]);
            };

            switch (col.agg) {
                case AggFunc::COUNT:
                    if (col.agg_distinct) {
                        for (auto idx : sel_indices) distinct_sets[ci].insert(agg_val(idx));
                    } else {
                        cnt[ci] += static_cast<int64_t>(sel_indices.size());
                    }
                    break;
                case AggFunc::SUM:
                    for (auto idx : sel_indices) d_accum[ci] += agg_dval(idx);
                    break;
                case AggFunc::AVG:
                    for (auto idx : sel_indices) {
                        d_accum[ci] += agg_dval(idx);
                        cnt[ci]++;
                    }
                    break;
                case AggFunc::MIN:
                    for (auto idx : sel_indices)
                        minv[ci] = std::min(minv[ci], agg_dval(idx));
                    break;
                case AggFunc::MAX:
                    for (auto idx : sel_indices)
                        maxv[ci] = std::max(maxv[ci], agg_dval(idx));
                    break;
                case AggFunc::FIRST:
                    for (auto idx : sel_indices) {
                        int64_t v = agg_val(idx);
                        if (!has_first[ci]) { first_val[ci] = v; has_first[ci] = true; }
                        last_val[ci] = v;
                    }
                    break;
                case AggFunc::LAST:
                    for (auto idx : sel_indices) {
                        int64_t v = agg_val(idx);
                        if (!has_first[ci]) { first_val[ci] = v; has_first[ci] = true; }
                        last_val[ci] = v;
                    }
                    break;
                case AggFunc::XBAR:
                    // GROUP BY 없이 XBAR SELECT — 단순히 첫 번째 값 반환
                    if (raw_ptr && !sel_indices.empty() && !has_first[ci]) {
                        int64_t b = col.xbar_bucket;
                        int64_t v = agg_val(sel_indices[0]);
                        first_val[ci] = b > 0 ? (v / b) * b : v;
                        has_first[ci] = true;
                    }
                    break;
                case AggFunc::VWAP: {
                    const int64_t* vdata = get_col_data(*part, col.agg_arg2);
                    if (raw_ptr && vdata) {
                        for (auto idx : sel_indices) {
                            vwap_pv[ci] += agg_dval(idx)
                                         * static_cast<double>(vdata[idx]);
                            vwap_v[ci]  += vdata[idx];
                        }
                    }
                    break;
                }
                case AggFunc::NONE:
                    break;
                case AggFunc::STDDEV:
                case AggFunc::VARIANCE:
                case AggFunc::MEDIAN:
                case AggFunc::PERCENTILE:
                    for (auto idx : sel_indices)
                        val_collectors[ci].push_back(agg_val(idx));
                    break;
            }
        }
    }

    std::vector<QueryResultSet::Value> row(stmt.columns.size());

    bool has_float = std::any_of(is_fcol.begin(), is_fcol.end(), [](bool b){ return b; });

    for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
        const auto& col = stmt.columns[ci];
        std::string name = col.alias.empty()
            ? (col.column.empty() ? "*" : col.column)
            : col.alias;
        result.column_names.push_back(name);
        result.column_types.push_back(is_fcol[ci] ? ColumnType::FLOAT64 : ColumnType::INT64);

        switch (col.agg) {
            case AggFunc::COUNT: row[ci] = col.agg_distinct
                ? static_cast<int64_t>(distinct_sets[ci].size()) : cnt[ci]; break;
            case AggFunc::SUM:
                row[ci] = is_fcol[ci] ? QueryResultSet::Value(d_accum[ci])
                                      : QueryResultSet::Value(static_cast<int64_t>(d_accum[ci]));
                break;
            case AggFunc::AVG:
                if (cnt[ci] > 0)
                    row[ci] = is_fcol[ci] ? QueryResultSet::Value(d_accum[ci] / cnt[ci])
                                          : QueryResultSet::Value(static_cast<int64_t>(d_accum[ci] / cnt[ci]));
                else
                    row[ci] = INT64_MIN;
                break;
            case AggFunc::MIN:
                row[ci] = is_fcol[ci] ? QueryResultSet::Value(minv[ci])
                                      : QueryResultSet::Value(static_cast<int64_t>(minv[ci]));
                break;
            case AggFunc::MAX:
                row[ci] = is_fcol[ci] ? QueryResultSet::Value(maxv[ci])
                                      : QueryResultSet::Value(static_cast<int64_t>(maxv[ci]));
                break;
            case AggFunc::VWAP:
                row[ci] = vwap_v[ci] > 0
                    ? static_cast<int64_t>(vwap_pv[ci] / vwap_v[ci])
                    : INT64_MIN;
                break;
            case AggFunc::FIRST: row[ci] = first_val[ci]; break;
            case AggFunc::LAST:  row[ci] = last_val[ci];  break;
            case AggFunc::XBAR:  row[ci] = first_val[ci]; break;
            case AggFunc::STDDEV:
            case AggFunc::VARIANCE:
            case AggFunc::MEDIAN:
            case AggFunc::PERCENTILE:
                row[ci] = compute_stats(col.agg, val_collectors[ci], col.percentile_value);
                break;
            case AggFunc::NONE: row[ci] = int64_t{0}; break;
        }
    }

    // Populate typed_rows (float-native) and rows (backward compat)
    if (has_float) {
        result.typed_rows.push_back(row);
    }
    {
        std::vector<int64_t> irow(row.size());
        for (size_t c = 0; c < row.size(); ++c)
            irow[c] = is_fcol[c] ? store_double(row[c].f) : row[c].i;
        result.rows.push_back(std::move(irow));
    }
    result.rows_scanned = rows_scanned;
    return result;
}

// ============================================================================
// GROUP BY + 집계
// ============================================================================
// 최적화 전략 (직렬 경로):
//   1. GROUP BY symbol: 파티션 구조 직접 활용 — 각 파티션이 이미 symbol별로
//      분리되어 있으므로 hash table 불필요. O(partitions) not O(rows).
//   2. GROUP BY 기타 컬럼: pre-allocated hash map으로 O(n) 집계
//   3. 타임스탬프 범위: 이진탐색으로 스캔 범위 최소화
// ============================================================================
QueryResultSet QueryExecutor::exec_group_agg(
    const SelectStmt& stmt,
    const std::vector<Partition*>& partitions)
{
    // 병렬 경로: 활성화 + pool_raw_ 존재 + 스레드 수 > 1 + 임계값 초과 시
    if (par_opts_.enabled && pool_raw_ &&
        pool_raw_->num_threads() > 1 &&
        estimate_total_rows(partitions) >= par_opts_.row_threshold &&
        partitions.size() >= 2)
    {
        try {
            return exec_group_agg_parallel(stmt, partitions);
        } catch (...) {
            // 폴백: 직렬 실행
        }
    }

    QueryResultSet result;
    size_t rows_scanned = 0;

    const auto& gb = stmt.group_by.value();
    const std::string& group_col = gb.columns[0];
    // xbar 버킷 크기 (0이면 일반 컬럼, >0이면 xbar 플로어)
    int64_t group_xbar_bucket = gb.xbar_buckets.empty() ? 0 : gb.xbar_buckets[0];
    int64_t group_dt_bucket = gb.date_trunc_buckets.empty() ? 0 : gb.date_trunc_buckets[0];
    bool is_symbol_group = (gb.columns.size() == 1 && group_col == "symbol"
                            && group_xbar_bucket == 0 && group_dt_bucket == 0);

    struct GroupState {
        int64_t  sum     = 0;
        int64_t  count   = 0;
        double   avg_sum = 0.0;
        int64_t  minv    = INT64_MAX;
        int64_t  maxv    = INT64_MIN;
        double   vwap_pv = 0.0;
        int64_t  vwap_v  = 0;
        int64_t  first_val = 0;
        int64_t  last_val  = 0;
        bool     has_first = false;
        std::vector<int64_t> vals; // STDDEV/VARIANCE/MEDIAN/PERCENTILE
    };

    // ─────────────────────────────────────────────────────────────────────
    // 최적화 경로 1: GROUP BY symbol
    // 파티션은 이미 symbol 단위로 분리되어 있음.
    // 각 파티션의 symbol_id = group key → hash table 완전 불필요.
    // O(partitions × rows_per_partition) but sequential access, no hashing.
    // ─────────────────────────────────────────────────────────────────────
    if (is_symbol_group) {
        // symbol_id → states (파티션 순회로 직접 누적)
        // 같은 symbol이 여러 파티션에 걸쳐 있을 수 있으므로 map 사용
        // (하지만 key=symbol_id 이므로 hashing cost << full-row hashing)
        std::unordered_map<int64_t, std::vector<GroupState>> groups;
        groups.reserve(partitions.size()); // 대부분 파티션 수 ≈ symbol 수

        for (auto* part : partitions) {
            if (is_cancelled()) { QueryResultSet r; r.error = "Query cancelled"; return r; }
            // symbol group key: 파티션 키에서 O(1)로 추출
            int64_t symbol_gkey = static_cast<int64_t>(part->key().symbol_id);

            std::vector<uint32_t> sel_indices = collect_and_intersect(stmt, *part, rows_scanned);

            // SAMPLE: reduce sel_indices before group aggregation
            if (stmt.sample_rate.has_value())
                apply_sample(sel_indices, *stmt.sample_rate);

            auto& states = groups[symbol_gkey];
            if (states.empty()) states.resize(stmt.columns.size());

            for (auto idx : sel_indices) {
                for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
                    const auto& col = stmt.columns[ci];
                    auto& gs = states[ci];
                    if (col.agg == AggFunc::NONE) continue;
                    const int64_t* data = (col.arith_expr || col.case_when)
                        ? nullptr : get_col_data(*part, col.column);
                    auto agg_v = [&]() -> int64_t {
                        if (col.case_when) return eval_case_when(*col.case_when, *part, idx);
                        if (col.arith_expr) return eval_arith(*col.arith_expr, *part, idx);
                        return data ? data[idx] : 0;
                    };
                    switch (col.agg) {
                        case AggFunc::COUNT: gs.count++; break;
                        case AggFunc::SUM:   gs.sum += agg_v(); break;
                        case AggFunc::AVG:   gs.avg_sum += agg_v(); gs.count++; break;
                        case AggFunc::MIN:   gs.minv = std::min(gs.minv, agg_v()); break;
                        case AggFunc::MAX:   gs.maxv = std::max(gs.maxv, agg_v()); break;
                        case AggFunc::FIRST: {
                            int64_t v = agg_v();
                            if (!gs.has_first) { gs.first_val = v; gs.has_first = true; }
                            gs.last_val = v;
                            break;
                        }
                        case AggFunc::LAST: {
                            int64_t v = agg_v();
                            if (!gs.has_first) { gs.first_val = v; gs.has_first = true; }
                            gs.last_val = v;
                            break;
                        }
                        case AggFunc::XBAR:
                            // XBAR는 GROUP BY 키 계산에 쓰임, SELECT에서도 쓸 수 있음
                            // SELECT xbar(timestamp, bucket) → 해당 버킷 플로어 값
                            if (data && !gs.has_first) {
                                int64_t bucket = col.xbar_bucket;
                                gs.first_val = bucket > 0
                                    ? (data[idx] / bucket) * bucket
                                    : data[idx];
                                gs.has_first = true;
                            }
                            break;
                        case AggFunc::VWAP: {
                            const int64_t* vd = get_col_data(*part, col.agg_arg2);
                            if (data && vd) {
                                gs.vwap_pv += static_cast<double>(data[idx])
                                            * static_cast<double>(vd[idx]);
                                gs.vwap_v  += vd[idx];
                            }
                            break;
                        }
                        case AggFunc::STDDEV: case AggFunc::VARIANCE:
                        case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                            gs.vals.push_back(agg_v()); break;
                        case AggFunc::NONE: break;
                    }
                }
            }
        }

        // 결과 컬럼 이름 설정
        result.column_names.push_back(group_col);
        result.column_types.push_back(ColumnType::INT64);
        for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
            const auto& col = stmt.columns[ci];
            if (col.agg == AggFunc::NONE) continue;
            std::string name = col.alias.empty()
                ? (col.column.empty() ? "*" : col.column) : col.alias;
            result.column_names.push_back(name);
            result.column_types.push_back(ColumnType::INT64);
        }

        for (auto& [gkey, states] : groups) {
            std::vector<int64_t> row;
            row.push_back(gkey);
            for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
                const auto& col = stmt.columns[ci];
                if (col.agg == AggFunc::NONE) continue;
                const auto& gs = states[ci];
                switch (col.agg) {
                    case AggFunc::COUNT: row.push_back(gs.count); break;
                    case AggFunc::SUM:   row.push_back(gs.sum);   break;
                    case AggFunc::AVG:
                        row.push_back(gs.count > 0
                            ? static_cast<int64_t>(gs.avg_sum / gs.count) : 0); break;
                    case AggFunc::MIN:
                        row.push_back(gs.minv == INT64_MAX ? INT64_MIN : gs.minv); break;
                    case AggFunc::MAX:
                        row.push_back(gs.maxv == INT64_MIN ? INT64_MIN : gs.maxv); break;
                    case AggFunc::VWAP:
                        row.push_back(gs.vwap_v > 0
                            ? static_cast<int64_t>(gs.vwap_pv / gs.vwap_v) : INT64_MIN); break;
                    case AggFunc::FIRST: row.push_back(gs.first_val); break;
                    case AggFunc::LAST:  row.push_back(gs.last_val);  break;
                    case AggFunc::XBAR:  row.push_back(gs.first_val); break;
                    case AggFunc::NONE: break;
                    case AggFunc::STDDEV: case AggFunc::VARIANCE:
                    case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                        row.push_back(compute_stats(col.agg, gs.vals, col.percentile_value)); break;
                }
            }
            result.rows.push_back(std::move(row));
        }

        result.rows_scanned = rows_scanned;

        // HAVING 필터 (is_symbol_group 경로에도 적용)
        if (stmt.having.has_value())
            result = apply_having_filter(std::move(result), stmt.having.value());

        // ORDER BY + LIMIT
        apply_order_by(result, stmt);
        if (!stmt.order_by.has_value() &&
            stmt.limit.has_value() && result.rows.size() > (size_t)stmt.limit.value()) {
            result.rows.resize(stmt.limit.value());
        }
        return result;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Optimized path 2: single-column GROUP BY (non-symbol).
    // Uses flat int64_t key → zero per-row heap allocations.
    // Also hoists column data pointers out of the inner row loop.
    // Handles XBAR, plain column, and arith_expr group keys.
    // ─────────────────────────────────────────────────────────────────────
    if (gb.columns.size() == 1) {
        const int64_t bucket = gb.xbar_buckets.empty() ? 0 : gb.xbar_buckets[0];
        const int64_t dt_bucket = gb.date_trunc_buckets.empty() ? 0 : gb.date_trunc_buckets[0];
        const size_t ncols = stmt.columns.size();

        // Flat GroupState: key→slot index map + single contiguous array.
        // Eliminates per-group vector<GroupState> heap allocations (N_groups × alloc).
        // flat_states layout: [slot0_col0, slot0_col1, ..., slot1_col0, slot1_col1, ...]
        std::unordered_map<int64_t, uint32_t> key_to_slot;
        key_to_slot.reserve(4096);
        std::vector<GroupState> flat_states;
        flat_states.reserve(4096 * ncols);
        uint32_t next_slot = 0;

        // Sorted-scan cache: for XBAR on sorted timestamps, consecutive rows share
        // the same bucket key.  Cache the last-seen (key, slot) pair so hash lookup
        // only fires on group-boundary crossings (~N_groups) instead of every row (~N_rows).
        int64_t  cached_key  = INT64_MIN;
        uint32_t cached_slot = 0;

        for (auto* part : partitions) {
            if (is_cancelled()) { QueryResultSet r; r.error = "Query cancelled"; return r; }

            std::vector<uint32_t> sel_indices = collect_and_intersect(stmt, *part, rows_scanned);

            // SAMPLE: reduce sel_indices before group aggregation
            if (stmt.sample_rate.has_value())
                apply_sample(sel_indices, *stmt.sample_rate);

            // Hoist group key column pointer (symbol handled inline)
            const int64_t* gkey_col = (group_col == "symbol")
                ? nullptr : get_col_data(*part, group_col);
            const int64_t symbol_kv = static_cast<int64_t>(part->key().symbol_id);

            // Hoist aggregate column pointers to partition scope
            std::vector<const int64_t*> col_ptrs(ncols, nullptr);
            std::vector<const int64_t*> vwap_ptrs(ncols, nullptr);
            for (size_t ci = 0; ci < ncols; ++ci) {
                const auto& col = stmt.columns[ci];
                if (col.agg == AggFunc::NONE || col.arith_expr) continue;
                col_ptrs[ci] = get_col_data(*part, col.column);
                if (col.agg == AggFunc::VWAP)
                    vwap_ptrs[ci] = get_col_data(*part, col.agg_arg2);
            }

            for (auto idx : sel_indices) {
                // Compute flat int64_t key — no heap allocation
                int64_t kv = gkey_col ? gkey_col[idx] : symbol_kv;
                if (bucket > 0) kv = (kv / bucket) * bucket;
                if (dt_bucket > 0) kv = (kv / dt_bucket) * dt_bucket;

                // Sorted-scan fast path: skip hash lookup when key unchanged.
                if (__builtin_expect(kv != cached_key, 0)) {
                    auto it = key_to_slot.find(kv);
                    if (__builtin_expect(it == key_to_slot.end(), 0)) {
                        it = key_to_slot.emplace(kv, next_slot++).first;
                        flat_states.resize(flat_states.size() + ncols);
                    }
                    cached_key  = kv;
                    cached_slot = it->second;
                }
                GroupState* states = flat_states.data() + cached_slot * ncols;

                for (size_t ci = 0; ci < ncols; ++ci) {
                    const auto& col = stmt.columns[ci];
                    auto& gs = states[ci];
                    if (col.agg == AggFunc::NONE) continue;
                    const int64_t* data = col_ptrs[ci];
                    auto agg_v = [&]() -> int64_t {
                        if (col.case_when) return eval_case_when(*col.case_when, *part, idx);
                        if (col.arith_expr) return eval_arith(*col.arith_expr, *part, idx);
                        return data ? data[idx] : 0;
                    };
                    switch (col.agg) {
                        case AggFunc::COUNT: gs.count++; break;
                        case AggFunc::SUM:   gs.sum += agg_v(); break;
                        case AggFunc::AVG:   gs.avg_sum += agg_v(); gs.count++; break;
                        case AggFunc::MIN:   gs.minv = std::min(gs.minv, agg_v()); break;
                        case AggFunc::MAX:   gs.maxv = std::max(gs.maxv, agg_v()); break;
                        case AggFunc::FIRST: {
                            int64_t v = agg_v();
                            if (!gs.has_first) { gs.first_val = v; gs.has_first = true; }
                            gs.last_val = v;
                            break;
                        }
                        case AggFunc::LAST: {
                            int64_t v = agg_v();
                            if (!gs.has_first) { gs.first_val = v; gs.has_first = true; }
                            gs.last_val = v;
                            break;
                        }
                        case AggFunc::XBAR:
                            if (!gs.has_first) {
                                int64_t v = data ? data[idx] : 0;
                                int64_t b = col.xbar_bucket;
                                gs.first_val = b > 0 ? (v / b) * b : v;
                                gs.has_first = true;
                            }
                            break;
                        case AggFunc::VWAP: {
                            const int64_t* vd = vwap_ptrs[ci];
                            if (data && vd) {
                                gs.vwap_pv += static_cast<double>(data[idx])
                                            * static_cast<double>(vd[idx]);
                                gs.vwap_v  += vd[idx];
                            }
                            break;
                        }
                        case AggFunc::NONE: break;
                        case AggFunc::STDDEV: case AggFunc::VARIANCE:
                        case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                            gs.vals.push_back(agg_v()); break;
                    }
                }
            }
        }

        // Output column names
        result.column_names.push_back(group_col);
        result.column_types.push_back(ColumnType::INT64);
        for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
            const auto& col = stmt.columns[ci];
            if (col.agg == AggFunc::NONE) continue;
            std::string name = col.alias.empty()
                ? (col.arith_expr ? "expr" : (col.column.empty() ? "*" : col.column))
                : col.alias;
            result.column_names.push_back(name);
            result.column_types.push_back(ColumnType::INT64);
        }

        for (auto& [gkey_scalar, slot] : key_to_slot) {
            std::vector<int64_t> row;
            row.push_back(gkey_scalar);
            const GroupState* states = flat_states.data() + slot * ncols;
            for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
                const auto& col = stmt.columns[ci];
                if (col.agg == AggFunc::NONE) continue;
                const auto& gs = states[ci];
                switch (col.agg) {
                    case AggFunc::COUNT: row.push_back(gs.count); break;
                    case AggFunc::SUM:   row.push_back(gs.sum);   break;
                    case AggFunc::AVG:
                        row.push_back(gs.count > 0
                            ? static_cast<int64_t>(gs.avg_sum / gs.count) : 0);
                        break;
                    case AggFunc::MIN:
                        row.push_back(gs.minv == INT64_MAX ? INT64_MIN : gs.minv);
                        break;
                    case AggFunc::MAX:
                        row.push_back(gs.maxv == INT64_MIN ? INT64_MIN : gs.maxv);
                        break;
                    case AggFunc::VWAP:
                        row.push_back(gs.vwap_v > 0
                            ? static_cast<int64_t>(gs.vwap_pv / gs.vwap_v) : INT64_MIN);
                        break;
                    case AggFunc::FIRST: row.push_back(gs.first_val); break;
                    case AggFunc::LAST:  row.push_back(gs.last_val);  break;
                    case AggFunc::XBAR:  row.push_back(gs.first_val); break;
                    case AggFunc::STDDEV: case AggFunc::VARIANCE:
                    case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                        row.push_back(compute_stats(col.agg, gs.vals, col.percentile_value)); break;
                    case AggFunc::NONE:  break;
                }
            }
            result.rows.push_back(std::move(row));
        }

        result.rows_scanned = rows_scanned;
        if (stmt.having.has_value())
            result = apply_having_filter(std::move(result), stmt.having.value());
        apply_order_by(result, stmt);
        if (!stmt.order_by.has_value() &&
            stmt.limit.has_value() && result.rows.size() > (size_t)stmt.limit.value())
            result.rows.resize(stmt.limit.value());
        return result;
    }

    // ─────────────────────────────────────────────────────────────────────
    // General path: multi-column GROUP BY.
    // Composite key: std::vector<int64_t> with VectorHash.
    // ─────────────────────────────────────────────────────────────────────
    std::unordered_map<std::vector<int64_t>, std::vector<GroupState>, VectorHash> groups;
    groups.reserve(1024);

    // Helper: build composite group key for one row
    auto make_group_key = [&](const Partition& part, uint32_t idx)
        -> std::vector<int64_t>
    {
        std::vector<int64_t> key;
        key.reserve(gb.columns.size());
        for (size_t gi = 0; gi < gb.columns.size(); ++gi) {
            const std::string& gcol   = gb.columns[gi];
            int64_t            bucket = gb.xbar_buckets[gi];
            int64_t            dt_bucket = (gi < gb.date_trunc_buckets.size())
                                           ? gb.date_trunc_buckets[gi] : 0;
            int64_t            kv;
            if (gcol == "symbol") {
                kv = static_cast<int64_t>(part.key().symbol_id);
            } else {
                const int64_t* gdata = get_col_data(part, gcol);
                kv = gdata ? gdata[idx] : 0;
            }
            if (bucket > 0) kv = (kv / bucket) * bucket;
            if (dt_bucket > 0) kv = (kv / dt_bucket) * dt_bucket;
            key.push_back(kv);
        }
        return key;
    };

    for (auto* part : partitions) {
        if (is_cancelled()) { QueryResultSet r; r.error = "Query cancelled"; return r; }

        std::vector<uint32_t> sel_indices = collect_and_intersect(stmt, *part, rows_scanned);

        // SAMPLE: reduce sel_indices before group aggregation
        if (stmt.sample_rate.has_value())
            apply_sample(sel_indices, *stmt.sample_rate);

        for (auto idx : sel_indices) {
            auto gkey  = make_group_key(*part, idx);
            auto& states = groups[gkey];
            if (states.empty()) states.resize(stmt.columns.size());

            for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
                const auto& col = stmt.columns[ci];
                auto& gs = states[ci];
                if (col.agg == AggFunc::NONE) continue;
                const int64_t* data = (col.arith_expr || col.case_when)
                    ? nullptr : get_col_data(*part, col.column);
                auto agg_v = [&]() -> int64_t {
                    if (col.case_when) return eval_case_when(*col.case_when, *part, idx);
                    if (col.arith_expr) return eval_arith(*col.arith_expr, *part, idx);
                    return data ? data[idx] : 0;
                };
                switch (col.agg) {
                    case AggFunc::COUNT: gs.count++; break;
                    case AggFunc::SUM:   gs.sum += agg_v(); break;
                    case AggFunc::AVG:   gs.avg_sum += agg_v(); gs.count++; break;
                    case AggFunc::MIN:   gs.minv = std::min(gs.minv, agg_v()); break;
                    case AggFunc::MAX:   gs.maxv = std::max(gs.maxv, agg_v()); break;
                    case AggFunc::FIRST: {
                        int64_t v = agg_v();
                        if (!gs.has_first) { gs.first_val = v; gs.has_first = true; }
                        gs.last_val = v;
                        break;
                    }
                    case AggFunc::LAST: {
                        int64_t v = agg_v();
                        if (!gs.has_first) { gs.first_val = v; gs.has_first = true; }
                        gs.last_val = v;
                        break;
                    }
                    case AggFunc::XBAR:
                        if (!gs.has_first) {
                            int64_t v = data ? data[idx] : 0;
                            int64_t b = col.xbar_bucket;
                            gs.first_val = b > 0 ? (v / b) * b : v;
                            gs.has_first = true;
                        }
                        break;
                    case AggFunc::VWAP: {
                        const int64_t* vd = get_col_data(*part, col.agg_arg2);
                        if (data && vd) {
                            gs.vwap_pv += static_cast<double>(data[idx])
                                        * static_cast<double>(vd[idx]);
                            gs.vwap_v  += vd[idx];
                        }
                        break;
                    }
                    case AggFunc::NONE: break;
                    case AggFunc::STDDEV: case AggFunc::VARIANCE:
                    case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                        gs.vals.push_back(agg_v()); break;
                }
            }
        }
    }

    // Output column names: all GROUP BY columns first, then aggregates
    for (const auto& gcol : gb.columns)  {
        result.column_names.push_back(gcol);
        result.column_types.push_back(ColumnType::INT64);
    }
    for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
        const auto& col = stmt.columns[ci];
        if (col.agg == AggFunc::NONE) continue;
        std::string name = col.alias.empty()
            ? (col.arith_expr ? "expr" : (col.column.empty() ? "*" : col.column))
            : col.alias;
        result.column_names.push_back(name);
        result.column_types.push_back(ColumnType::INT64);
    }

    for (auto& [gkey_vec, states] : groups) {
        std::vector<int64_t> row;
        // All group key columns
        for (int64_t k : gkey_vec) row.push_back(k);
        // Aggregate columns
        for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
            const auto& col = stmt.columns[ci];
            if (col.agg == AggFunc::NONE) continue;
            const auto& gs = states[ci];
            switch (col.agg) {
                case AggFunc::COUNT: row.push_back(gs.count); break;
                case AggFunc::SUM:   row.push_back(gs.sum);   break;
                case AggFunc::AVG:
                    row.push_back(gs.count > 0
                        ? static_cast<int64_t>(gs.avg_sum / gs.count) : 0);
                    break;
                case AggFunc::MIN:
                    row.push_back(gs.minv == INT64_MAX ? INT64_MIN : gs.minv);
                    break;
                case AggFunc::MAX:
                    row.push_back(gs.maxv == INT64_MIN ? INT64_MIN : gs.maxv);
                    break;
                case AggFunc::VWAP:
                    row.push_back(gs.vwap_v > 0
                        ? static_cast<int64_t>(gs.vwap_pv / gs.vwap_v) : INT64_MIN);
                    break;
                case AggFunc::FIRST: row.push_back(gs.first_val); break;
                case AggFunc::LAST:  row.push_back(gs.last_val);  break;
                case AggFunc::XBAR:  row.push_back(gs.first_val); break;
                case AggFunc::STDDEV: case AggFunc::VARIANCE:
                case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                    row.push_back(compute_stats(col.agg, gs.vals, col.percentile_value)); break;
                case AggFunc::NONE:  break;
            }
        }
        result.rows.push_back(std::move(row));
    }

    result.rows_scanned = rows_scanned;

    // HAVING 필터 (집계 결과에 적용)
    if (stmt.having.has_value())
        result = apply_having_filter(std::move(result), stmt.having.value());

    // ORDER BY + LIMIT
    apply_order_by(result, stmt);
    if (!stmt.order_by.has_value() &&
        stmt.limit.has_value() && result.rows.size() > (size_t)stmt.limit.value()) {
        result.rows.resize(stmt.limit.value());
    }

    return result;
}

// ============================================================================
// ASOF JOIN 실행
// ============================================================================
QueryResultSet QueryExecutor::exec_asof_join(
    const SelectStmt& stmt,
    const std::vector<Partition*>& left_parts,
    const std::vector<Partition*>& right_parts)
{
    QueryResultSet result;
    size_t rows_scanned = 0;

    if (left_parts.empty() || right_parts.empty()) {
        return result;
    }

    auto* lp = left_parts[0];
    auto* rp = right_parts[0];
    size_t ln = lp->num_rows();
    size_t rn = rp->num_rows();
    rows_scanned = ln + rn;

    std::string l_key_col, r_key_col;
    std::string l_time_col, r_time_col;

    for (const auto& cond : stmt.join->on_conditions) {
        if (cond.op == CompareOp::EQ) {
            l_key_col = cond.left_col;
            r_key_col = cond.right_col;
        } else if (cond.op == CompareOp::GE || cond.op == CompareOp::GT) {
            l_time_col = cond.left_col;
            r_time_col = cond.right_col;
        }
    }

    if (l_key_col.empty())  l_key_col  = "symbol";
    if (r_key_col.empty())  r_key_col  = "symbol";
    if (l_time_col.empty()) l_time_col = "timestamp";
    if (r_time_col.empty()) r_time_col = "timestamp";

    const ColumnVector* lk_cv = lp->get_column(l_key_col);
    const ColumnVector* rk_cv = rp->get_column(r_key_col);
    const ColumnVector* lt_cv = lp->get_column(l_time_col);
    const ColumnVector* rt_cv = rp->get_column(r_time_col);

    if (!lk_cv || !rk_cv || !lt_cv || !rt_cv) {
        result.error = "ASOF JOIN: required columns not found";
        return result;
    }

    AsofJoinOperator asof;
    JoinResult jres = asof.execute(*lk_cv, *rk_cv, lt_cv, rt_cv);

    const std::string l_alias = stmt.from_alias;
    const std::string r_alias = stmt.join->alias;
    const bool aj0_mode = (stmt.join->type == JoinClause::Type::AJ0);

    for (const auto& sel : stmt.columns) {
        if (sel.is_star) {
            for (const auto& cv : lp->columns()) {
                result.column_names.push_back(cv->name());
                result.column_types.push_back(cv->type());
            }
            continue;
        }
        // AJ0: skip right-table columns
        if (aj0_mode && !sel.table_alias.empty() && sel.table_alias == r_alias)
            continue;
        result.column_names.push_back(
            sel.alias.empty() ? sel.column : sel.alias);
        result.column_types.push_back(ColumnType::INT64);
    }

    size_t limit = stmt.limit.value_or(INT64_MAX);
    for (size_t i = 0; i < jres.match_count && result.rows.size() < limit; ++i) {
        int64_t li = jres.left_indices[i];
        int64_t ri = jres.right_indices[i];

        std::vector<int64_t> row;
        for (const auto& sel : stmt.columns) {
            if (sel.is_star) {
                for (const auto& cv : lp->columns()) {
                    const int64_t* d = static_cast<const int64_t*>(cv->raw_data());
                    row.push_back(d ? d[li] : 0);
                }
                continue;
            }
            bool is_right = (!sel.table_alias.empty() && sel.table_alias == r_alias);
            // AJ0: skip right-table columns
            if (aj0_mode && is_right) continue;
            if (is_right && ri >= 0) {
                const int64_t* d = get_col_data(*rp, sel.column);
                row.push_back(d ? d[ri] : 0);
            } else {
                const int64_t* d = get_col_data(*lp, sel.column);
                row.push_back(d ? d[li] : 0);
            }
        }
        result.rows.push_back(std::move(row));
    }

    result.rows_scanned = rows_scanned;
    return result;
}

// ============================================================================
// find_hash_join_node — walk physical plan tree to find HASH_JOIN node
// ============================================================================
const zeptodb::execution::PhysicalNode* QueryExecutor::find_hash_join_node(
    const std::shared_ptr<execution::PhysicalNode>& plan) {
    if (!plan) return nullptr;
    std::vector<const execution::PhysicalNode*> stack{plan.get()};
    while (!stack.empty()) {
        auto* n = stack.back(); stack.pop_back();
        if (n->type == execution::PhysicalNode::Type::HASH_JOIN) return n;
        if (n->right) stack.push_back(n->right.get());
        if (n->left)  stack.push_back(n->left.get());
    }
    return nullptr;
}

// ============================================================================
// Hash JOIN 실행 (equi join: INNER / LEFT)
// ============================================================================
// 알고리즘:
//   1. 모든 왼쪽/오른쪽 파티션의 데이터를 합쳐서 flat 벡터로 만듦
//   2. HashJoinOperator로 인덱스 쌍 계산
//   3. 인덱스 쌍으로 결과 행 조립
// ============================================================================
QueryResultSet QueryExecutor::exec_hash_join(
    const SelectStmt& stmt,
    const std::vector<Partition*>& left_parts,
    const std::vector<Partition*>& right_parts,
    const std::shared_ptr<execution::PhysicalNode>& physical_plan)
{
    QueryResultSet result;
    size_t rows_scanned = 0;

    if (left_parts.empty() || right_parts.empty()) {
        return result;
    }

    // ON 조건에서 equi join 키 추출 (첫 번째 EQ 조건 사용)
    std::string l_key_col, r_key_col;
    for (const auto& cond : stmt.join->on_conditions) {
        if (cond.op == CompareOp::EQ) {
            l_key_col = cond.left_col;
            r_key_col = cond.right_col;
            break;
        }
    }
    if (l_key_col.empty() || r_key_col.empty()) {
        result.error = "Hash JOIN: no equi-join condition found (need col = col)";
        return result;
    }

    const std::string l_alias = stmt.from_alias;
    const std::string r_alias = stmt.join->alias;

    // ── 왼쪽/오른쪽 키 데이터를 flat 벡터로 수집 ──
    // 파티션 경계 추적: flat_to_part[i] = (part_idx, local_row_idx)
    struct RowRef { size_t part_idx; size_t local_idx; };
    std::vector<int64_t> l_keys_flat, r_keys_flat;
    std::vector<RowRef>  l_refs, r_refs;

    for (size_t pi = 0; pi < left_parts.size(); ++pi) {
        auto* part = left_parts[pi];
        const int64_t* kd = get_col_data(*part, l_key_col);
        size_t n = part->num_rows();
        rows_scanned += n;
        for (size_t i = 0; i < n; ++i) {
            l_keys_flat.push_back(kd ? kd[i] : 0);
            l_refs.push_back({pi, i});
        }
    }

    for (size_t pi = 0; pi < right_parts.size(); ++pi) {
        auto* part = right_parts[pi];
        const int64_t* kd = get_col_data(*part, r_key_col);
        size_t n = part->num_rows();
        rows_scanned += n;
        for (size_t i = 0; i < n; ++i) {
            r_keys_flat.push_back(kd ? kd[i] : 0);
            r_refs.push_back({pi, i});
        }
    }

    // ── Cost-based planner: check if build side should be swapped ──
    // For INNER/FULL joins, the planner may recommend building on the left
    // (smaller) side instead of the default right side.
    bool is_left_join  = (stmt.join->type == JoinClause::Type::LEFT);
    bool is_right_join = (stmt.join->type == JoinClause::Type::RIGHT);
    bool is_full_join  = (stmt.join->type == JoinClause::Type::FULL);
    bool planner_swap  = false;  // true = planner says build on left for INNER/FULL
    if (!is_left_join && !is_right_join) {
        auto* hj_node = find_hash_join_node(physical_plan);
        if (hj_node && !hj_node->build_right) {
            planner_swap = true;
            ZEPTO_INFO("HashJoin: planner-directed build side swap (build=left, L={}rows R={}rows)",
                       l_keys_flat.size(), r_keys_flat.size());
        }
    }

    // ── Build hash map on the chosen side ──
    // Default: build on right. With planner_swap: build on left.
    auto& build_keys = planner_swap ? l_keys_flat : r_keys_flat;
    std::unordered_map<int64_t, std::vector<size_t>> hash_map;
    hash_map.reserve(build_keys.size() * 2);
    for (size_t i = 0; i < build_keys.size(); ++i) {
        hash_map[build_keys[i]].push_back(i);
    }

    // 매칭 인덱스 쌍 생성
    // LEFT JOIN:  unmatched left  rows → r_index = SIZE_MAX (right  NULL)
    // RIGHT JOIN: unmatched right rows → l_index = SIZE_MAX (left NULL)
    // FULL JOIN:  both unmatched sides included
    std::vector<size_t> matched_l; // SIZE_MAX = left-side NULL (RIGHT JOIN)
    std::vector<size_t> matched_r; // SIZE_MAX = right-side NULL (LEFT JOIN)

    if (is_right_join) {
        // RIGHT JOIN: build hash on left, iterate right
        std::unordered_map<int64_t, std::vector<size_t>> l_map;
        l_map.reserve(l_keys_flat.size() * 2);
        for (size_t li = 0; li < l_keys_flat.size(); ++li)
            l_map[l_keys_flat[li]].push_back(li);

        for (size_t ri = 0; ri < r_keys_flat.size(); ++ri) {
            auto it = l_map.find(r_keys_flat[ri]);
            if (it == l_map.end()) {
                matched_l.push_back(SIZE_MAX); // left NULL
                matched_r.push_back(ri);
                continue;
            }
            for (size_t li : it->second) {
                matched_l.push_back(li);
                matched_r.push_back(ri);
            }
        }
    } else if (is_full_join) {
        // FULL OUTER JOIN: probe side iterates, build side in hash_map
        auto& probe_keys = planner_swap ? r_keys_flat : l_keys_flat;
        auto& build_keys_ref = planner_swap ? l_keys_flat : r_keys_flat;
        std::vector<bool> build_matched(build_keys_ref.size(), false);
        for (size_t pi = 0; pi < probe_keys.size(); ++pi) {
            auto it = hash_map.find(probe_keys[pi]);
            if (it == hash_map.end()) {
                // probe side unmatched
                if (planner_swap) { matched_l.push_back(SIZE_MAX); matched_r.push_back(pi); }
                else              { matched_l.push_back(pi); matched_r.push_back(SIZE_MAX); }
                continue;
            }
            for (size_t bi : it->second) {
                if (planner_swap) { matched_l.push_back(bi); matched_r.push_back(pi); }
                else              { matched_l.push_back(pi); matched_r.push_back(bi); }
                build_matched[bi] = true;
            }
        }
        // Append unmatched build side rows
        for (size_t bi = 0; bi < build_keys_ref.size(); ++bi) {
            if (!build_matched[bi]) {
                if (planner_swap) { matched_l.push_back(bi); matched_r.push_back(SIZE_MAX); }
                else              { matched_l.push_back(SIZE_MAX); matched_r.push_back(bi); }
            }
        }
    } else {
        // INNER / LEFT: probe side iterates, build side in hash_map
        // Default: probe=left, build=right. planner_swap: probe=right, build=left.
        if (planner_swap) {
            // INNER only (LEFT excluded by planner_swap guard above)
            for (size_t ri = 0; ri < r_keys_flat.size(); ++ri) {
                auto it = hash_map.find(r_keys_flat[ri]);
                if (it == hash_map.end()) continue;
                for (size_t li : it->second) {
                    matched_l.push_back(li);
                    matched_r.push_back(ri);
                }
            }
        } else {
            for (size_t li = 0; li < l_keys_flat.size(); ++li) {
                auto it = hash_map.find(l_keys_flat[li]);
                if (it == hash_map.end()) {
                    if (is_left_join) {
                        matched_l.push_back(li);
                        matched_r.push_back(SIZE_MAX); // right NULL
                    }
                    continue;
                }
                for (size_t ri : it->second) {
                    matched_l.push_back(li);
                    matched_r.push_back(ri);
                }
            }
        }
    }

    // ── 결과 컬럼 이름 설정 ──
    // SELECT 목록 순서대로 (alias 기준으로 왼쪽/오른쪽 구분)
    for (const auto& sel : stmt.columns) {
        if (sel.is_star) {
            // t.* → 왼쪽 테이블 전체
            if (!left_parts.empty()) {
                for (const auto& cv : left_parts[0]->columns()) {
                    result.column_names.push_back(cv->name());
                    result.column_types.push_back(cv->type());
                }
            }
            continue;
        }
        std::string col_name = sel.alias.empty() ? sel.column : sel.alias;
        result.column_names.push_back(col_name);
        result.column_types.push_back(ColumnType::INT64);
    }

    // ── 결과 행 조립 ──
    size_t limit = stmt.limit.value_or(INT64_MAX);
    for (size_t m = 0; m < matched_l.size() && result.rows.size() < limit; ++m) {
        bool left_null  = (matched_l[m] == SIZE_MAX); // RIGHT JOIN: 왼쪽 없음
        bool right_null = (matched_r[m] == SIZE_MAX); // LEFT  JOIN: 오른쪽 없음

        auto* lp = (!left_null) ? left_parts[l_refs[matched_l[m]].part_idx] : nullptr;
        Partition* rp = nullptr;
        RowRef lr{0, 0}, rr{0, 0};
        if (!left_null)  lr = l_refs[matched_l[m]];
        if (!right_null) { rr = r_refs[matched_r[m]]; rp = right_parts[rr.part_idx]; }

        std::vector<int64_t> row;
        for (const auto& sel : stmt.columns) {
            if (sel.is_star) {
                // star expands left table columns
                auto* star_part = lp ? lp : (!right_parts.empty() ? left_parts[0] : nullptr);
                if (star_part) {
                    for (const auto& cv : star_part->columns()) {
                        if (left_null) { row.push_back(JOIN_NULL); continue; }
                        const int64_t* d = static_cast<const int64_t*>(cv->raw_data());
                        row.push_back(d ? d[lr.local_idx] : 0);
                    }
                }
                continue;
            }
            bool is_right = (!sel.table_alias.empty() && sel.table_alias == r_alias);
            if (is_right) {
                if (right_null || !rp) {
                    row.push_back(JOIN_NULL); // NULL sentinel (INT64_MIN)
                } else {
                    const int64_t* d = get_col_data(*rp, sel.column);
                    row.push_back(d ? d[rr.local_idx] : 0);
                }
            } else {
                if (left_null || !lp) {
                    row.push_back(JOIN_NULL); // NULL sentinel (INT64_MIN)
                } else {
                    const int64_t* d = get_col_data(*lp, sel.column);
                    row.push_back(d ? d[lr.local_idx] : 0);
                }
            }
        }
        result.rows.push_back(std::move(row));
    }

    result.rows_scanned = rows_scanned;
    return result;
}

// ============================================================================
// UNION JOIN 실행 (kdb+ uj — merge columns from both tables, concatenate rows)
// ============================================================================
// kdb+ uj: union join — merge two tables with matching columns.
// Matching columns are merged; non-matching columns get JOIN_NULL for missing side.
// All rows from both tables appear in the result.
// ============================================================================
QueryResultSet QueryExecutor::exec_union_join(
    const SelectStmt& stmt,
    const std::vector<Partition*>& left_parts,
    const std::vector<Partition*>& right_parts)
{
    QueryResultSet result;
    size_t rows_scanned = 0;

    // Collect all column names from both sides (union of columns)
    std::vector<std::string> all_cols;
    std::unordered_map<std::string, size_t> col_idx;

    auto add_col = [&](const std::string& name) {
        if (col_idx.find(name) == col_idx.end()) {
            col_idx[name] = all_cols.size();
            all_cols.push_back(name);
        }
    };

    // Gather column names from left partitions
    std::vector<std::string> left_cols, right_cols;
    if (!left_parts.empty()) {
        for (const auto& cv : left_parts[0]->columns()) {
            left_cols.push_back(cv->name());
            add_col(cv->name());
        }
    }
    if (!right_parts.empty()) {
        for (const auto& cv : right_parts[0]->columns()) {
            right_cols.push_back(cv->name());
            add_col(cv->name());
        }
    }

    result.column_names = all_cols;
    result.column_types.resize(all_cols.size(), ColumnType::INT64);

    // Emit left rows
    for (auto* part : left_parts) {
        size_t n = part->num_rows();
        rows_scanned += n;
        for (size_t r = 0; r < n; ++r) {
            std::vector<int64_t> row(all_cols.size(), JOIN_NULL);
            for (const auto& c : left_cols) {
                const int64_t* d = get_col_data(*part, c);
                if (d) row[col_idx[c]] = d[r];
            }
            // symbol column
            auto sym_it = col_idx.find("symbol");
            if (sym_it != col_idx.end())
                row[sym_it->second] = static_cast<int64_t>(part->key().symbol_id);
            result.rows.push_back(std::move(row));
        }
    }

    // Emit right rows
    for (auto* part : right_parts) {
        size_t n = part->num_rows();
        rows_scanned += n;
        for (size_t r = 0; r < n; ++r) {
            std::vector<int64_t> row(all_cols.size(), JOIN_NULL);
            for (const auto& c : right_cols) {
                const int64_t* d = get_col_data(*part, c);
                if (d) row[col_idx[c]] = d[r];
            }
            auto sym_it = col_idx.find("symbol");
            if (sym_it != col_idx.end())
                row[sym_it->second] = static_cast<int64_t>(part->key().symbol_id);
            result.rows.push_back(std::move(row));
        }
    }

    result.rows_scanned = rows_scanned;
    return result;
}

// ============================================================================
// PLUS JOIN 실행 (kdb+ pj — additive join on matching keys)
// ============================================================================
// kdb+ pj: for each left row, find matching right row by key.
// Numeric columns from right are ADDED to left (not replaced).
// Non-matching left rows pass through unchanged.
// ============================================================================
QueryResultSet QueryExecutor::exec_plus_join(
    const SelectStmt& stmt,
    const std::vector<Partition*>& left_parts,
    const std::vector<Partition*>& right_parts)
{
    QueryResultSet result;
    size_t rows_scanned = 0;

    if (left_parts.empty()) return result;

    // Extract equi-join key column
    std::string l_key_col, r_key_col;
    for (const auto& cond : stmt.join->on_conditions) {
        if (cond.op == CompareOp::EQ) {
            l_key_col = cond.left_col;
            r_key_col = cond.right_col;
            break;
        }
    }
    if (l_key_col.empty()) l_key_col = "symbol";
    if (r_key_col.empty()) r_key_col = "symbol";

    // Build right-side hash map: key → row data {col_name → value}
    // For pj, we only need the additive columns (non-key columns from right)
    std::vector<std::string> r_add_cols; // right columns to add (excluding key)
    if (!right_parts.empty()) {
        for (const auto& cv : right_parts[0]->columns()) {
            if (cv->name() != r_key_col)
                r_add_cols.push_back(cv->name());
        }
    }

    // Build hash: right_key → vector of {col_values}
    std::unordered_map<int64_t, std::vector<int64_t>> right_map;
    for (auto* part : right_parts) {
        size_t n = part->num_rows();
        rows_scanned += n;
        const int64_t* rk = get_col_data(*part, r_key_col);
        if (!rk) continue;
        for (size_t r = 0; r < n; ++r) {
            std::vector<int64_t> vals;
            for (const auto& c : r_add_cols) {
                const int64_t* d = get_col_data(*part, c);
                vals.push_back(d ? d[r] : 0);
            }
            right_map[rk[r]] = std::move(vals); // last wins for duplicate keys
        }
    }

    // Result columns: left columns (values += right matching columns where names overlap)
    if (!left_parts.empty()) {
        for (const auto& cv : left_parts[0]->columns()) {
            result.column_names.push_back(cv->name());
            result.column_types.push_back(cv->type());
        }
    }

    // Map: left col name → index in r_add_cols (for additive merge)
    std::unordered_map<std::string, size_t> r_col_idx;
    for (size_t i = 0; i < r_add_cols.size(); ++i)
        r_col_idx[r_add_cols[i]] = i;

    // Emit left rows with additive merge
    for (auto* part : left_parts) {
        size_t n = part->num_rows();
        rows_scanned += n;
        const int64_t* lk = get_col_data(*part, l_key_col);
        for (size_t r = 0; r < n; ++r) {
            std::vector<int64_t> row;
            int64_t key_val = lk ? lk[r] : 0;
            auto rit = right_map.find(key_val);
            for (const auto& cv : part->columns()) {
                const int64_t* d = static_cast<const int64_t*>(cv->raw_data());
                int64_t val = d ? d[r] : 0;
                // If this column exists in right and we have a match, add
                if (rit != right_map.end()) {
                    auto ci = r_col_idx.find(cv->name());
                    if (ci != r_col_idx.end())
                        val += rit->second[ci->second];
                }
                row.push_back(val);
            }
            result.rows.push_back(std::move(row));
        }
    }

    result.rows_scanned = rows_scanned;
    return result;
}

// ============================================================================
// apply_window_functions: 결과에 윈도우 함수 컬럼 추가
// ============================================================================
// 동작:
//   1. SELECT 목록에서 window_func != NONE인 항목 찾기
//   2. 해당 컬럼의 입력 데이터를 result.rows에서 추출
//   3. WindowFunction::compute() 호출
//   4. 결과를 새 컬럼으로 result.rows에 추가
// ============================================================================
void QueryExecutor::apply_window_functions(
    const SelectStmt& stmt,
    QueryResultSet& result)
{
    size_t n = result.rows.size();
    if (n == 0) return;

    for (const auto& sel : stmt.columns) {
        if (sel.window_func == WindowFunc::NONE) continue;

        // 입력 컬럼 데이터 추출 (result.rows에서)
        // col_name으로 기존 컬럼 인덱스 찾기
        int input_col_idx = -1;
        for (size_t i = 0; i < result.column_names.size(); ++i) {
            if (result.column_names[i] == sel.column) {
                input_col_idx = static_cast<int>(i);
                break;
            }
        }

        // PARTITION BY 키 데이터 추출
        int part_col_idx = -1;
        if (sel.window_spec.has_value() && !sel.window_spec->partition_by_cols.empty()) {
            const std::string& part_col = sel.window_spec->partition_by_cols[0];
            for (size_t i = 0; i < result.column_names.size(); ++i) {
                if (result.column_names[i] == part_col) {
                    part_col_idx = static_cast<int>(i);
                    break;
                }
            }
        }

        // 입력 벡터 구성
        std::vector<int64_t> input(n, 0);
        if (input_col_idx >= 0) {
            for (size_t i = 0; i < n; ++i) {
                const auto& row = result.rows[i];
                if (static_cast<size_t>(input_col_idx) < row.size()) {
                    input[i] = row[input_col_idx];
                }
            }
        }

        // 파티션 키 벡터 구성
        std::vector<int64_t> part_keys;
        const int64_t* part_key_ptr = nullptr;
        if (part_col_idx >= 0) {
            part_keys.resize(n);
            for (size_t i = 0; i < n; ++i) {
                const auto& row = result.rows[i];
                if (static_cast<size_t>(part_col_idx) < row.size()) {
                    part_keys[i] = row[part_col_idx];
                }
            }
            part_key_ptr = part_keys.data();
        }

        // WindowFrame 구성
        WindowFrame frame;
        if (sel.window_spec.has_value() && sel.window_spec->has_frame) {
            frame.preceding = sel.window_spec->preceding;
            frame.following = sel.window_spec->following;
        }

        // WindowFunction 생성 및 compute
        std::unique_ptr<WindowFunction> wf;
        switch (sel.window_func) {
            case WindowFunc::ROW_NUMBER:  wf = std::make_unique<WindowRowNumber>(); break;
            case WindowFunc::RANK:        wf = std::make_unique<WindowRank>(); break;
            case WindowFunc::DENSE_RANK:  wf = std::make_unique<WindowDenseRank>(); break;
            case WindowFunc::SUM:         wf = std::make_unique<WindowSum>(); break;
            case WindowFunc::AVG:         wf = std::make_unique<WindowAvg>(); break;
            case WindowFunc::MIN:         wf = std::make_unique<WindowMin>(); break;
            case WindowFunc::MAX:         wf = std::make_unique<WindowMax>(); break;
            case WindowFunc::LAG:
                wf = std::make_unique<WindowLag>(sel.window_offset, sel.window_default);
                break;
            case WindowFunc::LEAD:
                wf = std::make_unique<WindowLead>(sel.window_offset, sel.window_default);
                break;
            // kdb+ 스타일 금융 윈도우 함수
            case WindowFunc::EMA: {
                double alpha = sel.ema_alpha;
                if (alpha <= 0.0 && sel.ema_period > 0) {
                    alpha = 2.0 / (sel.ema_period + 1.0);
                }
                if (alpha <= 0.0) alpha = 0.1; // 기본값
                wf = std::make_unique<WindowEMA>(alpha);
                break;
            }
            case WindowFunc::DELTA:
                wf = std::make_unique<WindowDelta>();
                break;
            case WindowFunc::RATIO:
                wf = std::make_unique<WindowRatio>();
                break;
            case WindowFunc::NONE:
                continue;
        }

        // 결과 계산
        std::vector<int64_t> output(n, 0);
        wf->compute(input.data(), n, output.data(), frame, part_key_ptr);

        // 새 컬럼으로 추가
        std::string col_name = sel.alias.empty() ? sel.column : sel.alias;
        result.column_names.push_back(col_name);
        result.column_types.push_back(ColumnType::INT64);

        // 각 행에 새 값 추가
        for (size_t i = 0; i < n; ++i) {
            result.rows[i].push_back(output[i]);
        }
    }
}

// ============================================================================
// WHERE symbol 값 추출 (파티션 필터링 최적화용)
// ============================================================================
bool QueryExecutor::has_where_symbol(
    const SelectStmt& stmt,
    int64_t& out_sym,
    const std::string& /*alias*/) const
{
    if (!stmt.where.has_value()) return false;
    auto find_sym = [&](const auto& self, const std::shared_ptr<Expr>& expr) -> bool {
        if (!expr) return false;
        if (expr->kind == Expr::Kind::COMPARE) {
            if (expr->column == "symbol" && expr->op == CompareOp::EQ) {
                if (expr->is_string) {
                    int64_t code = pipeline_.symbol_dict().find(expr->value_str);
                    if (code < 0) { out_sym = -2; return true; } // not found → empty result
                    out_sym = code;
                } else {
                    out_sym = expr->value;
                }
                return true;
            }
        }
        return self(self, expr->left) || self(self, expr->right);
    };
    return find_sym(find_sym, stmt.where->expr);
}

// Extract symbol IN (1,2,3) for multi-partition routing
bool QueryExecutor::has_where_symbol_in(
    const SelectStmt& stmt,
    std::vector<int64_t>& out_syms) const
{
    if (!stmt.where.has_value()) return false;
    auto find_in = [&](const auto& self, const std::shared_ptr<Expr>& expr) -> bool {
        if (!expr) return false;
        if (expr->kind == Expr::Kind::IN && expr->column == "symbol" && !expr->negated) {
            out_syms = expr->in_values;
            return true;
        }
        return self(self, expr->left) || self(self, expr->right);
    };
    return find_in(find_in, stmt.where->expr);
}

// ============================================================================
// WINDOW JOIN 실행 (kdb+ wj 스타일)
// ============================================================================
// 각 왼쪽 행에 대해 시간 윈도우 [t-before, t+after] 안의 오른쪽 행 집계
// SELECT 목록의 wj_agg(r.col) 함수를 처리
// ============================================================================
QueryResultSet QueryExecutor::exec_window_join(
    const SelectStmt& stmt,
    const std::vector<Partition*>& left_parts,
    const std::vector<Partition*>& right_parts)
{
    QueryResultSet result;

    if (left_parts.empty() || right_parts.empty()) return result;

    const auto& jc = stmt.join.value();

    // ON 조건에서 equi 키 추출
    std::string l_key_col = "symbol", r_key_col = "symbol";
    for (const auto& cond : jc.on_conditions) {
        if (cond.op == CompareOp::EQ) {
            l_key_col = cond.left_col;
            r_key_col = cond.right_col;
            break;
        }
    }

    // 타임스탬프 컬럼 이름
    std::string l_time_col = jc.wj_left_time_col.empty()  ? "timestamp" : jc.wj_left_time_col;
    std::string r_time_col = jc.wj_right_time_col.empty() ? "timestamp" : jc.wj_right_time_col;
    int64_t before = jc.wj_window_before;
    int64_t after  = jc.wj_window_after;

    const std::string r_alias = jc.alias;

    // 왼쪽/오른쪽 데이터를 flat 벡터로 수집
    struct RowRef { size_t part_idx; size_t local_idx; };
    std::vector<int64_t> l_key_flat, r_key_flat;
    std::vector<int64_t> l_time_flat, r_time_flat;
    std::vector<RowRef> l_refs, r_refs;

    for (size_t pi = 0; pi < left_parts.size(); ++pi) {
        auto* part = left_parts[pi];
        const int64_t* kd = get_col_data(*part, l_key_col);
        const int64_t* td = get_col_data(*part, l_time_col);
        size_t n = part->num_rows();
        for (size_t i = 0; i < n; ++i) {
            l_key_flat.push_back(kd ? kd[i] : 0);
            l_time_flat.push_back(td ? td[i] : 0);
            l_refs.push_back({pi, i});
        }
    }

    for (size_t pi = 0; pi < right_parts.size(); ++pi) {
        auto* part = right_parts[pi];
        const int64_t* kd = get_col_data(*part, r_key_col);
        const int64_t* td = get_col_data(*part, r_time_col);
        size_t n = part->num_rows();
        for (size_t i = 0; i < n; ++i) {
            r_key_flat.push_back(kd ? kd[i] : 0);
            r_time_flat.push_back(td ? td[i] : 0);
            r_refs.push_back({pi, i});
        }
    }

    size_t ln = l_key_flat.size();
    size_t rn = r_key_flat.size();

    // 결과 컬럼 이름 설정 (왼쪽 일반 컬럼 + wj_agg 컬럼)
    for (const auto& sel : stmt.columns) {
        if (sel.wj_agg != WJAggFunc::NONE) {
            std::string name = sel.alias.empty() ? sel.column : sel.alias;
            result.column_names.push_back(name);
            result.column_types.push_back(ColumnType::INT64);
        } else if (!sel.is_star) {
            std::string name = sel.alias.empty() ? sel.column : sel.alias;
            result.column_names.push_back(name);
            result.column_types.push_back(ColumnType::INT64);
        }
    }

    // 각 wj_agg 컬럼에 대해 WindowJoinOperator 실행
    // SELECT에서 wj_agg를 찾아 처리
    struct WJColInfo {
        size_t sel_idx;   // stmt.columns 인덱스
        WJAggType agg_type;
        std::vector<int64_t> r_val_flat; // 오른쪽 집계 대상 컬럼 데이터
    };
    std::vector<WJColInfo> wj_cols;

    for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
        const auto& sel = stmt.columns[ci];
        if (sel.wj_agg == WJAggFunc::NONE) continue;

        WJAggType agg_type;
        switch (sel.wj_agg) {
            case WJAggFunc::AVG:   agg_type = WJAggType::AVG;   break;
            case WJAggFunc::SUM:   agg_type = WJAggType::SUM;   break;
            case WJAggFunc::COUNT: agg_type = WJAggType::COUNT; break;
            case WJAggFunc::MIN:   agg_type = WJAggType::MIN;   break;
            case WJAggFunc::MAX:   agg_type = WJAggType::MAX;   break;
            default:               agg_type = WJAggType::AVG;   break;
        }

        // 오른쪽 테이블에서 해당 컬럼 데이터 수집
        std::vector<int64_t> r_val_flat(rn, 0);
        for (size_t ri = 0; ri < rn; ++ri) {
            auto* rp = right_parts[r_refs[ri].part_idx];
            const int64_t* vd = get_col_data(*rp, sel.column);
            r_val_flat[ri] = vd ? vd[r_refs[ri].local_idx] : 0;
        }

        wj_cols.push_back({ci, agg_type, std::move(r_val_flat)});
    }

    // 각 왼쪽 행에 대해 window join 계산
    size_t limit = stmt.limit.value_or(SIZE_MAX);
    for (size_t li = 0; li < ln && result.rows.size() < limit; ++li) {
        std::vector<int64_t> row;

        // 왼쪽 일반 컬럼 먼저
        for (const auto& sel : stmt.columns) {
            if (sel.wj_agg != WJAggFunc::NONE) continue;
            if (sel.is_star) continue;
            auto* lp = left_parts[l_refs[li].part_idx];
            const int64_t* d = get_col_data(*lp, sel.column);
            row.push_back(d ? d[l_refs[li].local_idx] : 0);
        }

        // wj_agg 컬럼 계산
        for (auto& wjc : wj_cols) {
            WindowJoinOperator wjop(wjc.agg_type, before, after);
            auto wjres = wjop.execute(
                l_key_flat.data() + li, 1,  // 단일 왼쪽 행
                r_key_flat.data(), rn,
                l_time_flat.data() + li,
                r_time_flat.data(),
                wjc.r_val_flat.data()
            );
            row.push_back(wjres.agg_values.empty() ? 0 : wjres.agg_values[0]);
        }

        result.rows.push_back(std::move(row));
    }

    result.rows_scanned = ln + rn;
    return result;
}


// ============================================================================
// 병렬 집계 구현 (GROUP BY 없음)
// ============================================================================
// 전략:
//   1. 파티션 목록을 N청크로 분할
//   2. 각 스레드가 청크 내 파티션의 부분 집계 계산
//   3. 메인 스레드가 부분 집계 머지
// ============================================================================
QueryResultSet QueryExecutor::exec_agg_parallel(
    const SelectStmt& stmt,
    const std::vector<Partition*>& partitions)
{
    using namespace zeptodb::execution;

    size_t n_threads = pool_raw_->num_threads();
    ParallelScanExecutor pse(*pool_raw_);

    // 파티션 분배 모드 결정
    size_t total_rows = estimate_total_rows(partitions);
    ParallelMode mode = ParallelScanExecutor::select_mode(
        partitions.size(), total_rows, n_threads, par_opts_.row_threshold);

    if (mode == ParallelMode::SERIAL) {
        return exec_agg(stmt, partitions);
    }

    // CHUNKED: single large partition → split into row ranges
    // PARTITION: multiple partitions → split into partition chunks
    // Both use the same worker; CHUNKED creates N copies of the single partition
    // with row_begin/row_end limits per thread.
    struct ChunkInfo {
        std::vector<Partition*> parts;
        size_t row_begin = 0;
        size_t row_end   = SIZE_MAX;
    };

    std::vector<ChunkInfo> work_items;
    if (mode == ParallelMode::CHUNKED && partitions.size() == 1) {
        auto ranges = ParallelScanExecutor::make_row_chunks(
            partitions[0]->num_rows(), n_threads);
        for (auto& [rb, re] : ranges)
            work_items.push_back({{partitions[0]}, rb, re});
    } else {
        auto chunks = ParallelScanExecutor::make_partition_chunks(partitions, n_threads);
        for (auto& c : chunks)
            work_items.push_back({std::move(c), 0, SIZE_MAX});
    }

    size_t ncols = stmt.columns.size();

    // PartialAgg: 스레드별 부분 집계 상태
    struct PartialAgg {
        std::vector<int64_t>  sum;
        std::vector<double>   d_sum;
        std::vector<int64_t>  cnt;
        std::vector<int64_t>  minv;
        std::vector<int64_t>  maxv;
        std::vector<double>   vwap_pv;
        std::vector<int64_t>  vwap_v;
        std::vector<int64_t>  first_val;
        std::vector<int64_t>  last_val;
        std::vector<bool>     has_first;
        std::vector<std::vector<int64_t>> vals; // STDDEV/VARIANCE/MEDIAN/PERCENTILE
        size_t rows_scanned = 0;
    };

    auto init_partial = [&]() -> PartialAgg {
        PartialAgg p;
        p.sum.assign(ncols, 0);
        p.d_sum.assign(ncols, 0.0);
        p.cnt.assign(ncols, 0);
        p.minv.assign(ncols, INT64_MAX);
        p.maxv.assign(ncols, INT64_MIN);
        p.vwap_pv.assign(ncols, 0.0);
        p.vwap_v.assign(ncols, 0);
        p.first_val.assign(ncols, 0);
        p.last_val.assign(ncols, 0);
        p.has_first.assign(ncols, false);
        p.vals.resize(ncols);
        return p;
    };

    auto chunks_for_par = ParallelScanExecutor::make_partition_chunks(
        partitions, n_threads);

    // For CHUNKED mode, we need row ranges per thread
    std::vector<std::pair<size_t,size_t>> row_ranges;
    bool is_chunked = (mode == ParallelMode::CHUNKED && partitions.size() == 1);
    if (is_chunked) {
        row_ranges = ParallelScanExecutor::make_row_chunks(
            partitions[0]->num_rows(), n_threads);
        // Override chunks: each thread gets the same single partition
        chunks_for_par.clear();
        for (size_t i = 0; i < row_ranges.size(); ++i)
            chunks_for_par.push_back({partitions[0]});
    }

    auto partials = pse.parallel_for_chunks<PartialAgg>(
        chunks_for_par,
        init_partial,
        [&, is_chunked](const std::vector<Partition*>& chunk, size_t tid, PartialAgg& pa) {
            for (auto* part : chunk) {
                std::vector<uint32_t> sel_indices;
                if (is_chunked && tid < row_ranges.size()) {
                    auto [rb, re] = row_ranges[tid];
                    pa.rows_scanned += re - rb;
                    sel_indices = eval_where_ranged(stmt, *part, rb, re);
                } else {
                    size_t scanned = 0;
                    sel_indices = collect_and_intersect(stmt, *part, scanned);
                    pa.rows_scanned += scanned;
                }

                for (size_t ci = 0; ci < ncols; ++ci) {
                    const auto& col = stmt.columns[ci];
                    const int64_t* data = col.arith_expr
                        ? nullptr : get_col_data(*part, col.column);
                    auto agg_val = [&](uint32_t row_idx) -> int64_t {
                        if (col.arith_expr) return eval_arith(*col.arith_expr, *part, row_idx);
                        return data ? data[row_idx] : 0;
                    };

                    switch (col.agg) {
                        case AggFunc::COUNT:
                            pa.cnt[ci] += static_cast<int64_t>(sel_indices.size());
                            break;
                        case AggFunc::SUM:
                            for (auto idx : sel_indices) pa.sum[ci] += agg_val(idx);
                            break;
                        case AggFunc::AVG:
                            for (auto idx : sel_indices) {
                                pa.d_sum[ci] += static_cast<double>(agg_val(idx));
                                pa.cnt[ci]++;
                            }
                            break;
                        case AggFunc::MIN:
                            for (auto idx : sel_indices)
                                pa.minv[ci] = std::min(pa.minv[ci], agg_val(idx));
                            break;
                        case AggFunc::MAX:
                            for (auto idx : sel_indices)
                                pa.maxv[ci] = std::max(pa.maxv[ci], agg_val(idx));
                            break;
                        case AggFunc::FIRST:
                        case AggFunc::LAST:
                            for (auto idx : sel_indices) {
                                int64_t v = agg_val(idx);
                                if (!pa.has_first[ci]) {
                                    pa.first_val[ci] = v;
                                    pa.has_first[ci] = true;
                                }
                                pa.last_val[ci] = v;
                            }
                            break;
                        case AggFunc::XBAR:
                            if (!sel_indices.empty() && !pa.has_first[ci]) {
                                int64_t v = agg_val(sel_indices[0]);
                                int64_t b = col.xbar_bucket;
                                pa.first_val[ci] = b > 0 ? (v / b) * b : v;
                                pa.has_first[ci] = true;
                            }
                            break;
                        case AggFunc::VWAP: {
                            const int64_t* vd = get_col_data(*part, col.agg_arg2);
                            if (data && vd) for (auto idx : sel_indices) {
                                pa.vwap_pv[ci] += static_cast<double>(data[idx])
                                                * static_cast<double>(vd[idx]);
                                pa.vwap_v[ci] += vd[idx];
                            }
                            break;
                        }
                        case AggFunc::STDDEV: case AggFunc::VARIANCE:
                        case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                            for (auto idx : sel_indices) pa.vals[ci].push_back(agg_val(idx)); break;
                        case AggFunc::NONE: break;
                    }
                }
            }
        }
    );

    // ── 머지 ──
    PartialAgg merged = init_partial();
    for (auto& pa : partials) {
        merged.rows_scanned += pa.rows_scanned;
        for (size_t ci = 0; ci < ncols; ++ci) {
            merged.sum[ci]     += pa.sum[ci];
            merged.d_sum[ci]   += pa.d_sum[ci];
            merged.cnt[ci]     += pa.cnt[ci];
            merged.minv[ci]     = std::min(merged.minv[ci], pa.minv[ci]);
            merged.maxv[ci]     = std::max(merged.maxv[ci], pa.maxv[ci]);
            merged.vwap_pv[ci] += pa.vwap_pv[ci];
            merged.vwap_v[ci]  += pa.vwap_v[ci];
            if (!merged.has_first[ci] && pa.has_first[ci]) {
                merged.first_val[ci] = pa.first_val[ci];
                merged.has_first[ci] = true;
            }
            if (pa.has_first[ci]) {
                merged.last_val[ci] = pa.last_val[ci];
            }
            merged.vals[ci].insert(merged.vals[ci].end(),
                pa.vals[ci].begin(), pa.vals[ci].end());
        }
    }

    // ── 결과 조립 ──
    QueryResultSet result;
    std::vector<int64_t> row(ncols);
    for (size_t ci = 0; ci < ncols; ++ci) {
        const auto& col = stmt.columns[ci];
        std::string name = col.alias.empty()
            ? (col.arith_expr ? "expr" : (col.column.empty() ? "*" : col.column))
            : col.alias;
        result.column_names.push_back(name);
        result.column_types.push_back(ColumnType::INT64);

        switch (col.agg) {
            case AggFunc::COUNT: row[ci] = merged.cnt[ci]; break;
            case AggFunc::SUM:   row[ci] = merged.sum[ci]; break;
            case AggFunc::AVG:
                row[ci] = merged.cnt[ci] > 0
                    ? static_cast<int64_t>(merged.d_sum[ci] / merged.cnt[ci]) : INT64_MIN;
                break;
            case AggFunc::MIN:
                row[ci] = (merged.minv[ci] == INT64_MAX) ? INT64_MIN : merged.minv[ci];
                break;
            case AggFunc::MAX:
                row[ci] = (merged.maxv[ci] == INT64_MIN) ? INT64_MIN : merged.maxv[ci];
                break;
            case AggFunc::VWAP:
                row[ci] = merged.vwap_v[ci] > 0
                    ? static_cast<int64_t>(merged.vwap_pv[ci] / merged.vwap_v[ci]) : INT64_MIN;
                break;
            case AggFunc::FIRST: row[ci] = merged.first_val[ci]; break;
            case AggFunc::LAST:  row[ci] = merged.last_val[ci];  break;
            case AggFunc::XBAR:  row[ci] = merged.first_val[ci]; break;
            case AggFunc::NONE:  row[ci] = 0; break;
        }
    }

    result.rows.push_back(std::move(row));
    result.rows_scanned = merged.rows_scanned;
    return result;
}

// ============================================================================
// 병렬 GROUP BY 집계
// ============================================================================
// 전략:
//   1. 파티션을 N청크로 분배
//   2. 각 스레드가 청크에 대해 로컬 group map 생성
//   3. 메인 스레드가 group map 머지
// ============================================================================
QueryResultSet QueryExecutor::exec_group_agg_parallel(
    const SelectStmt& stmt,
    const std::vector<Partition*>& partitions)
{
    using namespace zeptodb::execution;

    size_t n_threads = pool_raw_->num_threads();
    ParallelScanExecutor pse(*pool_raw_);

    size_t total_rows = estimate_total_rows(partitions);
    ParallelMode mode = ParallelScanExecutor::select_mode(
        partitions.size(), total_rows, n_threads, par_opts_.row_threshold);

    if (mode == ParallelMode::SERIAL) {
        return exec_group_agg(stmt, partitions);
    }

    const auto& gb = stmt.group_by.value();

    size_t ncols = stmt.columns.size();

    struct GroupState {
        int64_t  sum     = 0;
        int64_t  count   = 0;
        double   avg_sum = 0.0;
        int64_t  minv    = INT64_MAX;
        int64_t  maxv    = INT64_MIN;
        double   vwap_pv = 0.0;
        int64_t  vwap_v  = 0;
        int64_t  first_val = 0;
        int64_t  last_val  = 0;
        bool     has_first = false;
        std::vector<int64_t> vals; // STDDEV/VARIANCE/MEDIAN/PERCENTILE
    };

    // ─────────────────────────────────────────────────────────────────────
    // Parallel optimized path: single-column GROUP BY.
    // Uses flat int64_t key per row — zero heap alloc per row.
    // ─────────────────────────────────────────────────────────────────────
    if (gb.columns.size() == 1) {
        const std::string& group_col_p  = gb.columns[0];
        const int64_t      bucket_p     = gb.xbar_buckets.empty() ? 0 : gb.xbar_buckets[0];

        // Flat GroupState per thread: key→slot + contiguous array.
        // Eliminates per-group vector<GroupState> heap allocs in each thread.
        struct PartialGroupScalar {
            std::unordered_map<int64_t, uint32_t> key_to_slot;
            std::vector<GroupState> flat_states;  // ncols * num_groups
            size_t rows_scanned = 0;
        };

        auto chunks = ParallelScanExecutor::make_partition_chunks(
            partitions,
            (mode == ParallelMode::PARTITION) ? n_threads : 1);

        auto partials = pse.parallel_for_chunks<PartialGroupScalar>(
            chunks,
            []() -> PartialGroupScalar { return {}; },
            [&](const std::vector<Partition*>& chunk, size_t /*tid*/, PartialGroupScalar& pg) {
                pg.key_to_slot.reserve(4096);
                pg.flat_states.reserve(4096 * ncols);
                uint32_t pg_next_slot = 0;
                int64_t  pg_cached_key  = INT64_MIN;
                uint32_t pg_cached_slot = 0;
                for (auto* part : chunk) {
                    std::vector<uint32_t> sel_indices;
                    {
                        size_t scanned = 0;
                        sel_indices = collect_and_intersect(stmt, *part, scanned);
                        pg.rows_scanned += scanned;
                    }

                    // Hoist key column + aggregate column pointers to partition scope
                    const int64_t* gkey_col = (group_col_p == "symbol")
                        ? nullptr : get_col_data(*part, group_col_p);
                    const int64_t sym_kv = static_cast<int64_t>(part->key().symbol_id);

                    std::vector<const int64_t*> col_ptrs(ncols, nullptr);
                    std::vector<const int64_t*> vwap_ptrs(ncols, nullptr);
                    for (size_t ci = 0; ci < ncols; ++ci) {
                        const auto& col = stmt.columns[ci];
                        if (col.agg == AggFunc::NONE || col.arith_expr) continue;
                        col_ptrs[ci]  = get_col_data(*part, col.column);
                        if (col.agg == AggFunc::VWAP)
                            vwap_ptrs[ci] = get_col_data(*part, col.agg_arg2);
                    }

                    for (auto idx : sel_indices) {
                        int64_t kv = gkey_col ? gkey_col[idx] : sym_kv;
                        if (bucket_p > 0) kv = (kv / bucket_p) * bucket_p;

                        if (__builtin_expect(kv != pg_cached_key, 0)) {
                            auto it = pg.key_to_slot.find(kv);
                            if (__builtin_expect(it == pg.key_to_slot.end(), 0)) {
                                it = pg.key_to_slot.emplace(kv, pg_next_slot++).first;
                                pg.flat_states.resize(pg.flat_states.size() + ncols);
                            }
                            pg_cached_key  = kv;
                            pg_cached_slot = it->second;
                        }
                        GroupState* states = pg.flat_states.data() + pg_cached_slot * ncols;

                        for (size_t ci = 0; ci < ncols; ++ci) {
                            const auto& col = stmt.columns[ci];
                            if (col.agg == AggFunc::NONE) continue;
                            auto& gs = states[ci];
                            const int64_t* data = col_ptrs[ci];
                            auto agg_v = [&]() -> int64_t {
                                if (col.case_when) return eval_case_when(*col.case_when, *part, idx);
                                if (col.arith_expr) return eval_arith(*col.arith_expr, *part, idx);
                                return data ? data[idx] : 0;
                            };
                            switch (col.agg) {
                                case AggFunc::COUNT: gs.count++; break;
                                case AggFunc::SUM:   gs.sum += agg_v(); break;
                                case AggFunc::AVG:   gs.avg_sum += agg_v(); gs.count++; break;
                                case AggFunc::MIN:   gs.minv = std::min(gs.minv, agg_v()); break;
                                case AggFunc::MAX:   gs.maxv = std::max(gs.maxv, agg_v()); break;
                                case AggFunc::FIRST:
                                case AggFunc::LAST: {
                                    int64_t v = agg_v();
                                    if (!gs.has_first) { gs.first_val = v; gs.has_first = true; }
                                    gs.last_val = v;
                                    break;
                                }
                                case AggFunc::XBAR:
                                    if (!gs.has_first) {
                                        int64_t v = data ? data[idx] : 0;
                                        int64_t b = col.xbar_bucket;
                                        gs.first_val = b > 0 ? (v / b) * b : v;
                                        gs.has_first = true;
                                    }
                                    break;
                                case AggFunc::VWAP: {
                                    const int64_t* vd = vwap_ptrs[ci];
                                    if (data && vd) {
                                        gs.vwap_pv += static_cast<double>(data[idx])
                                                    * static_cast<double>(vd[idx]);
                                        gs.vwap_v  += vd[idx];
                                    }
                                    break;
                                }
                                case AggFunc::NONE: break;
                                case AggFunc::STDDEV: case AggFunc::VARIANCE:
                                case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                                    gs.vals.push_back(agg_v()); break;
                            }
                        }
                    }
                }
            }
        );

        // Merge partial flat maps into single flat structure
        std::unordered_map<int64_t, uint32_t> merged_key_to_slot;
        std::vector<GroupState> merged_flat;
        size_t rows_scanned = 0;
        merged_key_to_slot.reserve(4096);
        merged_flat.reserve(4096 * ncols);
        uint32_t merged_next_slot = 0;
        for (auto& pg : partials) {
            rows_scanned += pg.rows_scanned;
            for (auto& [gk, src_slot] : pg.key_to_slot) {
                auto mit = merged_key_to_slot.find(gk);
                bool inserted = (mit == merged_key_to_slot.end());
                if (inserted) {
                    mit = merged_key_to_slot.emplace(gk, merged_next_slot++).first;
                    merged_flat.resize(merged_flat.size() + ncols);
                }
                GroupState* dst = merged_flat.data() + mit->second * ncols;
                const GroupState* src = pg.flat_states.data() + src_slot * ncols;
                if (inserted) {
                    // First occurrence — copy directly
                    std::copy(src, src + ncols, dst);
                    continue;
                }
                for (size_t ci = 0; ci < ncols; ++ci) {
                    const auto& col = stmt.columns[ci];
                    if (col.agg == AggFunc::NONE) continue;
                    auto& d = dst[ci];
                    const auto& s = src[ci];
                    switch (col.agg) {
                        case AggFunc::COUNT: d.count   += s.count; break;
                        case AggFunc::SUM:   d.sum     += s.sum; break;
                        case AggFunc::AVG:   d.avg_sum += s.avg_sum; d.count += s.count; break;
                        case AggFunc::MIN:   d.minv     = std::min(d.minv, s.minv); break;
                        case AggFunc::MAX:   d.maxv     = std::max(d.maxv, s.maxv); break;
                        case AggFunc::VWAP:  d.vwap_pv += s.vwap_pv; d.vwap_v += s.vwap_v; break;
                        case AggFunc::FIRST:
                        case AggFunc::LAST:
                            if (!d.has_first && s.has_first) { d.first_val = s.first_val; d.has_first = true; }
                            if (s.has_first) d.last_val = s.last_val;
                            break;
                        case AggFunc::XBAR:
                            if (!d.has_first && s.has_first) { d.first_val = s.first_val; d.has_first = true; }
                            break;
                        case AggFunc::NONE: break;
                        case AggFunc::STDDEV: case AggFunc::VARIANCE:
                        case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                            d.vals.insert(d.vals.end(), s.vals.begin(), s.vals.end()); break;
                    }
                }
            }
        }

        // Assemble result
        QueryResultSet result;
        result.column_names.push_back(group_col_p);
        result.column_types.push_back(ColumnType::INT64);
        for (size_t ci = 0; ci < ncols; ++ci) {
            const auto& col = stmt.columns[ci];
            if (col.agg == AggFunc::NONE) continue;
            std::string name = col.alias.empty()
                ? (col.arith_expr ? "expr" : (col.column.empty() ? "*" : col.column))
                : col.alias;
            result.column_names.push_back(name);
            result.column_types.push_back(ColumnType::INT64);
        }
        for (auto& [gk_scalar, slot] : merged_key_to_slot) {
            std::vector<int64_t> row;
            row.push_back(gk_scalar);
            const GroupState* states = merged_flat.data() + slot * ncols;
            for (size_t ci = 0; ci < ncols; ++ci) {
                const auto& col = stmt.columns[ci];
                if (col.agg == AggFunc::NONE) continue;
                const auto& gs = states[ci];
                switch (col.agg) {
                    case AggFunc::COUNT: row.push_back(gs.count); break;
                    case AggFunc::SUM:   row.push_back(gs.sum); break;
                    case AggFunc::AVG:   row.push_back(gs.count > 0 ? static_cast<int64_t>(gs.avg_sum / gs.count) : INT64_MIN); break;
                    case AggFunc::MIN:   row.push_back(gs.minv == INT64_MAX ? INT64_MIN : gs.minv); break;
                    case AggFunc::MAX:   row.push_back(gs.maxv == INT64_MIN ? INT64_MIN : gs.maxv); break;
                    case AggFunc::VWAP:  row.push_back(gs.vwap_v > 0 ? static_cast<int64_t>(gs.vwap_pv / gs.vwap_v) : INT64_MIN); break;
                    case AggFunc::FIRST: row.push_back(gs.first_val); break;
                    case AggFunc::LAST:  row.push_back(gs.last_val); break;
                    case AggFunc::XBAR:  row.push_back(gs.first_val); break;
                    case AggFunc::NONE: break;
                    case AggFunc::STDDEV: case AggFunc::VARIANCE:
                    case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                        row.push_back(compute_stats(col.agg, gs.vals, col.percentile_value)); break;
                }
            }
            result.rows.push_back(std::move(row));
        }
        result.rows_scanned = rows_scanned;
        if (stmt.having.has_value())
            result = apply_having_filter(std::move(result), stmt.having.value());
        apply_order_by(result, stmt);
        if (!stmt.order_by.has_value() &&
            stmt.limit.has_value() && result.rows.size() > (size_t)stmt.limit.value())
            result.rows.resize(stmt.limit.value());
        return result;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Multi-column parallel path: composite vector<int64_t> key.
    // ─────────────────────────────────────────────────────────────────────
    using GroupMap = std::unordered_map<std::vector<int64_t>, std::vector<GroupState>, VectorHash>;

    struct PartialGroup {
        GroupMap map;
        size_t rows_scanned = 0;
    };

    auto init_partial = []() -> PartialGroup { return {}; };

    auto chunks = ParallelScanExecutor::make_partition_chunks(
        partitions,
        (mode == ParallelMode::PARTITION) ? n_threads : 1);

    auto partials = pse.parallel_for_chunks<PartialGroup>(
        chunks,
        init_partial,
        [&](const std::vector<Partition*>& chunk, size_t /*tid*/, PartialGroup& pg) {
            pg.map.reserve(1024);
            for (auto* part : chunk) {
                std::vector<uint32_t> sel_indices;

                {
                    size_t scanned = 0;
                    sel_indices = collect_and_intersect(stmt, *part, scanned);
                    pg.rows_scanned += scanned;
                }

                for (auto idx : sel_indices) {
                    // Build composite group key
                    std::vector<int64_t> gkey;
                    gkey.reserve(gb.columns.size());
                    for (size_t gi = 0; gi < gb.columns.size(); ++gi) {
                        const std::string& gcol  = gb.columns[gi];
                        int64_t            bucket = gb.xbar_buckets[gi];
                        int64_t kv;
                        if (gcol == "symbol") {
                            kv = static_cast<int64_t>(part->key().symbol_id);
                        } else {
                            const int64_t* gdata = get_col_data(*part, gcol);
                            kv = gdata ? gdata[idx] : 0;
                        }
                        if (bucket > 0) kv = (kv / bucket) * bucket;
                        gkey.push_back(kv);
                    }

                    auto& states = pg.map[gkey];
                    if (states.empty()) states.resize(ncols);

                    for (size_t ci = 0; ci < ncols; ++ci) {
                        const auto& col = stmt.columns[ci];
                        if (col.agg == AggFunc::NONE) continue;
                        auto& gs = states[ci];
                        const int64_t* data = (col.arith_expr || col.case_when)
                            ? nullptr : get_col_data(*part, col.column);
                        auto agg_v = [&]() -> int64_t {
                            if (col.case_when) return eval_case_when(*col.case_when, *part, idx);
                            if (col.arith_expr) return eval_arith(*col.arith_expr, *part, idx);
                            return data ? data[idx] : 0;
                        };

                        switch (col.agg) {
                            case AggFunc::COUNT: gs.count++; break;
                            case AggFunc::SUM:   gs.sum += agg_v(); break;
                            case AggFunc::AVG:   gs.avg_sum += agg_v(); gs.count++; break;
                            case AggFunc::MIN:   gs.minv = std::min(gs.minv, agg_v()); break;
                            case AggFunc::MAX:   gs.maxv = std::max(gs.maxv, agg_v()); break;
                            case AggFunc::FIRST:
                            case AggFunc::LAST: {
                                int64_t v = agg_v();
                                if (!gs.has_first) { gs.first_val = v; gs.has_first = true; }
                                gs.last_val = v;
                                break;
                            }
                            case AggFunc::XBAR:
                                if (!gs.has_first) {
                                    int64_t v = agg_v();
                                    int64_t b = col.xbar_bucket;
                                    gs.first_val = b > 0 ? (v / b) * b : v;
                                    gs.has_first = true;
                                }
                                break;
                            case AggFunc::VWAP: {
                                const int64_t* vd = get_col_data(*part, col.agg_arg2);
                                if (!col.arith_expr && data && vd) {
                                    gs.vwap_pv += static_cast<double>(data[idx])
                                                * static_cast<double>(vd[idx]);
                                    gs.vwap_v  += vd[idx];
                                }
                                break;
                            }
                            case AggFunc::NONE: break;
                        }
                    }
                }
            }
        }
    );

    // ── 로컬 GroupMap 머지 ──
    GroupMap merged;
    size_t rows_scanned = 0;
    merged.reserve(1024);

    for (auto& pg : partials) {
        rows_scanned += pg.rows_scanned;
        for (auto& [gkey, src_states] : pg.map) {
            auto& dst_states = merged[gkey];
            if (dst_states.empty()) {
                dst_states = src_states;
                continue;
            }
            for (size_t ci = 0; ci < ncols; ++ci) {
                const auto& col = stmt.columns[ci];
                if (col.agg == AggFunc::NONE) continue;
                auto& dst = dst_states[ci];
                const auto& src = src_states[ci];
                switch (col.agg) {
                    case AggFunc::COUNT: dst.count   += src.count; break;
                    case AggFunc::SUM:   dst.sum     += src.sum;   break;
                    case AggFunc::AVG:   dst.avg_sum += src.avg_sum; dst.count += src.count; break;
                    case AggFunc::MIN:   dst.minv     = std::min(dst.minv, src.minv); break;
                    case AggFunc::MAX:   dst.maxv     = std::max(dst.maxv, src.maxv); break;
                    case AggFunc::VWAP:  dst.vwap_pv += src.vwap_pv; dst.vwap_v += src.vwap_v; break;
                    case AggFunc::FIRST:
                    case AggFunc::LAST:
                        if (!dst.has_first && src.has_first) {
                            dst.first_val = src.first_val;
                            dst.has_first = true;
                        }
                        if (src.has_first) dst.last_val = src.last_val;
                        break;
                    case AggFunc::XBAR:
                        if (!dst.has_first && src.has_first) {
                            dst.first_val = src.first_val;
                            dst.has_first = true;
                        }
                        break;
                    case AggFunc::NONE: break;
                    case AggFunc::STDDEV: case AggFunc::VARIANCE:
                    case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                        dst.vals.insert(dst.vals.end(), src.vals.begin(), src.vals.end()); break;
                }
            }
        }
    }

    // ── 결과 조립 ──
    QueryResultSet result;
    for (const auto& gcol : gb.columns) {
        result.column_names.push_back(gcol);
        result.column_types.push_back(ColumnType::INT64);
    }
    for (size_t ci = 0; ci < ncols; ++ci) {
        const auto& col = stmt.columns[ci];
        if (col.agg == AggFunc::NONE) continue;
        std::string name = col.alias.empty()
            ? (col.arith_expr ? "expr" : (col.column.empty() ? "*" : col.column))
            : col.alias;
        result.column_names.push_back(name);
        result.column_types.push_back(ColumnType::INT64);
    }

    for (auto& [gkey_vec, states] : merged) {
        std::vector<int64_t> row;
        for (int64_t k : gkey_vec) row.push_back(k);
        for (size_t ci = 0; ci < ncols; ++ci) {
            const auto& col = stmt.columns[ci];
            if (col.agg == AggFunc::NONE) continue;
            const auto& gs = states[ci];
            switch (col.agg) {
                case AggFunc::COUNT: row.push_back(gs.count); break;
                case AggFunc::SUM:   row.push_back(gs.sum);   break;
                case AggFunc::AVG:
                    row.push_back(gs.count > 0
                        ? static_cast<int64_t>(gs.avg_sum / gs.count) : 0); break;
                case AggFunc::MIN:
                    row.push_back(gs.minv == INT64_MAX ? INT64_MIN : gs.minv); break;
                case AggFunc::MAX:
                    row.push_back(gs.maxv == INT64_MIN ? INT64_MIN : gs.maxv); break;
                case AggFunc::VWAP:
                    row.push_back(gs.vwap_v > 0
                        ? static_cast<int64_t>(gs.vwap_pv / gs.vwap_v) : INT64_MIN); break;
                case AggFunc::FIRST: row.push_back(gs.first_val); break;
                case AggFunc::LAST:  row.push_back(gs.last_val);  break;
                case AggFunc::XBAR:  row.push_back(gs.first_val); break;
                case AggFunc::NONE: break;
                case AggFunc::STDDEV: case AggFunc::VARIANCE:
                case AggFunc::MEDIAN: case AggFunc::PERCENTILE:
                    row.push_back(compute_stats(col.agg, gs.vals, col.percentile_value)); break;
            }
        }
        result.rows.push_back(std::move(row));
    }

    result.rows_scanned = rows_scanned;

    // HAVING 필터 (병렬 집계 결과에 적용)
    if (stmt.having.has_value())
        result = apply_having_filter(std::move(result), stmt.having.value());

    apply_order_by(result, stmt);
    if (!stmt.order_by.has_value() &&
        stmt.limit.has_value() && result.rows.size() > (size_t)stmt.limit.value()) {
        result.rows.resize(stmt.limit.value());
    }

    return result;
}

// ============================================================================
// exec_simple_select_parallel — partition-parallel SELECT
// ============================================================================
QueryResultSet QueryExecutor::exec_simple_select_parallel(
    const SelectStmt& stmt,
    const std::vector<Partition*>& partitions)
{
    using namespace zeptodb::execution;

    size_t n_threads = pool_raw_->num_threads();
    ParallelScanExecutor pse(*pool_raw_);

    size_t total_rows = estimate_total_rows(partitions);
    ParallelMode mode = ParallelScanExecutor::select_mode(
        partitions.size(), total_rows, n_threads, par_opts_.row_threshold);

    std::vector<std::pair<size_t,size_t>> row_ranges;
    bool is_chunked = (mode == ParallelMode::CHUNKED && partitions.size() == 1);

    auto chunks = ParallelScanExecutor::make_partition_chunks(partitions, n_threads);
    if (is_chunked) {
        row_ranges = ParallelScanExecutor::make_row_chunks(
            partitions[0]->num_rows(), n_threads);
        chunks.clear();
        for (size_t i = 0; i < row_ranges.size(); ++i)
            chunks.push_back({partitions[0]});
    }

    bool is_star = stmt.columns.size() == 1 && stmt.columns[0].is_star;

    auto worker = [&, is_chunked](const std::vector<Partition*>& chunk,
                      size_t tid, QueryResultSet& out) {
        size_t limit = stmt.order_by.has_value() ? SIZE_MAX
                     : stmt.limit.value_or(SIZE_MAX);

        for (auto* part : chunk) {
            std::vector<uint32_t> sel;
            if (is_chunked && tid < row_ranges.size()) {
                auto [rb, re] = row_ranges[tid];
                out.rows_scanned += re - rb;
                sel = eval_where_ranged(stmt, *part, rb, re);
            } else {
                size_t scanned = 0;
                sel = collect_and_intersect(stmt, *part, scanned);
                out.rows_scanned += scanned;
            }

            for (uint32_t idx : sel) {
                if (out.rows.size() >= limit) break;
                std::vector<int64_t> row;
                if (is_star) {
                    for (const auto& cv : part->columns()) {
                        const int64_t* d = static_cast<const int64_t*>(cv->raw_data());
                        row.push_back(d ? d[idx] : 0);
                    }
                } else {
                    for (const auto& col : stmt.columns) {
                        if (col.window_func != WindowFunc::NONE) continue;
                        int64_t val;
                        if (col.case_when) {
                            val = eval_case_when(*col.case_when, *part, idx);
                        } else if (col.arith_expr) {
                            val = eval_arith(*col.arith_expr, *part, idx);
                        } else if (col.column == "symbol") {
                            val = static_cast<int64_t>(part->key().symbol_id);
                        } else {
                            const int64_t* d = get_col_data(*part, col.column);
                            val = d ? d[idx] : 0;
                        }
                        row.push_back(val);
                    }
                }
                out.rows.push_back(std::move(row));
            }
        }
    };

    auto init = [&]() -> QueryResultSet {
        QueryResultSet r;
        // Set up column metadata from first partition
        if (!partitions.empty()) {
            auto* part = partitions[0];
            if (is_star) {
                for (const auto& cv : part->columns()) {
                    r.column_names.push_back(cv->name());
                    r.column_types.push_back(cv->type());
                }
            } else {
                for (const auto& sel : stmt.columns) {
                    if (sel.window_func != WindowFunc::NONE) continue;
                    r.column_names.push_back(
                        sel.alias.empty() ? sel.column : sel.alias);
                    r.column_types.push_back(ColumnType::INT64);
                }
            }
        }
        return r;
    };

    auto partials = pse.parallel_for_chunks<QueryResultSet>(chunks, init, worker);

    // Merge: concat all partial results
    QueryResultSet result = init();
    for (auto& p : partials) {
        result.rows.insert(result.rows.end(),
                           std::make_move_iterator(p.rows.begin()),
                           std::make_move_iterator(p.rows.end()));
        result.rows_scanned += p.rows_scanned;
    }

    apply_order_by(result, stmt);
    if (!stmt.order_by.has_value() &&
        stmt.limit.has_value() && result.rows.size() > (size_t)stmt.limit.value()) {
        result.rows.resize(stmt.limit.value());
    }

    return result;
}

// ============================================================================
// Prepared statement cache
// ============================================================================
size_t QueryExecutor::sql_hash(const std::string& sql) {
    return std::hash<std::string>{}(sql);
}

size_t QueryExecutor::prepared_cache_size() const {
    std::lock_guard<std::mutex> lk(stmt_cache_mu_);
    return stmt_cache_.size();
}

void QueryExecutor::clear_prepared_cache() {
    std::lock_guard<std::mutex> lk(stmt_cache_mu_);
    stmt_cache_.clear();
}

// ============================================================================
// Query result cache
// ============================================================================
void QueryExecutor::enable_result_cache(size_t max_entries, double ttl_seconds) {
    std::lock_guard<std::mutex> lk(result_cache_mu_);
    result_cache_max_   = max_entries;
    result_cache_ttl_s_ = ttl_seconds;
}

void QueryExecutor::disable_result_cache() {
    std::lock_guard<std::mutex> lk(result_cache_mu_);
    result_cache_max_ = 0;
    result_cache_.clear();
}

void QueryExecutor::invalidate_result_cache(const std::string& /*table*/) {
    // Simple: clear all cached results on any write.
    // Future: selective invalidation by table name.
    std::lock_guard<std::mutex> lk(result_cache_mu_);
    result_cache_.clear();
}

size_t QueryExecutor::result_cache_size() const {
    std::lock_guard<std::mutex> lk(result_cache_mu_);
    return result_cache_.size();
}

// ============================================================================
// DuckDB Offload Methods
// ============================================================================
#ifdef ZEPTO_ENABLE_DUCKDB

zeptodb::execution::DuckDBEngine& QueryExecutor::get_duckdb_engine() {
    std::lock_guard<std::mutex> lk(duckdb_mu_);
    if (!duckdb_engine_) {
        execution::DuckDBConfig cfg;
        cfg.memory_limit_mb = duckdb_memory_limit_mb_;
        duckdb_engine_ = std::make_unique<execution::DuckDBEngine>(cfg);
    }
    return *duckdb_engine_;
}

QueryResultSet QueryExecutor::exec_via_duckdb(
    const std::string& sql,
    const std::vector<std::string>& parquet_paths)
{
    // Hold mutex for entire call to prevent concurrent view name collisions
    std::lock_guard<std::mutex> lk(duckdb_mu_);

    if (!duckdb_engine_) {
        execution::DuckDBConfig cfg;
        cfg.memory_limit_mb = duckdb_memory_limit_mb_;
        duckdb_engine_ = std::make_unique<execution::DuckDBEngine>(cfg);
    }
    auto& engine = *duckdb_engine_;

    // Register parquet files as views (paths already validated by caller)
    for (size_t i = 0; i < parquet_paths.size(); ++i) {
        std::string name = "pq_" + std::to_string(i);
        engine.register_parquet(name, parquet_paths[i]);
    }

    auto duckdb_result = engine.execute(sql);

    QueryResultSet result;
    if (!duckdb_result.ok()) {
        result.error = duckdb_result.error;
        return result;
    }

    // Convert materialized DuckDB data to ArrowColumnData, then to QueryResultSet
    std::vector<execution::ArrowColumnData> columns;
    columns.reserve(duckdb_result.num_columns);
    for (size_t c = 0; c < duckdb_result.num_columns; ++c) {
        execution::ArrowColumnData col;
        col.name = duckdb_result.column_names[c];
        switch (duckdb_result.column_type_hints[c]) {
            case 1:
                col.type = storage::ColumnType::FLOAT64;
                col.dbl_values = std::move(duckdb_result.dbl_columns[c]);
                break;
            case 2:
                col.type = storage::ColumnType::SYMBOL;
                col.str_values = std::move(duckdb_result.str_columns[c]);
                break;
            default:
                col.type = storage::ColumnType::INT64;
                col.int_values = std::move(duckdb_result.int_columns[c]);
                break;
        }
        columns.push_back(std::move(col));
    }

    execution::arrow_columns_to_result(columns, duckdb_result.num_rows, result);
    return result;
}

bool QueryExecutor::is_duckdb_table_func(const std::string& table_name,
                                          std::string& out_path) {
    // Match pattern: duckdb('path/to/file.parquet')
    const std::string prefix = "duckdb('";
    if (table_name.size() > prefix.size() + 2 &&
        table_name.starts_with(prefix) &&
        table_name.back() == ')' &&
        table_name[table_name.size() - 2] == '\'') {
        out_path = table_name.substr(prefix.size(),
                                     table_name.size() - prefix.size() - 2);
        return true;
    }
    return false;
}

#endif // ZEPTO_ENABLE_DUCKDB

} // namespace zeptodb::sql
