// ============================================================================
// ZeptoDB Scheduler implementation
// ============================================================================
#include "zeptodb/scheduler/scheduler.h"

#include <cassert>
#include <ctime>
#include <stdexcept>

namespace zeptodb::scheduler {

// ============================================================================
// Utility
// ============================================================================

int64_t Scheduler::now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()
    ).count();
}

// Compute next wall-clock epoch-ns for a DAILY_AT(hour, minute, second) job.
// If the time has already passed today, returns the time tomorrow.
int64_t Scheduler::next_daily_ns(int hour, int minute, int second) {
    using namespace std::chrono;

    // Current time as time_t (seconds since epoch)
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);

    std::tm tm_local{};
#if defined(_WIN32)
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif

    // Set today's target time
    tm_local.tm_hour = hour;
    tm_local.tm_min  = minute;
    tm_local.tm_sec  = second;
    tm_local.tm_isdst = -1;

    std::time_t target_t = std::mktime(&tm_local);

    // If the time has already passed today, advance by one day
    if (target_t <= system_clock::to_time_t(now)) {
        target_t += 24 * 3600;
    }

    return static_cast<int64_t>(target_t) * 1'000'000'000LL;
}

// ============================================================================
// Job registration
// ============================================================================

void Scheduler::add_job(const std::string& name, int64_t interval_ms,
                         std::function<void()> fn) {
    if (interval_ms <= 0)
        throw std::invalid_argument("interval_ms must be > 0");

    auto job          = std::make_shared<Job>();
    job->name         = name;
    job->type         = ScheduleType::INTERVAL;
    job->interval_ms  = interval_ms;
    job->fn           = std::move(fn);
    job->next_fire_ns = now_ns() + interval_ms * 1'000'000LL;
    job->recurring    = true;

    std::lock_guard<std::mutex> lk(mu_);
    jobs_[name] = job;
    enqueue_locked(job);
    cv_.notify_one();
}

void Scheduler::add_daily(const std::string& name, int hour, int minute, int second,
                           std::function<void()> fn) {
    auto job          = std::make_shared<Job>();
    job->name         = name;
    job->type         = ScheduleType::DAILY_AT;
    job->hour         = hour;
    job->minute       = minute;
    job->second       = second;
    job->fn           = std::move(fn);
    job->next_fire_ns = next_daily_ns(hour, minute, second);
    job->recurring    = true;

    std::lock_guard<std::mutex> lk(mu_);
    jobs_[name] = job;
    enqueue_locked(job);
    cv_.notify_one();
}

void Scheduler::add_once(const std::string& name, int64_t fire_at_ns,
                          std::function<void()> fn) {
    auto job          = std::make_shared<Job>();
    job->name         = name;
    job->type         = ScheduleType::ONCE_AT;
    job->fn           = std::move(fn);
    job->next_fire_ns = fire_at_ns;
    job->recurring    = false;

    std::lock_guard<std::mutex> lk(mu_);
    jobs_[name] = job;
    enqueue_locked(job);
    cv_.notify_one();
}

// ============================================================================
// Lifecycle
// ============================================================================

void Scheduler::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
        return;  // already running

    thread_ = std::thread([this]() { run(); });
}

void Scheduler::stop() {
    if (!running_.exchange(false))
        return;  // was not running

    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

// ============================================================================
// Control
// ============================================================================

bool Scheduler::cancel(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(name);
    if (it == jobs_.end()) return false;
    it->second->cancelled.store(true, std::memory_order_relaxed);
    jobs_.erase(it);
    return true;
}

void Scheduler::cancel_all() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [_, job] : jobs_)
        job->cancelled.store(true, std::memory_order_relaxed);
    jobs_.clear();
}

std::vector<std::string> Scheduler::list_jobs() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> names;
    names.reserve(jobs_.size());
    for (const auto& [name, _] : jobs_)
        names.push_back(name);
    return names;
}

// ============================================================================
// Internal helpers
// ============================================================================

void Scheduler::enqueue_locked(std::shared_ptr<Job> job) {
    pq_.push(std::move(job));
}

// ============================================================================
// Background thread
// ============================================================================

void Scheduler::run() {
    std::unique_lock<std::mutex> lk(mu_);

    while (running_.load(std::memory_order_relaxed)) {
        // Drain cancelled jobs from the top of the queue
        while (!pq_.empty() &&
               pq_.top()->cancelled.load(std::memory_order_relaxed)) {
            pq_.pop();
        }

        if (pq_.empty()) {
            // Nothing scheduled — wait until a job is added or stop() called
            cv_.wait(lk, [this]() {
                return !running_.load(std::memory_order_relaxed) || !pq_.empty();
            });
            continue;
        }

        int64_t fire_ns = pq_.top()->next_fire_ns;
        int64_t now     = now_ns();

        if (fire_ns > now) {
            // Sleep until the next job's fire time (or until woken early)
            auto wake = std::chrono::system_clock::time_point{
                std::chrono::nanoseconds{fire_ns}};
            cv_.wait_until(lk, wake, [this, fire_ns]() {
                return !running_.load(std::memory_order_relaxed) ||
                       (!pq_.empty() && pq_.top()->next_fire_ns < fire_ns);
            });
            continue;
        }

        // Pop and fire the job
        auto job = pq_.top();
        pq_.pop();

        if (job->cancelled.load(std::memory_order_relaxed))
            continue;

        // Unlock while running the callback so the scheduler isn't blocked
        lk.unlock();
        try {
            job->fn();
        } catch (...) {
            // Jobs must not propagate exceptions — swallow silently
        }
        lk.lock();

        // Re-enqueue if recurring and not cancelled
        if (job->recurring && !job->cancelled.load(std::memory_order_relaxed)) {
            if (job->type == ScheduleType::INTERVAL) {
                job->next_fire_ns = now_ns() + job->interval_ms * 1'000'000LL;
            } else {
                // DAILY_AT: compute next day's fire time
                job->next_fire_ns = next_daily_ns(job->hour, job->minute, job->second);
            }
            pq_.push(job);
        } else if (!job->recurring) {
            // ONCE_AT: remove from jobs_ map after firing
            jobs_.erase(job->name);
        }
    }
}

}  // namespace zeptodb::scheduler
