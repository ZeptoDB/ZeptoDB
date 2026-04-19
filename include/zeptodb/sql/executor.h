#pragma once
// ============================================================================
// ZeptoDB: SQL Query Executor
// ============================================================================
// AST → ZeptoPipeline 실행 변환기
// 파싱된 SelectStmt를 ZeptoDB 엔진 API로 매핑하여 실행
// ============================================================================

#include "zeptodb/sql/ast.h"
#include "zeptodb/sql/parser.h"
#include "zeptodb/storage/column_store.h"
#include "zeptodb/storage/string_dictionary.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/execution/query_planner.h"
#include "zeptodb/execution/query_scheduler.h"
#include "zeptodb/execution/table_statistics.h"
#include "zeptodb/execution/worker_pool.h"
#include "zeptodb/auth/cancellation_token.h"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <functional>

#ifdef ZEPTO_ENABLE_DUCKDB
namespace zeptodb::execution { class DuckDBEngine; }
#endif

namespace zeptodb::sql {

using zeptodb::storage::ColumnType;

// ============================================================================
// QueryResultSet: SQL 쿼리 결과
// ============================================================================
struct QueryResultSet {
    std::vector<std::string>             column_names;
    std::vector<ColumnType>              column_types;
    std::vector<std::vector<int64_t>>    rows;    // all values as int64 (scaled)

    // Typed value for float-aware result cells
    struct Value {
        union { int64_t i; double f; };
        Value() : i(0) {}
        Value(int64_t v) : i(v) {}
        Value(double v) : f(v) {}
    };
    // Float-aware result rows. When non-empty, used instead of `rows`.
    // Each cell is either int64 or double based on column_types[col].
    std::vector<std::vector<Value>>      typed_rows;

    // String-typed result rows (EXPLAIN plan, future string columns).
    // When non-empty, each entry corresponds to one plan/text row.
    // column_names = {"plan"}, string_rows[i] = plan line i.
    std::vector<std::string>             string_rows;

    // Symbol dictionary for resolving STRING column codes to strings
    const storage::StringDictionary*     symbol_dict = nullptr;

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
    explicit QueryExecutor(zeptodb::core::ZeptoPipeline& pipeline);

    /// 커스텀 스케줄러 주입 (테스트용 or 분산용)
    QueryExecutor(zeptodb::core::ZeptoPipeline& pipeline,
                  std::unique_ptr<zeptodb::execution::QueryScheduler> scheduler);

    /// Destructor (defined in .cpp for unique_ptr with incomplete types)
    ~QueryExecutor();

    /// SQL string execution → QueryResultSet
    QueryResultSet execute(const std::string& sql);

    /// Execute with cancellation token (set token->cancel() from another thread to abort)
    QueryResultSet execute(const std::string& sql,
                           zeptodb::auth::CancellationToken* token);

