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
#include "apex/execution/query_scheduler.h"
#include "apex/execution/worker_pool.h"
#include "apex/auth/cancellation_token.h"
#include <memory>
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
// ParallelOptions: 병렬 실행 설정
// ============================================================================
struct ParallelOptions {
    bool   enabled        = false;
    size_t num_threads    = 0;        // 0 = hardware_concurrency
    size_t row_threshold  = 100'000;  // 이 행 수 미만은 단일 스레드
};

// ============================================================================
// QueryExecutor: SQL 실행 엔진
// ============================================================================
class QueryExecutor {
public:
    /// 기본 생성자: LocalQueryScheduler (hardware_concurrency 스레드)
    explicit QueryExecutor(apex::core::ApexPipeline& pipeline);

    /// 커스텀 스케줄러 주입 (테스트용 or 분산용)
    QueryExecutor(apex::core::ApexPipeline& pipeline,
                  std::unique_ptr<apex::execution::QueryScheduler> scheduler);

    /// SQL string execution → QueryResultSet
    QueryResultSet execute(const std::string& sql);

    /// Execute with cancellation token (set token->cancel() from another thread to abort)
    QueryResultSet execute(const std::string& sql,
                           apex::auth::CancellationToken* token);

    /// 병렬 실행 활성화 (LocalQueryScheduler 재생성, num_threads 지정)
    void enable_parallel(size_t num_threads = 0,
                         size_t row_threshold = 100'000);

    /// 병렬 실행 비활성화 (단일 스레드 폴백)
    void disable_parallel();

    /// 현재 병렬 설정 조회
    const ParallelOptions& parallel_options() const { return par_opts_; }

    /// 현재 스케줄러 접근 (테스트용)
    apex::execution::QueryScheduler& scheduler() { return *scheduler_; }
    const apex::execution::QueryScheduler& scheduler() const { return *scheduler_; }

    /// 파이프라인 통계 반환 (HTTP /stats 용)
    const apex::core::PipelineStats& stats() const;

private:
    apex::core::ApexPipeline& pipeline_;
    ParallelOptions par_opts_;
    std::unique_ptr<apex::execution::QueryScheduler> scheduler_;
    // LocalQueryScheduler 의 WorkerPool 을 가리키는 raw pointer.
    // 로컬 병렬 경로(exec_agg_parallel 등)에서 직접 사용.
    // 비-로컬 스케줄러 주입 시 nullptr → 직렬 폴백.
    apex::execution::WorkerPool* pool_raw_ = nullptr;

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

    // WINDOW JOIN 실행 (kdb+ wj 스타일)
    QueryResultSet exec_window_join(
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

    // HAVING 절 필터: 집계 결과 행을 조건에 맞게 걸러냄
    QueryResultSet apply_having_filter(QueryResultSet result,
                                       const WhereClause& having) const;

    // ── Virtual table (CTE / subquery) execution path ────────────────────────

    // Execute a SELECT whose FROM source is an in-memory QueryResultSet
    // (produced by a CTE or a FROM-subquery).  Handles WHERE, GROUP BY,
    // ORDER BY, LIMIT on the virtual result set.
    QueryResultSet exec_select_virtual(
        const SelectStmt& stmt,
        const QueryResultSet& src,
        const std::string& src_alias);

    // ── 병렬 집계 경로 ──────────────────────────────────────────────────────

    // 병렬 단순 집계 (GROUP BY 없음)
    QueryResultSet exec_agg_parallel(
        const SelectStmt& stmt,
        const std::vector<apex::storage::Partition*>& partitions);

    // 병렬 GROUP BY 집계
    QueryResultSet exec_group_agg_parallel(
        const SelectStmt& stmt,
        const std::vector<apex::storage::Partition*>& partitions);

    // 총 행 수 추정 (병렬 임계값 판단용)
    size_t estimate_total_rows(
        const std::vector<apex::storage::Partition*>& partitions) const;
};

} // namespace apex::sql

