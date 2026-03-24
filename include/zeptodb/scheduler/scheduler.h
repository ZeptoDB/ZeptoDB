// ============================================================================
// ZeptoDB Scheduler — kdb+ .z.ts equivalent
//
// Supports three schedule types:
//   INTERVAL  — fire every N milliseconds (e.g., heartbeat, rolling stats)
//   DAILY_AT  — fire once per day at a wall-clock time (e.g., EOD at 17:00:00)
//   ONCE_AT   — fire once at a specific epoch-nanosecond timestamp
//
// Usage:
//   Scheduler sched;
//   sched.add_job("heartbeat", 1000, []() { flush_metrics(); });
//   sched.add_daily("eod", 17, 0, 0, []() { pipeline.flush_hdb(); });
//   sched.add_once("warmup", now_ns() + 5e9, []() { preload_cache(); });
//   sched.start();
//   // ... later ...
//   sched.stop();
// ============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zeptodb::scheduler {

enum class ScheduleType {
    INTERVAL,   // every interval_ms milliseconds
    DAILY_AT,   // every day at hour:minute:second (local wall clock)
    ONCE_AT,    // fire once at fire_at_ns (epoch nanoseconds)
};

// ============================================================================
// Scheduler
// ============================================================================
class Scheduler {
public:
    Scheduler()  = default;
    ~Scheduler() { stop(); }

    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // ---- Job registration ------------------------------------------------

    // Recurring: fire every interval_ms milliseconds.
    // First fire: interval_ms from now.
    void add_job(const std::string& name, int64_t interval_ms,
                 std::function<void()> fn);

    // Recurring: fire once per day at hour:minute:second (local time).
    // If the time has already passed today, first fire is tomorrow.
    void add_daily(const std::string& name, int hour, int minute, int second,
                   std::function<void()> fn);

    // One-shot: fire once at fire_at_ns (epoch nanoseconds, e.g. from now_ns()).
    void add_once(const std::string& name, int64_t fire_at_ns,
                  std::function<void()> fn);

    // ---- Lifecycle -------------------------------------------------------

    // Start the scheduler background thread. Safe to call multiple times.
    void start();

    // Stop the scheduler. Blocks until the background thread exits.
    // All pending jobs are discarded; registered jobs remain and can be
    // restarted with start().
    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // ---- Control ---------------------------------------------------------

    // Cancel a named job (safe to call from any thread, including job callbacks).
    // Returns true if the job existed and was cancelled.
    bool cancel(const std::string& name);

    // Cancel all registered jobs.
    void cancel_all();

    // Return names of all currently registered (non-cancelled) jobs.
    std::vector<std::string> list_jobs() const;

    // ---- Utility ---------------------------------------------------------

    // Current epoch time in nanoseconds.
    static int64_t now_ns();

private:
    struct Job {
        std::string    name;
        ScheduleType   type;
        int64_t        interval_ms = 0;         // INTERVAL
        int            hour = 0, minute = 0, second = 0; // DAILY_AT
        std::function<void()> fn;

        int64_t        next_fire_ns = 0;
        bool           recurring    = true;      // false for ONCE_AT
        std::atomic<bool> cancelled {false};
    };

    struct JobCmp {
        bool operator()(const std::shared_ptr<Job>& a,
                        const std::shared_ptr<Job>& b) const {
            return a->next_fire_ns > b->next_fire_ns;  // min-heap
        }
    };

    using JobQueue = std::priority_queue<
        std::shared_ptr<Job>,
        std::vector<std::shared_ptr<Job>>,
        JobCmp>;

    // Enqueue a job (must hold mu_).
    void enqueue_locked(std::shared_ptr<Job> job);

    // Compute the next epoch-ns for a DAILY_AT job.
    static int64_t next_daily_ns(int hour, int minute, int second);

    void run();

    mutable std::mutex      mu_;
    std::condition_variable cv_;
    JobQueue                pq_;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs_;

    std::atomic<bool> running_{false};
    std::thread       thread_;
};

}  // namespace zeptodb::scheduler
