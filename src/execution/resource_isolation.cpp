#include "zeptodb/execution/resource_isolation.h"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
#include <thread>

namespace zeptodb::execution {

bool ResourceIsolation::pin_realtime() {
    return pin_to_cores(config_.realtime_cores);
}

bool ResourceIsolation::pin_analytics() {
    return pin_to_cores(config_.analytics_cores);
}

bool ResourceIsolation::pin_drain() {
    return pin_to_cores(config_.drain_cores);
}

bool ResourceIsolation::pin_to_core(int core_id) {
    return pin_to_cores(CpuSet::single(core_id));
}

bool ResourceIsolation::pin_to_cores(const CpuSet& cores) {
    if (cores.empty()) return false;
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int c : cores.cores) CPU_SET(c, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0;
#else
    (void)cores;
    return false;  // not supported on non-Linux
#endif
}

int ResourceIsolation::num_cpus() {
    return static_cast<int>(std::thread::hardware_concurrency());
}

} // namespace zeptodb::execution
