#pragma once
// ============================================================================
// ZeptoDB: LocalQueryScheduler — C++20 스레드 풀 기반 로컬 스케줄러
// ============================================================================
// QueryScheduler 의 로컬 구현체.
//   - WorkerPool (std::jthread + 3단 우선순위 큐) 소유
//   - scatter(): 파티션 청크 → 부분 집계 결과 (WorkerPool 으로 분산)
//   - gather(): 부분 결과 머지
//   - pool(): WorkerPool 직접 접근 (QueryExecutor 내부 경로용)
// ============================================================================

#include "zeptodb/execution/query_scheduler.h"
#include "zeptodb/execution/worker_pool.h"
#include "zeptodb/core/pipeline.h"

namespace zeptodb::execution {

class LocalQueryScheduler : public QueryScheduler {
public:
    /// pipeline: 파티션 데이터 접근용
    /// num_threads: 0 = hardware_concurrency 자동 감지
    explicit LocalQueryScheduler(zeptodb::core::ZeptoPipeline& pipeline,
                                  size_t num_threads = 0);

    // ── QueryScheduler interface ─────────────────────────────────────────────

    /// Fragment 목록을 WorkerPool 에 분산 실행 → PartialAggResult 반환
    /// 각 Fragment 는 독립적인 파티션 청크에 대한 집계 작업
    std::vector<PartialAggResult> scatter(
        const std::vector<QueryFragment>& fragments) override;

    /// 부분 결과 목록을 단일 PartialAggResult 로 머지
    PartialAggResult gather(
        std::vector<PartialAggResult>&& partials) override;

    size_t      worker_count()   const override { return pool_.num_threads(); }
    std::string scheduler_type() const override { return "local"; }

    // ── 로컬 전용 접근자 ────────────────────────────────────────────────────
    /// WorkerPool 직접 접근 (QueryExecutor 병렬 경로에서 사용)
    WorkerPool&       pool()       { return pool_; }
    const WorkerPool& pool() const { return pool_; }

private:
    zeptodb::core::ZeptoPipeline& pipeline_;
    WorkerPool                pool_;

    /// 단일 Fragment 실행 (WorkerPool 태스크 내부에서 호출)
    PartialAggResult execute_fragment(const QueryFragment& frag);
};

} // namespace zeptodb::execution
