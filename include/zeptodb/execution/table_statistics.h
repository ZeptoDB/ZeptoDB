#pragma once
// ============================================================================
// Layer 3: Table Statistics — Per-partition, per-column statistics
// ============================================================================
// Design doc: docs/design/cost_based_planner.md §3.1
//   - Incremental min/max/count on every append — O(1) per row
//   - HyperLogLog for approximate distinct count (64 registers, 64 bytes)
//   - Seal snapshot: frozen on partition seal
// ============================================================================

#include "zeptodb/storage/partition_manager.h"
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace zeptodb::execution {

// ============================================================================
// ColumnStats: per-column statistics with HyperLogLog distinct estimation
// ============================================================================
struct ColumnStats {
    int64_t min_val = std::numeric_limits<int64_t>::max();
    int64_t max_val = std::numeric_limits<int64_t>::min();
    size_t  row_count = 0;
    size_t  null_count = 0;
    size_t  distinct_approx = 0;

    /// O(1) incremental update
    void update(int64_t value);

    /// Merge another ColumnStats (cross-partition aggregation)
    void merge(const ColumnStats& other);

private:
    // HyperLogLog: 64 registers (m=64), 6 bits each stored as uint8_t
    static constexpr size_t HLL_M = 64;
    static constexpr size_t HLL_P = 6;  // log2(64)
    uint8_t hll_registers_[HLL_M] = {};

    static uint64_t hll_hash(int64_t value);
    void hll_add(int64_t value);
    void hll_merge(const uint8_t other[HLL_M]);
    size_t hll_estimate() const;
};

// ============================================================================
// PartitionStats: per-partition statistics
// ============================================================================
struct PartitionStats {
    size_t  row_count = 0;
    int64_t ts_min = std::numeric_limits<int64_t>::max();
    int64_t ts_max = std::numeric_limits<int64_t>::min();
    std::unordered_map<std::string, ColumnStats> column_stats;
    bool sealed = false;

    /// Increment row count (call once per row, before record_append per column)
    void record_row() { if (!sealed) ++row_count; }

    /// Update column stats on append (no-op if sealed). Does NOT increment row_count.
    void record_append(const std::string& col, int64_t value);

    /// Freeze stats
    void seal();
};

// ============================================================================
// TableStatistics: aggregated statistics across partitions (thread-safe)
// ============================================================================
class TableStatistics {
public:
    /// Recompute stats for a partition from its column data
    void update_partition(const storage::Partition* part);

    /// Merge all partition stats into one aggregate
    PartitionStats aggregate(const std::string& table) const;

    /// Total row count across all partitions
    size_t estimate_rows(const std::string& table) const;

    /// Check if a partition is already tracked (thread-safe)
    bool has_partition(const storage::Partition* part) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return partition_stats_.find(part) != partition_stats_.end();
    }

    /// Direct access to partition stats (for testing — NOT thread-safe)
    const std::unordered_map<const storage::Partition*, PartitionStats>&
    partition_stats() const { return partition_stats_; }

private:
    mutable std::mutex mutex_;
    std::unordered_map<const storage::Partition*, PartitionStats> partition_stats_;
};

} // namespace zeptodb::execution
