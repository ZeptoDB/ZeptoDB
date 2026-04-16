#pragma once
// ============================================================================
// Layer 3: Cost Model — Selectivity estimation and cost calculation
// ============================================================================
// Design doc: docs/design/cost_based_planner.md §3.2
//   - Abstract cost units (proportional, not real nanoseconds)
//   - Selectivity: EQ, range, IN, AND, OR
//   - Cost: seq scan, index scan, hash join, sort, aggregate
// ============================================================================

#include "zeptodb/execution/table_statistics.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace zeptodb::execution {

// Cost constants (abstract units)
inline constexpr double SEQ_COST     = 1.0;
inline constexpr double RANDOM_COST  = 4.0;
inline constexpr double INDEX_PROBE  = 0.5;
inline constexpr double HASH_BUILD   = 1.5;
inline constexpr double HASH_PROBE   = 0.5;
inline constexpr double SORT_COST    = 2.0;
inline constexpr double AGG_COST     = 1.0;

// ============================================================================
// CostEstimate
// ============================================================================
struct CostEstimate {
    double io_cost  = 0.0;
    double cpu_cost = 0.0;
    size_t est_rows = 0;
    double total() const { return io_cost + cpu_cost; }
};

// ============================================================================
// CostModel: selectivity estimation + cost functions
// ============================================================================
class CostModel {
public:
    // --- Selectivity estimation ---

    /// Equality: 1 / distinct_approx (min 1/row_count)
    static double selectivity_eq(const ColumnStats& stats);

    /// Range [lo, hi]: (hi - lo) / (max - min), clamped [0, 1]
    static double selectivity_range(const ColumnStats& stats, int64_t lo, int64_t hi);

    /// IN list: list_size / distinct_approx, clamped [0, 1]
    static double selectivity_in(const ColumnStats& stats, size_t list_size);

    /// AND: multiply
    static double selectivity_and(double a, double b) { return a * b; }

    /// OR: a + b - a*b
    static double selectivity_or(double a, double b) { return a + b - a * b; }

    // --- Cost functions ---

    static CostEstimate estimate_seq_scan(size_t rows, size_t num_columns);
    static CostEstimate estimate_index_scan(size_t total_rows, double selectivity);
    static CostEstimate estimate_hash_join(size_t build_rows, size_t probe_rows);
    static CostEstimate estimate_sort(size_t rows);
    static CostEstimate estimate_aggregate(size_t rows, size_t groups);

    /// True if index scan is cheaper than seq scan
    static bool prefer_index_scan(size_t total_rows, double selectivity);
};

} // namespace zeptodb::execution
