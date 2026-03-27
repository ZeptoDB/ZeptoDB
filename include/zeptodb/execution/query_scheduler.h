#pragma once
// ============================================================================
// ZeptoDB: QueryScheduler — 로컬/분산 쿼리 스케줄러 추상화
// ============================================================================
// 설계 목표:
//   - 로컬 스레드 풀 (LocalQueryScheduler) 과 분산 노드
//     (DistributedQueryScheduler) 를 동일 인터페이스로 추상화.
//   - QueryExecutor 코드 수정 없이 DistributedScheduler 로 교체 가능.
//   - PartialAggResult: 직렬화 가능 → 나중에 RDMA/UCX 전송에 사용.
// ============================================================================

#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeptodb::execution {

// ============================================================================
// AggType: 집계 함수 타입 (SQL 레이어 AggFunc 와 독립적으로 선언)
// ============================================================================
enum class AggType : uint8_t {
    NONE    = 0,
    SUM     = 1,
    COUNT   = 2,
    AVG     = 3,
    MIN     = 4,
    MAX     = 5,
    VWAP    = 6,
    FIRST   = 7,
    LAST    = 8,
    XBAR    = 9,
    GROUP_BY = 10,
};

// ============================================================================
// QueryFragment: 단일 노드(또는 스레드)가 실행할 최소 작업 단위
// ============================================================================
// 직렬화 가능 필드:
//   - partition_ids, table_name, agg_*, filter hints
// 로컬 전용 (직렬화 제외):
//   - 없음 (LocalQueryScheduler 는 pipeline 참조로 파티션 조회)
// ============================================================================
struct QueryFragment {
    std::string            table_name;
    std::vector<uint32_t>  partition_ids;   // 담당 파티션 ID 목록

    // 집계 정보 (SELECT 컬럼별)
    std::vector<AggType>     agg_types;     // 컬럼당 집계 타입
    std::vector<std::string> agg_columns;  // 집계 대상 컬럼명
    std::vector<std::string> agg_args2;    // 보조 컬럼 (VWAP: volume)
    std::vector<int64_t>     xbar_buckets; // XBAR 버킷 크기

    // GROUP BY
    bool        has_group_by      = false;
    std::string group_by_column;
    int64_t     group_xbar_bucket = 0;
    int64_t     group_dt_bucket   = 0;

    // 단순 필터 힌트 (직렬화 가능)
    int64_t symbol_filter = -1;       // -1 = no filter
    int64_t ts_lo         = INT64_MIN; // timestamp BETWEEN lo AND hi
    int64_t ts_hi         = INT64_MAX;

    // 행 범위 (청크 병렬 분할용)
    size_t row_begin = 0;
    size_t row_end   = SIZE_MAX;
};

// ============================================================================
// PartialAggResult: 부분 집계 결과 — 네트워크 직렬화 가능
// ============================================================================
// 각 컬럼에 대해 충분한 상태를 저장해서 나중에 다른 Partial 과 머지 가능.
// GROUP BY: group_key → sub-PartialAggResult 맵으로 계층 표현.
// ============================================================================
struct PartialAggResult {
    // 컬럼별 집계 상태 (agg_types.size() 와 동일 크기)
    std::vector<int64_t> sum;
    std::vector<int64_t> count;
    std::vector<double>  d_sum;      // AVG 용 (정밀도 유지)
    std::vector<int64_t> min_val;
    std::vector<int64_t> max_val;
    std::vector<int64_t> vwap_pv;   // price * volume 합 (scaled to int64)
    std::vector<int64_t> vwap_vol;  // volume 합
    std::vector<int64_t> first_val;
    std::vector<int64_t> last_val;
    std::vector<bool>    has_first;
    size_t rows_scanned = 0;

    // GROUP BY 용: group_key → 하위 PartialAggResult
    // (재귀 구조를 허용하기 위해 shared_ptr 사용)
    std::unordered_map<int64_t, std::shared_ptr<PartialAggResult>> group_partials;

    // ncols 크기로 초기화 (min_val = INT64_MAX, max_val = INT64_MIN)
    void resize(size_t ncols);

    // 다른 Partial 을 this 에 병합 (gather 에서 사용)
    void merge(const PartialAggResult& other);

    // 직렬화 인터페이스 (나중에 RDMA/UCX 전송용)
    std::vector<uint8_t> serialize() const;
    static PartialAggResult deserialize(const uint8_t* buf, size_t len);
};

// ============================================================================
// QueryScheduler: 로컬 스레드 풀 / 분산 노드 공통 인터페이스
// ============================================================================
class QueryScheduler {
public:
    virtual ~QueryScheduler() = default;

    // Fragment 를 실행 → 부분 결과 반환
    virtual std::vector<PartialAggResult> scatter(
        const std::vector<QueryFragment>& fragments) = 0;

    // 부분 결과들을 최종 결과로 머지
    virtual PartialAggResult gather(
        std::vector<PartialAggResult>&& partials) = 0;

    // 스케줄러 정보
    virtual size_t      worker_count()   const = 0;
    virtual std::string scheduler_type() const = 0;
};

} // namespace zeptodb::execution
