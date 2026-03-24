#pragma once
// ============================================================================
// ZeptoDB: ParallelScanExecutor — Parallel Partition/Row Scan Utilities
// ============================================================================
// 두 가지 병렬화 모드:
//   1. 파티션 병렬: 파티션 수 >= 스레드 수 → 파티션 목록을 N청크로 분할
//   2. 행 청크 병렬: 단일 파티션 내 행을 N청크로 분할 (large single partition)
//
// 자동 모드 선택:
//   - total_rows < threshold(100K) → SERIAL (스레드 오버헤드 > 이득)
//   - num_partitions >= num_threads → PARTITION
//   - otherwise → CHUNKED
// ============================================================================

#include "zeptodb/execution/worker_pool.h"
#include "zeptodb/storage/column_store.h"
#include "zeptodb/storage/partition_manager.h"

#include <functional>
#include <latch>
#include <utility>
#include <vector>
#include <cstddef>

namespace zeptodb::execution {

// ============================================================================
// ParallelMode: 병렬화 전략 선택
// ============================================================================
enum class ParallelMode {
    SERIAL,     // 단일 스레드 (소량 데이터)
    PARTITION,  // 파티션 단위 병렬 (파티션 수 >= 스레드 수)
    CHUNKED,    // 행 청크 단위 병렬 (대형 단일 파티션)
};

// ============================================================================
// ParallelScanExecutor: 병렬 스캔 유틸리티
// ============================================================================
class ParallelScanExecutor {
public:
    explicit ParallelScanExecutor(WorkerPool& pool) : pool_(pool) {}

    // -------------------------------------------------------------------------
    // 파티션 청크 분배
    // 반환: num_chunks 개의 파티션 리스트 (마지막 청크에 나머지 포함)
    // -------------------------------------------------------------------------
    static std::vector<std::vector<zeptodb::storage::Partition*>>
    make_partition_chunks(const std::vector<zeptodb::storage::Partition*>& parts,
                          size_t num_chunks);

    // -------------------------------------------------------------------------
    // 행 범위 청크 분배 [0, num_rows) → N개 (begin, end) 쌍
    // -------------------------------------------------------------------------
    static std::vector<std::pair<size_t, size_t>>
    make_row_chunks(size_t num_rows, size_t num_chunks);

    // -------------------------------------------------------------------------
    // 병렬화 모드 자동 선택
    // -------------------------------------------------------------------------
    static ParallelMode select_mode(size_t num_partitions,
                                    size_t total_rows,
                                    size_t num_threads,
                                    size_t serial_threshold = 100'000);

    // -------------------------------------------------------------------------
    // 병렬 포파티션 실행:
    //   - chunks: make_partition_chunks()의 반환값
    //   - worker_fn: void(const std::vector<Partition*>& chunk, size_t tid, T& out)
    //   - initial: 각 스레드 출력의 초기값 생성자
    // 반환: std::vector<T> — 스레드 수만큼의 부분 결과
    // -------------------------------------------------------------------------
    template<typename T, typename WorkerFn, typename InitFn>
    std::vector<T> parallel_for_chunks(
        const std::vector<std::vector<zeptodb::storage::Partition*>>& chunks,
        InitFn init_fn,
        WorkerFn worker_fn)
    {
        size_t n = chunks.size();
        std::vector<T> results(n);
        for (size_t i = 0; i < n; ++i) results[i] = init_fn();

        std::latch done(static_cast<ptrdiff_t>(n));
        for (size_t i = 0; i < n; ++i) {
            pool_.submit(
                [&chunks, &results, &worker_fn, i]() {
                    worker_fn(chunks[i], i, results[i]);
                },
                WorkerPool::Priority::HIGH,
                &done
            );
        }
        done.wait();
        return results;
    }

    WorkerPool& pool() { return pool_; }

private:
    WorkerPool& pool_;
};

} // namespace zeptodb::execution