    /// 병렬 실행 활성화 (LocalQueryScheduler 재생성, num_threads 지정)
    void enable_parallel(size_t num_threads = 0,
                         size_t row_threshold = 100'000);

    /// 병렬 실행 비활성화 (단일 스레드 폴백)
    void disable_parallel();

    /// 현재 병렬 설정 조회
    const ParallelOptions& parallel_options() const { return par_opts_; }

    /// Prepared statement cache stats
    size_t prepared_cache_size() const;
    void   clear_prepared_cache();

    /// Prime the prepared-statement cache with a pre-parsed AST for the given
    /// SQL. Used by the HTTP ACL path (devlog 091 F4) to avoid re-parsing.
    /// No-op if the cache already contains this SQL's hash or is at capacity.
    void cache_prepared(const std::string& sql, ParsedStatement ps);

    /// Query result cache control
    void   enable_result_cache(size_t max_entries = 128, double ttl_seconds = 5.0);
    void   disable_result_cache();
    void   invalidate_result_cache(const std::string& table = "");
    size_t result_cache_size() const;

    /// 현재 스케줄러 접근 (테스트용)
    zeptodb::execution::QueryScheduler& scheduler() { return *scheduler_; }
    const zeptodb::execution::QueryScheduler& scheduler() const { return *scheduler_; }

    /// 파이프라인 통계 반환 (HTTP /stats 용)
    const zeptodb::core::PipelineStats& stats() const;

    /// 2-tier routing: returns true if the query needs cost-based planning
    static bool needs_cost_planning(const SelectStmt& stmt) {
        if (stmt.join)                                  return true;
        if (stmt.from_subquery)                         return true;
        if (!stmt.cte_defs.empty())                     return true;
        if (stmt.set_op != SelectStmt::SetOp::NONE)    return true;
        return false;
    }

private:
    zeptodb::core::ZeptoPipeline& pipeline_;
    ParallelOptions par_opts_;
    std::unique_ptr<zeptodb::execution::QueryScheduler> scheduler_;
    // LocalQueryScheduler 의 WorkerPool 을 가리키는 raw pointer.
    // 로컬 병렬 경로(exec_agg_parallel 등)에서 직접 사용.
    // 비-로컬 스케줄러 주입 시 nullptr → 직렬 폴백.
    zeptodb::execution::WorkerPool* pool_raw_ = nullptr;

    // ── Cost-based planner state ─────────────────────────────────────────────
    zeptodb::execution::TableStatistics table_stats_;

    // ── Prepared statement cache (parse result reuse) ────────────────────────
    mutable std::mutex stmt_cache_mu_;
    std::unordered_map<size_t, ParsedStatement> stmt_cache_;  // hash(sql) → parsed

    // ── Query result cache (TTL-based) ───────────────────────────────────────
    struct CachedResult {
        QueryResultSet result;
        double         expire_time;  // monotonic seconds
    };
    mutable std::mutex result_cache_mu_;
    std::unordered_map<size_t, CachedResult> result_cache_;
    size_t result_cache_max_   = 0;      // 0 = disabled
    double result_cache_ttl_s_ = 5.0;

    // ── DuckDB offload engine (lazy-initialized) ─────────────────────────────
#ifdef ZEPTO_ENABLE_DUCKDB
    bool   enable_duckdb_offload_  = true;
    size_t duckdb_memory_limit_mb_ = 256;
    std::unique_ptr<zeptodb::execution::DuckDBEngine> duckdb_engine_;
    std::mutex duckdb_mu_;

    /// Lazy-initialize DuckDB engine on first use
    zeptodb::execution::DuckDBEngine& get_duckdb_engine();

    /// Execute a query via DuckDB on Parquet files
    QueryResultSet exec_via_duckdb(const std::string& sql,
                                   const std::vector<std::string>& parquet_paths);

    /// Check if a table name is a duckdb('path') function call
    static bool is_duckdb_table_func(const std::string& table_name,
                                     std::string& out_path);
#endif

    static size_t sql_hash(const std::string& sql);

    // Resolve uncorrelated scalar/IN subqueries in WHERE tree into literals
    void resolve_subqueries(std::shared_ptr<Expr>& expr);

    // DDL 실행 함수들
    QueryResultSet exec_create_table(const CreateTableStmt& stmt);
    QueryResultSet exec_drop_table(const DropTableStmt& stmt);
    QueryResultSet exec_alter_table(const AlterTableStmt& stmt);

    // DML 실행 함수들
    QueryResultSet exec_insert(const InsertStmt& stmt);
    QueryResultSet exec_update(const UpdateStmt& stmt);
    QueryResultSet exec_delete(const DeleteStmt& stmt);

    // Materialized View
    QueryResultSet exec_create_mv(const CreateMVStmt& stmt);
    QueryResultSet exec_drop_mv(const DropMVStmt& stmt);

    // SELECT 실행 내부 함수들
    QueryResultSet exec_select(const SelectStmt& stmt_in);

    // WHERE 절 평가 (행 인덱스 필터링)
    std::vector<uint32_t> eval_where(
        const SelectStmt& stmt,
        const zeptodb::storage::Partition& part,
        size_t num_rows);

    // WHERE 절 평가 — 타임스탬프 범위 힌트를 이용해 [row_begin, row_end) 범위만 스캔
    std::vector<uint32_t> eval_where_ranged(
        const SelectStmt& stmt,
        const zeptodb::storage::Partition& part,
        size_t row_begin,
        size_t row_end);

    // WHERE Expr 재귀 평가
    std::vector<uint32_t> eval_expr(
        const std::shared_ptr<Expr>& expr,
        const zeptodb::storage::Partition& part,
        size_t num_rows,
        const std::string& default_alias);

    // 집계 없는 단순 SELECT
    QueryResultSet exec_simple_select(
        const SelectStmt& stmt,
        const std::vector<zeptodb::storage::Partition*>& partitions);

    // GROUP BY + 집계
    QueryResultSet exec_group_agg(
        const SelectStmt& stmt,
        const std::vector<zeptodb::storage::Partition*>& partitions);

    // 집계 (GROUP BY 없음)
    QueryResultSet exec_agg(
        const SelectStmt& stmt,
        const std::vector<zeptodb::storage::Partition*>& partitions);

    // ASOF JOIN 실행
    QueryResultSet exec_asof_join(
        const SelectStmt& stmt,
        const std::vector<zeptodb::storage::Partition*>& left_partitions,
        const std::vector<zeptodb::storage::Partition*>& right_partitions);

    // Hash JOIN 실행 (equi join)
    QueryResultSet exec_hash_join(
        const SelectStmt& stmt,
        const std::vector<zeptodb::storage::Partition*>& left_partitions,
        const std::vector<zeptodb::storage::Partition*>& right_partitions,
        const std::shared_ptr<zeptodb::execution::PhysicalNode>& physical_plan = nullptr);

    // WINDOW JOIN 실행 (kdb+ wj 스타일)
    QueryResultSet exec_window_join(
        const SelectStmt& stmt,
        const std::vector<zeptodb::storage::Partition*>& left_partitions,
        const std::vector<zeptodb::storage::Partition*>& right_partitions);

    // UNION JOIN 실행 (kdb+ uj — merge columns from both tables)
    QueryResultSet exec_union_join(
        const SelectStmt& stmt,
        const std::vector<zeptodb::storage::Partition*>& left_partitions,
        const std::vector<zeptodb::storage::Partition*>& right_partitions);

    // PLUS JOIN 실행 (kdb+ pj — additive join on matching keys)
    QueryResultSet exec_plus_join(
        const SelectStmt& stmt,
        const std::vector<zeptodb::storage::Partition*>& left_partitions,
        const std::vector<zeptodb::storage::Partition*>& right_partitions);

    // 윈도우 함수 적용 (결과에 새 컬럼 추가)
    void apply_window_functions(
        const SelectStmt& stmt,
        QueryResultSet& result);

    // 파티션 목록 조회 (테이블명 기준)
    std::vector<zeptodb::storage::Partition*> find_partitions(const std::string& table_name);

    // 파티션에서 컬럼 데이터 가져오기 (없으면 nullptr)
    const int64_t* get_col_data(
        const zeptodb::storage::Partition& part,
        const std::string& col_name) const;

    /// Type-dispatched column access
    template<typename T>
    const T* get_col_typed(
        const zeptodb::storage::Partition& part,
        const std::string& col_name) const {
        const auto* cv = part.get_column(col_name);
        return cv ? static_cast<const T*>(cv->raw_data()) : nullptr;
    }

    // SymbolId 조회 (trades, quotes 테이블 공통)
    bool has_where_symbol(const SelectStmt& stmt, int64_t& out_sym,
                          const std::string& alias) const;
    bool has_where_symbol_in(const SelectStmt& stmt,
                             std::vector<int64_t>& out_syms) const;

    // WHERE timestamp BETWEEN X AND Y 조건 추출
    bool extract_time_range(const SelectStmt& stmt,
                            int64_t& out_lo, int64_t& out_hi) const;

    // WHERE col BETWEEN X AND Y / col >= X AND col <= Y on an s#-sorted column.
    // Returns true (and populates out_col/out_lo/out_hi) if the WHERE clause
    // contains a tightenable range condition on any sorted column in the given
    // partition.
    bool extract_sorted_col_range(const SelectStmt& stmt,
                                  const zeptodb::storage::Partition& part,
                                  std::string& out_col,
                                  int64_t& out_lo,
                                  int64_t& out_hi) const;

    // WHERE col = X on a g# (grouped/hash) or p# (parted) indexed column.
    // Returns true if an equality condition was found on an indexed column.
    bool extract_index_eq(const SelectStmt& stmt,
                          const zeptodb::storage::Partition& part,
                          std::string& out_col,
                          int64_t& out_val) const;

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
        const std::vector<zeptodb::storage::Partition*>& partitions);

