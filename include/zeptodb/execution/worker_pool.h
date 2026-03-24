#pragma once
// ============================================================================
// ZeptoDB: WorkerPool — C++20 Thread Pool for Parallel Query Execution
// ============================================================================
// kdb+ -s N 옵션과 유사한 멀티코어 병렬 실행 엔진
//
// 설계:
//   - std::jthread 기반 워커 (자동 join, stop_token 지원)
//   - 3단 우선순위 큐 (HIGH / NORMAL / LOW)
//   - std::latch (C++20) 기반 배치 완료 대기
//   - CPU 코어 수 자동 감지 (hardware_concurrency)
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <latch>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdint>

namespace zeptodb::execution {

// ============================================================================
// WorkerPool: 고정 크기 스레드 풀 + 우선순위 작업 큐
// ============================================================================
class WorkerPool {
public:
    enum class Priority : uint8_t { HIGH = 0, NORMAL = 1, LOW = 2 };

    /// num_threads = 0이면 hardware_concurrency 자동 감지
    explicit WorkerPool(size_t num_threads = 0);

    ~WorkerPool();

    // Non-copyable, non-movable (mutex + threads)
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /// 작업 제출 (비동기)
    /// submitted latch가 있으면 작업 완료 시 count_down() 호출
    void submit(std::function<void()> fn,
                Priority p = Priority::NORMAL,
                std::latch* done = nullptr);

    /// 모든 미완료 작업이 끝날 때까지 블록
    void wait_idle();

    /// 워커 스레드 수
    size_t num_threads() const { return workers_.size(); }

    /// 현재 대기 중인 작업 수
    size_t pending_count() const {
        return static_cast<size_t>(in_flight_.load(std::memory_order_relaxed));
    }

private:
    struct Task {
        std::function<void()> fn;
        std::latch*           done;  // nullable
    };

    // 3개 우선순위 큐 (각각 독립 deque)
    std::deque<Task> queues_[3];
    std::mutex        queue_mutex_;
    std::condition_variable cv_work_;   // 새 작업 알림
    std::condition_variable cv_idle_;   // 모든 작업 완료 알림

    std::atomic<int64_t> in_flight_{0}; // 제출됐지만 완료 안 된 작업 수
    bool                 stop_ = false;

    std::vector<std::jthread> workers_;

    void worker_loop(std::stop_token st);
};

// ============================================================================
// 편의 함수: N개 작업을 병렬 실행하고 완료까지 블록
// ============================================================================
/// tasks를 pool에 제출하고 모두 완료될 때까지 대기
inline void parallel_run(WorkerPool& pool,
                         std::vector<std::function<void()>> tasks,
                         WorkerPool::Priority p = WorkerPool::Priority::NORMAL)
{
    if (tasks.empty()) return;
    std::latch done(static_cast<ptrdiff_t>(tasks.size()));
    for (auto& fn : tasks) {
        pool.submit(std::move(fn), p, &done);
    }
    done.wait();
}

} // namespace zeptodb::execution
