#pragma once
// ============================================================================
// ResourceIsolation — CPU pinning for realtime vs analytics workloads
// ============================================================================
// Pins threads to specific CPU cores:
//   - Realtime cores (e.g. 0-3): ingestion, tick processing
//   - Analytics cores (e.g. 4-7): queries, aggregation, JOINs
//
// Uses pthread_setaffinity_np on Linux.
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

namespace apex::execution {

struct CpuSet {
    std::vector<int> cores;  // list of core IDs

    static CpuSet range(int from, int to) {
        CpuSet s;
        for (int i = from; i <= to; ++i) s.cores.push_back(i);
        return s;
    }

    static CpuSet single(int core) { return {{core}}; }

    bool empty() const { return cores.empty(); }
    size_t size() const { return cores.size(); }
};

struct IsolationConfig {
    CpuSet realtime_cores;   // ingestion threads
    CpuSet analytics_cores;  // query threads
    CpuSet drain_cores;      // drain thread(s)
};

class ResourceIsolation {
public:
    explicit ResourceIsolation(IsolationConfig cfg = {}) : config_(std::move(cfg)) {}

    /// Pin the calling thread to the realtime core set.
    bool pin_realtime();

    /// Pin the calling thread to the analytics core set.
    bool pin_analytics();

    /// Pin the calling thread to the drain core set.
    bool pin_drain();

    /// Pin the calling thread to a specific core.
    static bool pin_to_core(int core_id);

    /// Pin the calling thread to a set of cores.
    static bool pin_to_cores(const CpuSet& cores);

    /// Get the number of available CPU cores.
    static int num_cpus();

    const IsolationConfig& config() const { return config_; }

private:
    IsolationConfig config_;
};

} // namespace apex::execution
