// ============================================================================
// APEX-DB: SQL Query Executor Implementation
// ============================================================================
// SelectStmt AST를 ApexPipeline API로 변환 실행
// ============================================================================

#include "apex/sql/executor.h"
#include "apex/sql/parser.h"
#include "apex/execution/join_operator.h"
#include "apex/execution/window_function.h"
#include "apex/execution/vectorized_engine.h"
#include "apex/execution/parallel_scan.h"
#include "apex/execution/local_scheduler.h"
#include "apex/common/logger.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>
#include <stdexcept>

namespace apex::sql {

// Thread-local cancellation token — set before execute(), cleared after.
// Checked at partition scan boundaries without changing any inner function signatures.
static thread_local apex::auth::CancellationToken* tl_cancel_token = nullptr;

// Helper: returns true if the current query should be aborted
static inline bool is_cancelled() {
    return tl_cancel_token && tl_cancel_token->is_cancelled();
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

using namespace apex::core;
using namespace apex::storage;
using namespace apex::execution;

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
QueryExecutor::QueryExecutor(ApexPipeline& pipeline)
    : pipeline_(pipeline)
{
    // 기본: LocalQueryScheduler (hardware_concurrency 스레드)
    auto local = std::make_unique<apex::execution::LocalQueryScheduler>(pipeline);
    pool_raw_  = &local->pool();
    scheduler_ = std::move(local);
}

QueryExecutor::QueryExecutor(ApexPipeline& pipeline,
                             std::unique_ptr<apex::execution::QueryScheduler> sched)
    : pipeline_(pipeline)
    , scheduler_(std::move(sched))
    , pool_raw_(nullptr)  // 비-로컬 스케줄러: 직렬 폴백
{}

void QueryExecutor::enable_parallel(size_t num_threads, size_t row_threshold) {
    par_opts_.enabled      = true;
    par_opts_.num_threads  = num_threads;
    par_opts_.row_threshold = row_threshold;
    // 지정된 스레드 수로 LocalQueryScheduler 재생성
    auto local = std::make_unique<apex::execution::LocalQueryScheduler>(
        pipeline_, num_threads);
    pool_raw_  = &local->pool();
    scheduler_ = std::move(local);
}

void QueryExecutor::disable_parallel() {
    par_opts_.enabled = false;
    // 단일 스레드 스케줄러로 교체
    auto local = std::make_unique<apex::execution::LocalQueryScheduler>(
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
QueryResultSet QueryExecutor::execute(const std::string& sql) {
    double t0 = now_us();
    try {
        Parser parser;
        SelectStmt stmt = parser.parse(sql);
        auto result = exec_select(stmt);
        result.execution_time_us = now_us() - t0;
        return result;
    } catch (const std::exception& e) {
        QueryResultSet err;
        err.error = e.what();
        err.execution_time_us = now_us() - t0;
        return err;
    }
}

QueryResultSet QueryExecutor::execute(const std::string& sql,
                                       apex::auth::CancellationToken* token)
{
    tl_cancel_token = token;
    auto result = execute(sql);
    tl_cancel_token = nullptr;
    return result;
}

// ============================================================================
// SELECT 실행 디스패처
// ============================================================================
QueryResultSet QueryExecutor::exec_select(const SelectStmt& stmt) {
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
            static_cast<apex::SymbolId>(sym_filter));

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

        // Hash JOIN (INNER / LEFT)
        if (stmt.join.has_value() && (stmt.join->type == JoinClause::Type::INNER
                                   || stmt.join->type == JoinClause::Type::LEFT)) {
            auto right_parts = find_partitions(stmt.join->table);
            return exec_hash_join(stmt, left_parts, right_parts);
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

    // Hash JOIN (INNER / LEFT)
    if (stmt.join.has_value() && (stmt.join->type == JoinClause::Type::INNER
                               || stmt.join->type == JoinClause::Type::LEFT)) {
        auto right_parts = find_partitions(stmt.join->table);
        return exec_hash_join(stmt, left_parts, right_parts);
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
            const int64_t* data = get_col_data(part, expr->column);
            if (!data) return {};
            int64_t val = expr->value;
            std::vector<uint32_t> result;
            result.reserve(num_rows / 4);
            switch (expr->op) {
                case CompareOp::EQ:
                    for (size_t i = 0; i < num_rows; ++i)
                        if (data[i] == val) result.push_back(static_cast<uint32_t>(i));
                    break;
                case CompareOp::NE:
                    for (size_t i = 0; i < num_rows; ++i)
                        if (data[i] != val) result.push_back(static_cast<uint32_t>(i));
                    break;
                case CompareOp::GT:
                    for (size_t i = 0; i < num_rows; ++i)
                        if (data[i] > val) result.push_back(static_cast<uint32_t>(i));
                    break;
                case CompareOp::LT:
                    for (size_t i = 0; i < num_rows; ++i)
                        if (data[i] < val) result.push_back(static_cast<uint32_t>(i));
                    break;
                case CompareOp::GE:
                    for (size_t i = 0; i < num_rows; ++i)
                        if (data[i] >= val) result.push_back(static_cast<uint32_t>(i));
                    break;
                case CompareOp::LE:
                    for (size_t i = 0; i < num_rows; ++i)
                        if (data[i] <= val) result.push_back(static_cast<uint32_t>(i));
                    break;
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
            const int64_t* data = get_col_data(part, expr->column);
            if (!data) return {};
            const auto& vals = expr->in_values;
            std::vector<uint32_t> result;
            result.reserve(num_rows / 4);
            for (size_t i = 0; i < num_rows; ++i) {
                for (int64_t v : vals) {
                    if (data[i] == v) {
                        result.push_back(static_cast<uint32_t>(i));
                        break;
                    }
                }
            }
            return result;
        }
        case Expr::Kind::IS_NULL: {
            // NULL is represented by INT64_MIN sentinel in APEX-DB
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
            int64_t arg = node.func_arg ? eval_arith(*node.func_arg, part, idx) : 0;
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
            const int64_t* d = col_ptr(part, expr->column);
            if (!d) return false;
            int64_t v   = d[idx];
            int64_t cmp = expr->is_float
                ? static_cast<int64_t>(expr->value_f) : expr->value;
            switch (expr->op) {
                case CompareOp::EQ: return v == cmp;
                case CompareOp::NE: return v != cmp;
                case CompareOp::GT: return v >  cmp;
                case CompareOp::LT: return v <  cmp;
                case CompareOp::GE: return v >= cmp;
                case CompareOp::LE: return v <= cmp;
            }
            return false;
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
                       const std::vector<int64_t>&)> eval_row;
    eval_row = [&](const std::shared_ptr<Expr>& expr,
                   const std::vector<int64_t>& row) -> bool {
        if (!expr) return true;
        switch (expr->kind) {
            case Expr::Kind::AND:
                return eval_row(expr->left, row) && eval_row(expr->right, row);
            case Expr::Kind::OR:
                return eval_row(expr->left, row) || eval_row(expr->right, row);
            case Expr::Kind::NOT:
                return !eval_row(expr->left, row);
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
                int64_t cmp = expr->is_float
                    ? static_cast<int64_t>(expr->value_f)
                    : expr->value;
                switch (expr->op) {
                    case CompareOp::EQ: return v == cmp;
                    case CompareOp::NE: return v != cmp;
                    case CompareOp::GT: return v >  cmp;
                    case CompareOp::LT: return v <  cmp;
                    case CompareOp::GE: return v >= cmp;
                    case CompareOp::LE: return v <= cmp;
                }
                return false;
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

    for (auto& row : result.rows) {
        if (eval_row(having.expr, row))
            filtered.rows.push_back(std::move(row));
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

    size_t limit = stmt.limit.value_or(result.rows.size());

    if (limit < result.rows.size()) {
        // top-N partial sort: std::partial_sort (O(n log k))
        std::partial_sort(
            result.rows.begin(),
            result.rows.begin() + static_cast<ptrdiff_t>(limit),
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
        result.rows.resize(limit);
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
}


QueryResultSet QueryExecutor::exec_simple_select(
    const SelectStmt& stmt,
    const std::vector<Partition*>& partitions)
{
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
                if (sel.window_func != WindowFunc::NONE) continue; // 윈도우 컬럼은 나중에 추가
                result.column_names.push_back(
                    sel.alias.empty() ? sel.column : sel.alias);
                result.column_types.push_back(ColumnType::INT64);
            }
        }
    }

    // 타임스탬프 범위 이진탐색 최적화 여부 확인
    int64_t ts_lo = INT64_MIN, ts_hi = INT64_MAX;
    bool use_ts_index = extract_time_range(stmt, ts_lo, ts_hi);

    for (auto* part : partitions) {
        if (is_cancelled()) { QueryResultSet r; r.error = "Query cancelled"; return r; }
        size_t n = part->num_rows();

        std::vector<uint32_t> sel_indices;
        if (use_ts_index) {
            // O(1): 파티션이 범위와 겹치지 않으면 스킵
            if (!part->overlaps_time_range(ts_lo, ts_hi)) continue;
            // O(log n): 이진탐색으로 범위 추출
            auto [r_begin, r_end] = part->timestamp_range(ts_lo, ts_hi);
            rows_scanned += r_end - r_begin;
            sel_indices = eval_where_ranged(stmt, *part, r_begin, r_end);
        } else {
            rows_scanned += n;
            sel_indices = eval_where(stmt, *part, n);
        }

        // ORDER BY가 있으면 LIMIT은 apply_order_by에서 처리 → 여기서 제한 없이 수집
        bool has_order = stmt.order_by.has_value();
        size_t limit = has_order ? SIZE_MAX : stmt.limit.value_or(SIZE_MAX);

        for (uint32_t idx : sel_indices) {
            if (result.rows.size() >= limit) break;

            std::vector<int64_t> row;
            if (is_star) {
                for (const auto& cv : part->columns()) {
                    const int64_t* d = static_cast<const int64_t*>(cv->raw_data());
                    row.push_back(d ? d[idx] : 0);
                }
            } else {
                for (const auto& sel : stmt.columns) {
                    if (sel.window_func != WindowFunc::NONE) continue;
                    int64_t val;
                    if (sel.case_when) {
                        val = eval_case_when(*sel.case_when, *part, idx);
                    } else if (sel.arith_expr) {
                        val = eval_arith(*sel.arith_expr, *part, idx);
                    } else if (sel.column == "symbol") {
                        val = static_cast<int64_t>(part->key().symbol_id);
                    } else {
                        const int64_t* d = get_col_data(*part, sel.column);
                        val = d ? d[idx] : 0;
                    }
                    row.push_back(val);
                }
            }
            result.rows.push_back(std::move(row));
        }
    }

    result.rows_scanned = rows_scanned;

    // ORDER BY + LIMIT: top-N partial sort
    apply_order_by(result, stmt);

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

    std::vector<int64_t>  i_accum(stmt.columns.size(), 0);
    std::vector<double>   d_accum(stmt.columns.size(), 0.0);
    std::vector<int64_t>  cnt(stmt.columns.size(), 0);
    std::vector<int64_t>  minv(stmt.columns.size(), INT64_MAX);
    std::vector<int64_t>  maxv(stmt.columns.size(), INT64_MIN);
    std::vector<double>   vwap_pv(stmt.columns.size(), 0.0);
    std::vector<int64_t>  vwap_v(stmt.columns.size(), 0);
    std::vector<int64_t>  first_val(stmt.columns.size(), 0);
    std::vector<int64_t>  last_val(stmt.columns.size(), 0);
    std::vector<bool>     has_first(stmt.columns.size(), false);

    // 타임스탬프 이진탐색 최적화
    int64_t ts_lo = INT64_MIN, ts_hi = INT64_MAX;
    bool use_ts_index = extract_time_range(stmt, ts_lo, ts_hi);

    for (auto* part : partitions) {
        if (is_cancelled()) { QueryResultSet r; r.error = "Query cancelled"; return r; }
        size_t n = part->num_rows();

        std::vector<uint32_t> sel_indices;
        if (use_ts_index) {
            if (!part->overlaps_time_range(ts_lo, ts_hi)) continue;
            auto [r_begin, r_end] = part->timestamp_range(ts_lo, ts_hi);
            rows_scanned += r_end - r_begin;
            sel_indices = eval_where_ranged(stmt, *part, r_begin, r_end);
        } else {
            rows_scanned += n;
            sel_indices = eval_where(stmt, *part, n);
        }

        for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
            const auto& col = stmt.columns[ci];
            const int64_t* data = col.arith_expr
                ? nullptr : get_col_data(*part, col.column);
            // For arithmetic expressions, compute value per row; otherwise use raw data
            auto agg_val = [&](uint32_t row_idx) -> int64_t {
                if (col.arith_expr) return eval_arith(*col.arith_expr, *part, row_idx);
                return data ? data[row_idx] : 0;
            };

            switch (col.agg) {
                case AggFunc::COUNT:
                    cnt[ci] += static_cast<int64_t>(sel_indices.size());
                    break;
                case AggFunc::SUM:
                    for (auto idx : sel_indices) i_accum[ci] += agg_val(idx);
                    break;
                case AggFunc::AVG:
                    for (auto idx : sel_indices) {
                            d_accum[ci] += static_cast<double>(agg_val(idx));
                            cnt[ci]++;
                    }
                    break;
                case AggFunc::MIN:
                    for (auto idx : sel_indices)
                        minv[ci] = std::min(minv[ci], agg_val(idx));
                    break;
                case AggFunc::MAX:
                    for (auto idx : sel_indices)
                        maxv[ci] = std::max(maxv[ci], agg_val(idx));
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
                    if (data && !sel_indices.empty() && !has_first[ci]) {
                        int64_t b = col.xbar_bucket;
                        int64_t v = data[sel_indices[0]];
                        first_val[ci] = b > 0 ? (v / b) * b : v;
                        has_first[ci] = true;
                    }
                    break;
                case AggFunc::VWAP: {
                    const int64_t* vdata = get_col_data(*part, col.agg_arg2);
                    if (data && vdata) {
                        for (auto idx : sel_indices) {
                            vwap_pv[ci] += static_cast<double>(data[idx])
                                         * static_cast<double>(vdata[idx]);
                            vwap_v[ci]  += vdata[idx];
                        }
                    }
                    break;
                }
                case AggFunc::NONE:
                    break;
            }
        }
    }

    std::vector<int64_t> row(stmt.columns.size());

    for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
        const auto& col = stmt.columns[ci];
        std::string name = col.alias.empty()
            ? (col.column.empty() ? "*" : col.column)
            : col.alias;
        result.column_names.push_back(name);
        result.column_types.push_back(ColumnType::INT64);

        switch (col.agg) {
            case AggFunc::COUNT: row[ci] = cnt[ci]; break;
            case AggFunc::SUM:   row[ci] = i_accum[ci]; break;
            case AggFunc::AVG:
                row[ci] = cnt[ci] > 0
                    ? static_cast<int64_t>(d_accum[ci] / cnt[ci])
                    : 0;
                break;
            case AggFunc::MIN:
                row[ci] = (minv[ci] == INT64_MAX) ? 0 : minv[ci];
                break;
            case AggFunc::MAX:
                row[ci] = (maxv[ci] == INT64_MIN) ? 0 : maxv[ci];
                break;
            case AggFunc::VWAP:
                row[ci] = vwap_v[ci] > 0
                    ? static_cast<int64_t>(vwap_pv[ci] / vwap_v[ci])
                    : 0;
                break;
            case AggFunc::FIRST: row[ci] = first_val[ci]; break;
            case AggFunc::LAST:  row[ci] = last_val[ci];  break;
            case AggFunc::XBAR:  row[ci] = first_val[ci]; break;
            case AggFunc::NONE: row[ci] = 0; break;
        }
    }

    result.rows.push_back(std::move(row));
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
    bool is_symbol_group = (gb.columns.size() == 1 && group_col == "symbol" && group_xbar_bucket == 0);

    struct GroupState {
        int64_t  sum     = 0;
        int64_t  count   = 0;
        double   avg_sum = 0.0;
        int64_t  minv    = INT64_MAX;
        int64_t  maxv    = INT64_MIN;
        double   vwap_pv = 0.0;
        int64_t  vwap_v  = 0;
        int64_t  first_val = 0;      // FIRST() 집계
        int64_t  last_val  = 0;      // LAST() 집계
        bool     has_first = false;  // 첫 값 기록 여부
    };

    // 타임스탬프 범위 이진탐색 최적화
    int64_t ts_lo = INT64_MIN, ts_hi = INT64_MAX;
    bool use_ts_index = extract_time_range(stmt, ts_lo, ts_hi);

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
            size_t n = part->num_rows();
            // symbol group key: 파티션 키에서 O(1)로 추출
            int64_t symbol_gkey = static_cast<int64_t>(part->key().symbol_id);

            std::vector<uint32_t> sel_indices;
            if (use_ts_index) {
                if (!part->overlaps_time_range(ts_lo, ts_hi)) continue;
                auto [r_begin, r_end] = part->timestamp_range(ts_lo, ts_hi);
                rows_scanned += r_end - r_begin;
                sel_indices = eval_where_ranged(stmt, *part, r_begin, r_end);
            } else {
                rows_scanned += n;
                sel_indices = eval_where(stmt, *part, n);
            }

            auto& states = groups[symbol_gkey];
            if (states.empty()) states.resize(stmt.columns.size());

            for (auto idx : sel_indices) {
                for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
                    const auto& col = stmt.columns[ci];
                    auto& gs = states[ci];
                    if (col.agg == AggFunc::NONE) continue;
                    const int64_t* data = col.arith_expr
                        ? nullptr : get_col_data(*part, col.column);
                    auto agg_v = [&]() -> int64_t {
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
                        row.push_back(gs.minv == INT64_MAX ? 0 : gs.minv); break;
                    case AggFunc::MAX:
                        row.push_back(gs.maxv == INT64_MIN ? 0 : gs.maxv); break;
                    case AggFunc::VWAP:
                        row.push_back(gs.vwap_v > 0
                            ? static_cast<int64_t>(gs.vwap_pv / gs.vwap_v) : 0); break;
                    case AggFunc::FIRST: row.push_back(gs.first_val); break;
                    case AggFunc::LAST:  row.push_back(gs.last_val);  break;
                    case AggFunc::XBAR:  row.push_back(gs.first_val); break;
                    case AggFunc::NONE: break;
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
    // General path: supports multi-column GROUP BY and arith_expr in aggs.
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
            int64_t            kv;
            if (gcol == "symbol") {
                kv = static_cast<int64_t>(part.key().symbol_id);
            } else {
                const int64_t* gdata = get_col_data(part, gcol);
                kv = gdata ? gdata[idx] : 0;
            }
            if (bucket > 0) kv = (kv / bucket) * bucket;
            key.push_back(kv);
        }
        return key;
    };

    for (auto* part : partitions) {
        if (is_cancelled()) { QueryResultSet r; r.error = "Query cancelled"; return r; }
        size_t n = part->num_rows();

        std::vector<uint32_t> sel_indices;
        if (use_ts_index) {
            if (!part->overlaps_time_range(ts_lo, ts_hi)) continue;
            auto [r_begin, r_end] = part->timestamp_range(ts_lo, ts_hi);
            rows_scanned += r_end - r_begin;
            sel_indices = eval_where_ranged(stmt, *part, r_begin, r_end);
        } else {
            rows_scanned += n;
            sel_indices = eval_where(stmt, *part, n);
        }

        for (auto idx : sel_indices) {
            auto gkey  = make_group_key(*part, idx);
            auto& states = groups[gkey];
            if (states.empty()) states.resize(stmt.columns.size());

            for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
                const auto& col = stmt.columns[ci];
                auto& gs = states[ci];
                if (col.agg == AggFunc::NONE) continue;
                const int64_t* data = col.arith_expr
                    ? nullptr : get_col_data(*part, col.column);
                auto agg_v = [&]() -> int64_t {
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
                    row.push_back(gs.minv == INT64_MAX ? 0 : gs.minv);
                    break;
                case AggFunc::MAX:
                    row.push_back(gs.maxv == INT64_MIN ? 0 : gs.maxv);
                    break;
                case AggFunc::VWAP:
                    row.push_back(gs.vwap_v > 0
                        ? static_cast<int64_t>(gs.vwap_pv / gs.vwap_v) : 0);
                    break;
                case AggFunc::FIRST: row.push_back(gs.first_val); break;
                case AggFunc::LAST:  row.push_back(gs.last_val);  break;
                case AggFunc::XBAR:  row.push_back(gs.first_val); break;
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

    for (const auto& sel : stmt.columns) {
        if (sel.is_star) {
            for (const auto& cv : lp->columns()) {
                result.column_names.push_back(cv->name());
                result.column_types.push_back(cv->type());
            }
            continue;
        }
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
    const std::vector<Partition*>& right_parts)
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

    // ── HashJoinOperator 사용: Arena 없이 ColumnVector가 필요하므로
    //    직접 unordered_map 방식으로 처리 ──
    std::unordered_map<int64_t, std::vector<size_t>> hash_map;
    hash_map.reserve(r_keys_flat.size() * 2);
    for (size_t ri = 0; ri < r_keys_flat.size(); ++ri) {
        hash_map[r_keys_flat[ri]].push_back(ri);
    }

    // 매칭 인덱스 쌍 생성
    // LEFT JOIN: 매칭 없는 왼쪽 행은 r_index = SIZE_MAX (NULL 표시)
    bool is_left_join = (stmt.join->type == JoinClause::Type::LEFT);
    std::vector<size_t> matched_l;
    std::vector<size_t> matched_r; // SIZE_MAX = NULL (LEFT JOIN 전용)

    for (size_t li = 0; li < l_keys_flat.size(); ++li) {
        auto it = hash_map.find(l_keys_flat[li]);
        if (it == hash_map.end()) {
            if (is_left_join) {
                // LEFT JOIN: 오른쪽 NULL로 포함
                matched_l.push_back(li);
                matched_r.push_back(SIZE_MAX); // NULL 센티넬
            }
            continue;
        }
        for (size_t ri : it->second) {
            matched_l.push_back(li);
            matched_r.push_back(ri);
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
        const RowRef& lr = l_refs[matched_l[m]];
        bool right_null = (matched_r[m] == SIZE_MAX); // LEFT JOIN에서 오른쪽 없음
        auto* lp = left_parts[lr.part_idx];
        Partition* rp = nullptr;
        RowRef rr{0, 0};
        if (!right_null) {
            rr = r_refs[matched_r[m]];
            rp = right_parts[rr.part_idx];
        }

        std::vector<int64_t> row;
        for (const auto& sel : stmt.columns) {
            if (sel.is_star) {
                for (const auto& cv : lp->columns()) {
                    const int64_t* d = static_cast<const int64_t*>(cv->raw_data());
                    row.push_back(d ? d[lr.local_idx] : 0);
                }
                continue;
            }
            bool is_right = (!sel.table_alias.empty() && sel.table_alias == r_alias);
            if (is_right) {
                if (right_null || !rp) {
                    row.push_back(JOIN_NULL); // NULL 센티넬 (INT64_MIN)
                } else {
                    const int64_t* d = get_col_data(*rp, sel.column);
                    row.push_back(d ? d[rr.local_idx] : 0);
                }
            } else {
                const int64_t* d = get_col_data(*lp, sel.column);
                row.push_back(d ? d[lr.local_idx] : 0);
            }
        }
        result.rows.push_back(std::move(row));
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
                out_sym = expr->value;
                return true;
            }
        }
        return self(self, expr->left) || self(self, expr->right);
    };
    return find_sym(find_sym, stmt.where->expr);
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
    using namespace apex::execution;

    size_t n_threads = pool_raw_->num_threads();
    ParallelScanExecutor pse(*pool_raw_);

    // 파티션 분배 모드 결정
    size_t total_rows = estimate_total_rows(partitions);
    ParallelMode mode = ParallelScanExecutor::select_mode(
        partitions.size(), total_rows, n_threads, par_opts_.row_threshold);

    if (mode == ParallelMode::SERIAL) {
        return exec_agg(stmt, partitions); // 재귀 방지를 위해 직렬 실행
    }

    // 타임스탬프 범위 최적화
    int64_t ts_lo = INT64_MIN, ts_hi = INT64_MAX;
    bool use_ts_index = extract_time_range(stmt, ts_lo, ts_hi);

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
        return p;
    };

    auto chunks = ParallelScanExecutor::make_partition_chunks(
        partitions,
        (mode == ParallelMode::PARTITION) ? n_threads : 1);

    auto partials = pse.parallel_for_chunks<PartialAgg>(
        chunks,
        init_partial,
        [&](const std::vector<Partition*>& chunk, size_t /*tid*/, PartialAgg& pa) {
            for (auto* part : chunk) {
                size_t n = part->num_rows();

                std::vector<uint32_t> sel_indices;
                if (use_ts_index) {
                    if (!part->overlaps_time_range(ts_lo, ts_hi)) continue;
                    auto [r_begin, r_end] = part->timestamp_range(ts_lo, ts_hi);
                    pa.rows_scanned += r_end - r_begin;
                    sel_indices = eval_where_ranged(stmt, *part, r_begin, r_end);
                } else {
                    pa.rows_scanned += n;
                    sel_indices = eval_where(stmt, *part, n);
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
                    ? static_cast<int64_t>(merged.d_sum[ci] / merged.cnt[ci]) : 0;
                break;
            case AggFunc::MIN:
                row[ci] = (merged.minv[ci] == INT64_MAX) ? 0 : merged.minv[ci];
                break;
            case AggFunc::MAX:
                row[ci] = (merged.maxv[ci] == INT64_MIN) ? 0 : merged.maxv[ci];
                break;
            case AggFunc::VWAP:
                row[ci] = merged.vwap_v[ci] > 0
                    ? static_cast<int64_t>(merged.vwap_pv[ci] / merged.vwap_v[ci]) : 0;
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
    using namespace apex::execution;

    size_t n_threads = pool_raw_->num_threads();
    ParallelScanExecutor pse(*pool_raw_);

    size_t total_rows = estimate_total_rows(partitions);
    ParallelMode mode = ParallelScanExecutor::select_mode(
        partitions.size(), total_rows, n_threads, par_opts_.row_threshold);

    if (mode == ParallelMode::SERIAL) {
        return exec_group_agg(stmt, partitions);
    }

    const auto& gb = stmt.group_by.value();

    int64_t ts_lo = INT64_MIN, ts_hi = INT64_MAX;
    bool use_ts_index = extract_time_range(stmt, ts_lo, ts_hi);

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
    };

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
                size_t n = part->num_rows();
                std::vector<uint32_t> sel_indices;

                if (use_ts_index) {
                    if (!part->overlaps_time_range(ts_lo, ts_hi)) continue;
                    auto [r_begin, r_end] = part->timestamp_range(ts_lo, ts_hi);
                    pg.rows_scanned += r_end - r_begin;
                    sel_indices = eval_where_ranged(stmt, *part, r_begin, r_end);
                } else {
                    pg.rows_scanned += n;
                    sel_indices = eval_where(stmt, *part, n);
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
                        const int64_t* data = col.arith_expr
                            ? nullptr : get_col_data(*part, col.column);
                        auto agg_v = [&]() -> int64_t {
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
                    row.push_back(gs.minv == INT64_MAX ? 0 : gs.minv); break;
                case AggFunc::MAX:
                    row.push_back(gs.maxv == INT64_MIN ? 0 : gs.maxv); break;
                case AggFunc::VWAP:
                    row.push_back(gs.vwap_v > 0
                        ? static_cast<int64_t>(gs.vwap_pv / gs.vwap_v) : 0); break;
                case AggFunc::FIRST: row.push_back(gs.first_val); break;
                case AggFunc::LAST:  row.push_back(gs.last_val);  break;
                case AggFunc::XBAR:  row.push_back(gs.first_val); break;
                case AggFunc::NONE: break;
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

} // namespace apex::sql