    // 병렬 단순 SELECT (집계 없음)
    QueryResultSet exec_simple_select_parallel(
        const SelectStmt& stmt,
        const std::vector<zeptodb::storage::Partition*>& partitions);

    // 병렬 GROUP BY 집계
    QueryResultSet exec_group_agg_parallel(
        const SelectStmt& stmt,
        const std::vector<zeptodb::storage::Partition*>& partitions);

    // 총 행 수 추정 (병렬 임계값 판단용)
    size_t estimate_total_rows(
        const std::vector<zeptodb::storage::Partition*>& partitions) const;

    // ── Composite Index (Index Intersection) ─────────────────────────────────

    /// Accumulates intersected row ranges and sets from multiple index lookups.
    struct IndexResult {
        size_t range_begin = 0;
        size_t range_end   = SIZE_MAX;
        std::vector<uint32_t> row_set;
        bool has_set = false;

        void set_range(size_t b, size_t e);
        void intersect_range(size_t b, size_t e);
        void intersect_set(const std::vector<uint32_t>& s);
        std::vector<uint32_t> materialize() const;
    };

    struct SortedRangePred { std::string col; int64_t lo; int64_t hi; };
    struct IndexEqPred     { std::string col; int64_t val; };

    /// Extract ALL s#-sorted column range predicates from WHERE (AND-connected).
    std::vector<SortedRangePred> extract_all_sorted_ranges(
        const SelectStmt& stmt,
        const zeptodb::storage::Partition& part) const;

