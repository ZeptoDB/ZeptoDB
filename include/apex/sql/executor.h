#pragma once
// ============================================================================
// APEX-DB: SQL Query Executor
// ============================================================================
// AST → ApexPipeline 실행 변환기
// 파싱된 SelectStmt를 APEX 엔진 API로 매핑하여 실행
// ============================================================================

#include "apex/sql/ast.h"
#include "apex/storage/column_store.h"
#include "apex/core/pipeline.h"
#include <string>
#include <vector>
#include <cstdint>

namespace apex::sql {

using apex::storage::ColumnType;

// ============================================================================
// QueryResultSet: SQL 쿼리 결과
// ============================================================================
struct QueryResultSet {
    std::vector<std::string>             column_names;
    std::vector<ColumnType>              column_types;
    std::vector<std::vector<int64_t>>    rows;    // 모든 값을 int64로 (scaled)

    double  execution_time_us = 0.0;
    size_t  rows_scanned      = 0;
    std::string error;

    bool ok() const { return error.empty(); }
};

// ============================================================================
// QueryExecutor: SQL 실행 엔진
// ============================================================================
class QueryExecutor {
public:
    explicit QueryExecutor(apex::core::ApexPipeline& pipeline);

    /// SQL 문자열 실행 → QueryResultSet 반환
    QueryResultSet execute(const std::string& sql);

    /// 파이프라인 통계 반환 (HTTP /stats 용)
    const apex::core::PipelineStats& stats() const;

private:
    apex::core::ApexPipeline& pipeline_;

    // SELECT 실행 내부 함수들
    QueryResultSet exec_select(const SelectStmt& stmt);

    // WHERE 절 평가 (행 인덱스 필터링)
    std::vector<uint32_t> eval_where(
        const SelectStmt& stmt,
        const apex::storage::Partition& part,
        size_t num_rows);

    // WHERE 절 평가 — 타임스탬프 범위 힌트를 이용해 [row_begin, row_end) 범위만 스캔
    std::vector<uint32_t> eval_where_ranged(
        const SelectStmt& stmt,
        const apex::storage::Partition& part,
        size_t row_begin,
        size_t row_end);

    // WHERE Expr 재귀 평가
    std::vector<uint32_t> eval_expr(
        const std::shared_ptr<Expr>& expr,
        const apex::storage::Partition& part,
        size_t num_rows,
        const std::string& default_alias);

    // 집계 없는 단순 SELECT
    QueryResultSet exec_simple_select(
        const SelectStmt& stmt,
        const std::vector<apex::storage::Partition*>& partitions);

    // GROUP BY + 집계
    QueryResultSet exec_group_agg(
        const SelectStmt& stmt,
        const std::vector<apex::storage::Partition*>& partitions);

    // 집계 (GROUP BY 없음)
    QueryResultSet exec_agg(
        const SelectStmt& stmt,
        const std::vector<apex::storage::Partition*>& partitions);

    // ASOF JOIN 실행
    QueryResultSet exec_asof_join(
        const SelectStmt& stmt,
        const std::vector<apex::storage::Partition*>& left_partitions,
        const std::vector<apex::storage::Partition*>& right_partitions);

    // Hash JOIN 실행 (equi join)
    QueryResultSet exec_hash_join(
        const SelectStmt& stmt,
        const std::vector<apex::storage::Partition*>& left_partitions,
        const std::vector<apex::storage::Partition*>& right_partitions);

    // 윈도우 함수 적용 (결과에 새 컬럼 추가)
    void apply_window_functions(
        const SelectStmt& stmt,
        QueryResultSet& result);

    // 파티션 목록 조회 (테이블명 기준)
    std::vector<apex::storage::Partition*> find_partitions(const std::string& table_name);

    // 파티션에서 컬럼 데이터 가져오기 (없으면 nullptr)
    const int64_t* get_col_data(
        const apex::storage::Partition& part,
        const std::string& col_name) const;

    // SymbolId 조회 (trades, quotes 테이블 공통)
    bool has_where_symbol(const SelectStmt& stmt, int64_t& out_sym,
                          const std::string& alias) const;

    // WHERE timestamp BETWEEN X AND Y 조건 추출
    bool extract_time_range(const SelectStmt& stmt,
                            int64_t& out_lo, int64_t& out_hi) const;

    // ORDER BY + LIMIT 적용 (top-N partial sort)
    void apply_order_by(QueryResultSet& result, const SelectStmt& stmt);
};

} // namespace apex::sql

