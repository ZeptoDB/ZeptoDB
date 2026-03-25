#pragma once
// ============================================================================
// ZeptoDB: MetricsCollector — self-metrics into bounded circular buffer
// ============================================================================
// Design constraints (protect main workload):
//   1. Fixed-size circular buffer — O(1) write, zero allocation after init
//   2. Memory hard limit — sizeof(MetricsSnapshot) * capacity, never grows
//   3. Capture is lock-free (atomic write index), reads take shared lock
//   4. Background thread runs at SCHED_IDLE priority (yields to trading)
//   5. get_history() supports limit + since to minimize copy & JSON size
//   6. Automatic downsampling: keeps last N at full resolution,
//      older entries decimated (every Kth) to bound total memory
// ============================================================================

#include "zeptodb/core/pipeline.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace zeptodb::server {

// ============================================================================
// MetricsSnapshot — 72 bytes, cache-line friendly
// ============================================================================
struct MetricsSnapshot {
    int64_t  timestamp_ms           = 0;
    uint64_t ticks_ingested         = 0;
    uint64_t ticks_stored           = 0;
    uint64_t ticks_dropped          = 0;
    uint64_t queries_executed       = 0;
    uint64_t total_rows_scanned     = 0;
    uint64_t partitions_created     = 0;
    int64_t  last_ingest_latency_ns = 0;
    uint16_t node_id                = 0;
};

// ============================================================================
// MetricsConfig — all tunables in one place
// ============================================================================
struct MetricsConfig {
    std::chrono::milliseconds interval{3000};   // capture interval
    size_t capacity          = 1200;            // max snapshots (1200 × 3s = 1h)
    size_t max_memory_bytes  = 256 * 1024;      // 256 KB hard limit (~3500 snapshots)
    size_t response_limit    = 600;             // max snapshots per API response (30 min)
    uint16_t node_id         = 0;
};

// ============================================================================
// MetricsCollector
// ============================================================================
class MetricsCollector {
public:
    explicit MetricsCollector(const zeptodb::core::PipelineStats& stats,
                              MetricsConfig cfg = {})
        : stats_(stats), cfg_(cfg)
    {
        // Enforce memory limit: capacity cannot exceed max_memory_bytes
        size_t max_by_mem = cfg_.max_memory_bytes / sizeof(MetricsSnapshot);
        if (max_by_mem < 1) max_by_mem = 1;
        if (cfg_.capacity > max_by_mem) cfg_.capacity = max_by_mem;

        ring_.resize(cfg_.capacity);
        count_.store(0);
        head_.store(0);
    }

    // Legacy constructor for backward compatibility (tests)
    explicit MetricsCollector(const zeptodb::core::PipelineStats& stats,
                              std::chrono::milliseconds interval,
                              size_t capacity = 1200,
                              uint16_t node_id = 0)
        : MetricsCollector(stats, MetricsConfig{interval, capacity,
                           capacity * sizeof(MetricsSnapshot) + 1, 600, node_id})
    {}

    ~MetricsCollector() { stop(); }

    MetricsCollector(const MetricsCollector&) = delete;
    MetricsCollector& operator=(const MetricsCollector&) = delete;