    /// Extract ALL g#/p# equality predicates from WHERE (AND-connected).
    std::vector<IndexEqPred> extract_all_index_eqs(
        const SelectStmt& stmt,
        const zeptodb::storage::Partition& part) const;

    /// Evaluate a single WHERE Expr node for one row. Returns true if the row matches.
    bool eval_expr_single_row(
        const std::shared_ptr<Expr>& expr,
        const zeptodb::storage::Partition& part,
        uint32_t row_idx) const;

    /// Evaluate WHERE on candidate rows, skipping predicates on indexed_cols.
    std::vector<uint32_t> eval_remaining_where(
        const SelectStmt& stmt,
        const zeptodb::storage::Partition& part,
        const std::vector<uint32_t>& candidates,
        const std::unordered_set<std::string>& indexed_cols);

    /// Collect-and-intersect: build IndexResult from all applicable indexes,
    /// then evaluate remaining WHERE predicates on the intersection.
    /// Returns filtered row indices and updates rows_scanned.
    std::vector<uint32_t> collect_and_intersect(
        const SelectStmt& stmt,
        const zeptodb::storage::Partition& part,
        size_t& rows_scanned);

    // ── Cost-based planner wiring helpers ────────────────────────────────────

    /// Walk a physical plan tree to find the HASH_JOIN PhysicalNode.
    /// Returns nullptr if no plan or no HASH_JOIN node found.
    static const zeptodb::execution::PhysicalNode* find_hash_join_node(
        const std::shared_ptr<zeptodb::execution::PhysicalNode>& plan);
};

} // namespace zeptodb::sql

