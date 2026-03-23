#pragma once
// ============================================================================
// APEX-DB: Hot Symbol Detection & Rebalancing
// ============================================================================
// Detects symbols with disproportionate ingestion traffic (e.g. AAPL/SPY
// receiving 10x average) and provides pinning to dedicated nodes.
//
// Usage:
//   detector.record(symbol_id);          // call on every tick
//   auto hot = detector.detect_hot();    // periodic check
//   for (auto& [sym, rate] : hot)
//       router.pin_symbol(sym, dedicated_node);
// ============================================================================

#include "apex/common/types.h"
#include "apex/cluster/partition_router.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace apex::cluster {

struct HotSymbolInfo {
    SymbolId symbol;
    uint64_t tick_count;    // ticks in current window
    double   ratio;         // tick_count / average
};

// ============================================================================
// HotSymbolDetector: sliding-window per-symbol tick counter
// ============================================================================
class HotSymbolDetector {
public:
    /// @param threshold_ratio  symbol is "hot" if its rate > threshold × average
    /// @param min_ticks        minimum ticks to be considered (avoid noise)
    explicit HotSymbolDetector(double threshold_ratio = 5.0,
                               uint64_t min_ticks = 1000)
        : threshold_(threshold_ratio), min_ticks_(min_ticks) {}

    /// Record one tick for a symbol (called on ingestion path — must be fast)
    void record(SymbolId sym) {
        std::lock_guard<std::mutex> lock(mu_);
        counts_[sym]++;
        total_++;
    }

    /// Detect hot symbols: returns symbols exceeding threshold × average rate.
    /// Resets counters after detection (sliding window).
    std::vector<HotSymbolInfo> detect_hot() {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<HotSymbolInfo> result;
        if (counts_.empty() || total_ == 0) return result;

        double avg = static_cast<double>(total_) / counts_.size();

        for (auto& [sym, cnt] : counts_) {
            if (cnt >= min_ticks_) {
                double ratio = static_cast<double>(cnt) / avg;
                if (ratio >= threshold_) {
                    result.push_back({sym, cnt, ratio});
                }
            }
        }

        // Reset for next window
        counts_.clear();
        total_ = 0;
        return result;
    }

    /// Snapshot current counts without resetting (for monitoring)
    std::vector<HotSymbolInfo> snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<HotSymbolInfo> result;
        if (counts_.empty() || total_ == 0) return result;

        double avg = static_cast<double>(total_) / counts_.size();
        for (auto& [sym, cnt] : counts_) {
            double ratio = avg > 0 ? static_cast<double>(cnt) / avg : 0;
            result.push_back({sym, cnt, ratio});
        }
        return result;
    }

    double threshold() const { return threshold_; }
    void set_threshold(double t) { threshold_ = t; }

private:
    double   threshold_;
    uint64_t min_ticks_;
    mutable std::mutex mu_;
    std::unordered_map<SymbolId, uint64_t> counts_;
    uint64_t total_ = 0;
};

} // namespace apex::cluster