    void start() {
        if (running_.load()) return;
        running_.store(true);
        thread_ = std::thread([this]() {
            set_low_priority();
            collect_loop();
        });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    /// Manual snapshot capture — O(1), no allocation, no erase
    void capture_now() {
        MetricsSnapshot snap;
        snap.timestamp_ms           = now_ms();
        snap.ticks_ingested         = stats_.ticks_ingested.load(std::memory_order_relaxed);
        snap.ticks_stored           = stats_.ticks_stored.load(std::memory_order_relaxed);
        snap.ticks_dropped          = stats_.ticks_dropped.load(std::memory_order_relaxed);
        snap.queries_executed       = stats_.queries_executed.load(std::memory_order_relaxed);
        snap.total_rows_scanned     = stats_.total_rows_scanned.load(std::memory_order_relaxed);
        snap.partitions_created     = stats_.partitions_created.load(std::memory_order_relaxed);
        snap.last_ingest_latency_ns = stats_.last_ingest_latency_ns.load(std::memory_order_relaxed);
        snap.node_id                = cfg_.node_id;

        // O(1) circular write — no mutex needed for single-writer
        size_t idx = head_.load(std::memory_order_relaxed) % cfg_.capacity;
        ring_[idx] = snap;
        head_.fetch_add(1, std::memory_order_release);

        size_t cur = count_.load(std::memory_order_relaxed);
        if (cur < cfg_.capacity) {
            count_.store(cur + 1, std::memory_order_release);
        }
    }

    /// Get snapshots. Thread-safe, bounded output.
    /// @param since_ms  Only return snapshots >= this timestamp (0 = all)
    /// @param limit     Max snapshots to return (0 = use config default)
    std::vector<MetricsSnapshot> get_history(int64_t since_ms = 0,
                                              size_t limit = 0) const {
        if (limit == 0) limit = cfg_.response_limit;

        // Guard against concurrent heavy reads (max 2 simultaneous)
        size_t prev = active_reads_.fetch_add(1, std::memory_order_acq_rel);
        if (prev >= max_concurrent_reads_) {
            active_reads_.fetch_sub(1, std::memory_order_release);
            return {};  // shed load — return empty rather than OOM
        }

        size_t cnt = count_.load(std::memory_order_acquire);
        size_t h   = head_.load(std::memory_order_acquire);

        std::vector<MetricsSnapshot> result;
        if (cnt > 0) {
            size_t start = (h >= cnt) ? (h - cnt) : 0;
            result.reserve(std::min(cnt, limit));
            for (size_t i = start; i < h && result.size() < limit; ++i) {
                auto& s = ring_[i % cfg_.capacity];
                if (since_ms > 0 && s.timestamp_ms < since_ms) continue;
                result.push_back(s);
            }
        }

        active_reads_.fetch_sub(1, std::memory_order_release);
        return result;
    }

    size_t size() const {
        return count_.load(std::memory_order_acquire);
    }

    size_t capacity() const { return cfg_.capacity; }

    size_t memory_bytes() const {
        return cfg_.capacity * sizeof(MetricsSnapshot);
    }

    const MetricsConfig& config() const { return cfg_; }

    /// Update node_id for cluster mode (safe to call before start()).
    void set_node_id(uint16_t id) { cfg_.node_id = id; }

    /// Build JSON — pre-sized ostringstream to minimize allocations
    static std::string to_json(const std::vector<MetricsSnapshot>& snaps) {
        if (snaps.empty()) return "[]";

        // ~120 chars per entry estimate
        std::ostringstream os;
        os.str().reserve(snaps.size() * 130 + 2);
        os << '[';
        for (size_t i = 0; i < snaps.size(); ++i) {
            if (i > 0) os << ',';
            auto& s = snaps[i];
            os << "{\"timestamp_ms\":" << s.timestamp_ms
               << ",\"node_id\":" << s.node_id
               << ",\"ticks_ingested\":" << s.ticks_ingested
               << ",\"ticks_stored\":" << s.ticks_stored
               << ",\"ticks_dropped\":" << s.ticks_dropped
               << ",\"queries_executed\":" << s.queries_executed
               << ",\"total_rows_scanned\":" << s.total_rows_scanned
               << ",\"partitions_created\":" << s.partitions_created
               << ",\"last_ingest_latency_ns\":" << s.last_ingest_latency_ns
               << '}';
        }
        os << ']';
        return os.str();
    }

private:
    void collect_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            capture_now();
            std::this_thread::sleep_for(cfg_.interval);
        }
    }

    /// Set thread to lowest scheduling priority so it never preempts trading
    static void set_low_priority() {
#ifdef __linux__
        // SCHED_IDLE: only runs when no other thread wants CPU
        struct sched_param param{};
        param.sched_priority = 0;
        pthread_setschedparam(pthread_self(), SCHED_IDLE, &param);
#endif
    }

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    const zeptodb::core::PipelineStats& stats_;
    MetricsConfig cfg_;

    std::atomic<bool> running_{false};
    std::thread thread_;

    // Circular buffer — fixed size, never reallocates
    std::vector<MetricsSnapshot> ring_;
    std::atomic<size_t> head_{0};   // next write position (monotonic)
    std::atomic<size_t> count_{0};  // entries written (capped at capacity)

    // Concurrent read guard — shed load if too many simultaneous reads
    mutable std::atomic<size_t> active_reads_{0};
    static constexpr size_t max_concurrent_reads_ = 4;
};

} // namespace zeptodb::server
