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
#include "apex/common/logger.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <unordered_map>
#include <stdexcept>

namespace apex::sql {

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
    : pipeline_(pipeline) {}

const PipelineStats& QueryExecutor::stats() const {
    return pipeline_.stats();
}

// ============================================================================
// 메인 실행 진입점
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

// ============================================================================
// SELECT 실행 디스패처
// ============================================================================
QueryResultSet QueryExecutor::exec_select(const SelectStmt& stmt) {
    // WHERE symbol = N 조건 추출 (파티션 레벨 필터링)
    int64_t sym_filter = -1;
    if (has_where_symbol(stmt, sym_filter, stmt.from_alias)) {
        // symbol 기반 파티션 필터링
        auto& pm = pipeline_.partition_manager();
        auto left_parts = pm.get_partitions_for_symbol(
            static_cast<apex::SymbolId>(sym_filter));

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

    // 심볼 필터 없음 → 전체 파티션
    auto left_parts = find_partitions(stmt.from_table);

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
                    if (sel.window_func != WindowFunc::NONE) continue; // 나중에 추가
                    const int64_t* d = get_col_data(*part, sel.column);
                    row.push_back(d ? d[idx] : 0);
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
// 집계 실행 (GROUP BY 없음)
// ============================================================================
QueryResultSet QueryExecutor::exec_agg(
    const SelectStmt& stmt,
    const std::vector<Partition*>& partitions)
{
    QueryResultSet result;
    size_t rows_scanned = 0;

    std::vector<int64_t>  i_accum(stmt.columns.size(), 0);
    std::vector<double>   d_accum(stmt.columns.size(), 0.0);
    std::vector<int64_t>  cnt(stmt.columns.size(), 0);
    std::vector<int64_t>  minv(stmt.columns.size(), INT64_MAX);
    std::vector<int64_t>  maxv(stmt.columns.size(), INT64_MIN);
    std::vector<double>   vwap_pv(stmt.columns.size(), 0.0);
    std::vector<int64_t>  vwap_v(stmt.columns.size(), 0);

    // 타임스탬프 이진탐색 최적화
    int64_t ts_lo = INT64_MIN, ts_hi = INT64_MAX;
    bool use_ts_index = extract_time_range(stmt, ts_lo, ts_hi);

    for (auto* part : partitions) {
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
            const int64_t* data = get_col_data(*part, col.column);

            switch (col.agg) {
                case AggFunc::COUNT:
                    cnt[ci] += static_cast<int64_t>(sel_indices.size());
                    break;
                case AggFunc::SUM:
                    if (data) {
                        for (auto idx : sel_indices) i_accum[ci] += data[idx];
                    }
                    break;
                case AggFunc::AVG:
                    if (data) {
                        for (auto idx : sel_indices) {
                            d_accum[ci] += static_cast<double>(data[idx]);
                            cnt[ci]++;
                        }
                    }
                    break;
                case AggFunc::MIN:
                    if (data) {
                        for (auto idx : sel_indices)
                            minv[ci] = std::min(minv[ci], data[idx]);
                    }
                    break;
                case AggFunc::MAX:
                    if (data) {
                        for (auto idx : sel_indices)
                            maxv[ci] = std::max(maxv[ci], data[idx]);
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
// 최적화 전략:
//   1. GROUP BY symbol: 파티션 구조 직접 활용 — 각 파티션이 이미 symbol별로
//      분리되어 있으므로 hash table 불필요. O(partitions) not O(rows).
//   2. GROUP BY 기타 컬럼: pre-allocated hash map으로 O(n) 집계
//   3. 타임스탬프 범위: 이진탐색으로 스캔 범위 최소화
// ============================================================================
QueryResultSet QueryExecutor::exec_group_agg(
    const SelectStmt& stmt,
    const std::vector<Partition*>& partitions)
{
    QueryResultSet result;
    size_t rows_scanned = 0;

    const auto& gb = stmt.group_by.value();
    const std::string& group_col = gb.columns[0];
    bool is_symbol_group = (group_col == "symbol");

    struct GroupState {
        int64_t  sum     = 0;
        int64_t  count   = 0;
        double   avg_sum = 0.0;
        int64_t  minv    = INT64_MAX;
        int64_t  maxv    = INT64_MIN;
        double   vwap_pv = 0.0;
        int64_t  vwap_v  = 0;
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
                    const int64_t* data = get_col_data(*part, col.column);
                    switch (col.agg) {
                        case AggFunc::COUNT: gs.count++; break;
                        case AggFunc::SUM:
                            if (data) gs.sum += data[idx]; break;
                        case AggFunc::AVG:
                            if (data) { gs.avg_sum += data[idx]; gs.count++; } break;
                        case AggFunc::MIN:
                            if (data) gs.minv = std::min(gs.minv, data[idx]); break;
                        case AggFunc::MAX:
                            if (data) gs.maxv = std::max(gs.maxv, data[idx]); break;
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
                    case AggFunc::NONE: break;
                }
            }
            result.rows.push_back(std::move(row));
        }

        result.rows_scanned = rows_scanned;

        // ORDER BY + LIMIT
        apply_order_by(result, stmt);
        if (!stmt.order_by.has_value() &&
            stmt.limit.has_value() && result.rows.size() > (size_t)stmt.limit.value()) {
            result.rows.resize(stmt.limit.value());
        }
        return result;
    }

    // ─────────────────────────────────────────────────────────────────────
    // 일반 경로 2: GROUP BY 기타 컬럼 (hash-based aggregation)
    // pre-allocated unordered_map으로 O(n) 집계
    // ─────────────────────────────────────────────────────────────────────
    std::unordered_map<int64_t, std::vector<GroupState>> groups;
    groups.reserve(1024); // 일반적인 cardinality 예상 pre-allocation

    for (auto* part : partitions) {
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

        const int64_t* gdata = get_col_data(*part, group_col);
        if (!gdata) continue;

        for (auto idx : sel_indices) {
            int64_t gkey = gdata[idx];
            auto& states = groups[gkey];
            if (states.empty()) states.resize(stmt.columns.size());

            for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
                const auto& col = stmt.columns[ci];
                auto& gs = states[ci];
                if (col.agg == AggFunc::NONE) continue;
                const int64_t* data = get_col_data(*part, col.column);
                switch (col.agg) {
                    case AggFunc::COUNT: gs.count++; break;
                    case AggFunc::SUM:
                        if (data) gs.sum += data[idx]; break;
                    case AggFunc::AVG:
                        if (data) { gs.avg_sum += data[idx]; gs.count++; } break;
                    case AggFunc::MIN:
                        if (data) gs.minv = std::min(gs.minv, data[idx]); break;
                    case AggFunc::MAX:
                        if (data) gs.maxv = std::max(gs.maxv, data[idx]); break;
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

    result.column_names.push_back(group_col);
    result.column_types.push_back(ColumnType::INT64);
    for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
        const auto& col = stmt.columns[ci];
        if (col.agg == AggFunc::NONE) continue;
        std::string name = col.alias.empty()
            ? (col.column.empty() ? "*" : col.column)
            : col.alias;
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
                case AggFunc::NONE: break;
            }
        }
        result.rows.push_back(std::move(row));
    }

    result.rows_scanned = rows_scanned;

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
    // (join_operator.h의 HashJoinOperator는 ColumnVector를 받으므로,
    //  여기서는 std::unordered_map을 직접 사용)
    std::unordered_map<int64_t, std::vector<size_t>> hash_map;
    hash_map.reserve(r_keys_flat.size() * 2);
    for (size_t ri = 0; ri < r_keys_flat.size(); ++ri) {
        hash_map[r_keys_flat[ri]].push_back(ri);
    }

    // 매칭 인덱스 쌍 생성
    std::vector<size_t> matched_l, matched_r;
    for (size_t li = 0; li < l_keys_flat.size(); ++li) {
        auto it = hash_map.find(l_keys_flat[li]);
        if (it == hash_map.end()) continue;
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
        const RowRef& rr = r_refs[matched_r[m]];
        auto* lp = left_parts[lr.part_idx];
        auto* rp = right_parts[rr.part_idx];

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
                const int64_t* d = get_col_data(*rp, sel.column);
                row.push_back(d ? d[rr.local_idx] : 0);
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


} // namespace apex::sql
