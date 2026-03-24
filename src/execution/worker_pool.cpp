// ============================================================================
// ZeptoDB: WorkerPool Implementation
// ============================================================================

#include "zeptodb/execution/worker_pool.h"
#include <algorithm>

namespace zeptodb::execution {

WorkerPool::WorkerPool(size_t num_threads) {
    if (num_threads == 0) {
        num_threads = std::max(1u, std::thread::hardware_concurrency());
    }

    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this](std::stop_token st) {
            worker_loop(st);
        });
    }
}

WorkerPool::~WorkerPool() {
    {
        std::unique_lock lock(queue_mutex_);
        stop_ = true;
    }
    cv_work_.notify_all();
    // jthread destructor requests stop and joins automatically
}

void WorkerPool::submit(std::function<void()> fn,
                        Priority p,
                        std::latch* done)
{
    {
        std::unique_lock lock(queue_mutex_);
        in_flight_.fetch_add(1, std::memory_order_relaxed);
        queues_[static_cast<size_t>(p)].push_back({std::move(fn), done});
    }
    cv_work_.notify_one();
}

void WorkerPool::wait_idle() {
    std::unique_lock lock(queue_mutex_);
    cv_idle_.wait(lock, [this] {
        return in_flight_.load(std::memory_order_relaxed) == 0;
    });
}

void WorkerPool::worker_loop(std::stop_token st) {
    while (true) {
        Task task;
        {
            std::unique_lock lock(queue_mutex_);
            cv_work_.wait(lock, [this, &st] {
                if (stop_ || st.stop_requested()) return true;
                for (auto& q : queues_) {
                    if (!q.empty()) return true;
                }
                return false;
            });

            if ((stop_ || st.stop_requested())) {
                // Drain remaining tasks before exit
                bool found = false;
                for (auto& q : queues_) {
                    if (!q.empty()) {
                        task = std::move(q.front());
                        q.pop_front();
                        found = true;
                        break;
                    }
                }
                if (!found) break;
            } else {
                // Pick highest priority non-empty queue
                bool found = false;
                for (auto& q : queues_) {
                    if (!q.empty()) {
                        task = std::move(q.front());
                        q.pop_front();
                        found = true;
                        break;
                    }
                }
                if (!found) continue;
            }
        }

        // Execute task
        task.fn();

        // Signal completion
        if (task.done) {
            task.done->count_down();
        }

        int64_t remaining = in_flight_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            cv_idle_.notify_all();
        }
    }
}

} // namespace zeptodb::execution
