#pragma once
// ============================================================================
// Layer 3: Vectorized Execution Engine
// ============================================================================
// Design reference: layer3_execution_engine.md
//   - DataBlock Pipeline (8192 rows)
//   - SIMD (Highway)-based filtering and aggregation
//   - Keep L1/L2 cache hot
//
// Phase B v2 optimizations:
//   - BitMask: replaces SelectionVector and stores filter results as bits
//   - filter_gt_i64_bitmask: writes StoreMaskBits directly into uint64_t words
//     without branches
//   - sum_i64_fast: manual 4-way unroll plus prefetch for compiler-independent
//     optimization
//   - sum_i64_simd_v2: 8-way SIMD unroll plus prefetch
//   - sum_i64_masked: bitmask-based selective sum using ctz(bits), no gather
//   - vwap_fused: single-pass 4x SIMD unroll plus prefetch for both arrays
// ============================================================================

#include "zeptodb/common/types.h"
#include "zeptodb/storage/column_store.h"

#include <functional>
#include <memory>
#include <vector>
#include <span>
#include <bit>       // popcount
#include <cstdint>

namespace zeptodb::execution {

// ============================================================================
// SelectionVector: filter result index array (v1, kept for compatibility)
// ============================================================================
class SelectionVector {
public:
    explicit SelectionVector(size_t max_size);

    void add(uint32_t idx) { indices_[size_++] = idx; }
    void reset() { size_ = 0; }

    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] const uint32_t* data() const { return indices_.get(); }
    [[nodiscard]] uint32_t operator[](size_t i) const { return indices_[i]; }

    // Set after SIMD filters write directly into indices_.
    void set_size(size_t n) { size_ = n; }

private:
    std::unique_ptr<uint32_t[]> indices_;
    size_t size_ = 0;
};

// ============================================================================
// BitMask: stores filter results as a bit array (v2, Phase B optimization)
//
// Design notes:
//   SelectionVector writes one uint32_t index per passing row, which costs
//   memory bandwidth and introduces branch pressure. BitMask ORs SIMD
//   StoreMaskBits output directly into uint64_t words:
//     1) Write bandwidth drops from N bytes of indexes to N/8 bytes of bits.
//     2) Aggregation uses ctz(bits), avoiding branch prediction pressure.
//     3) Cache footprint drops from about 4 MB of indexes to 128 KB of bits
//        for 1M rows.
// ============================================================================
class BitMask {
public:
    /// Create a zero-initialized bitmask for num_rows rows.
    explicit BitMask(size_t num_rows);

    /// Clear every bit.
    void clear();

    /// Set the i-th bit.
    void set(size_t i) {
        bits_[i >> 6] |= (1ULL << (i & 63));
    }

    /// Return whether the i-th bit is set.
    [[nodiscard]] bool test(size_t i) const {
        return (bits_[i >> 6] >> (i & 63)) & 1ULL;
    }

    /// Count set bits, which equals the number of passing rows.
    [[nodiscard]] size_t popcount() const;

    /// Access the backing uint64_t words for direct SIMD writes.
    [[nodiscard]] uint64_t* data() { return bits_.get(); }
    [[nodiscard]] const uint64_t* data() const { return bits_.get(); }

    [[nodiscard]] size_t num_rows()  const { return num_rows_; }
    [[nodiscard]] size_t num_words() const { return num_words_; }

private:
    std::unique_ptr<uint64_t[]> bits_;
    size_t num_rows_;
    size_t num_words_;
};

// ============================================================================
// Operators v1: existing compatibility interface
// ============================================================================

/// Scalar filter for one column, such as price > 10000.
/// Writes passing row indexes into SelectionVector.
void filter_gt_i64(
    const int64_t* column_data,
    size_t num_rows,
    int64_t threshold,
    SelectionVector& result
);

/// Sum one int64 column.
int64_t sum_i64(
    const int64_t* column_data,
    size_t num_rows
);

/// Sum rows selected by SelectionVector.
int64_t sum_i64_selected(
    const int64_t* column_data,
    const SelectionVector& selection
);

/// Compute VWAP: sum(price * volume) / sum(volume).
double vwap(
    const int64_t* prices,
    const int64_t* volumes,
    size_t num_rows
);

// ============================================================================
// Operators v2: Phase B optimized variants
// ============================================================================

/// Bitmask-based filter (v2).
/// Writes directly into BitMask instead of SelectionVector.
/// Avoids branches and improves filter-result cache efficiency by 8x.
void filter_gt_i64_bitmask(
    const int64_t* column_data,
    size_t num_rows,
    int64_t threshold,
    BitMask& result
);

/// Manual 4-way unrolled sum for scalar compiler-independent optimization.
/// Uses four accumulators to hide arithmetic latency, prefetches to hide memory
/// latency, and maximizes scalar ILP without relying on autovectorization.
[[nodiscard]] int64_t sum_i64_fast(
    const int64_t* column_data,
    size_t num_rows
);

/// 8-way SIMD-unrolled sum with prefetch (v2).
[[nodiscard]] int64_t sum_i64_simd_v2(
    const int64_t* column_data,
    size_t num_rows
);

/// Bitmask-based selective sum.
/// Uses ctz(bits) to sum only passing rows without gather. This is most
/// effective for low selectivity sparse filters.
[[nodiscard]] int64_t sum_i64_masked(
    const int64_t* column_data,
    const BitMask& mask
);

/// Fused VWAP pipeline (v2, __int128 integer accumulator).
/// Processes price and volume arrays in one pass, uses 4x SIMD unrolling with
/// prefetch for both arrays, and avoids integer overflow while minimizing
/// floating-point conversion.
[[nodiscard]] double vwap_fused(
    const int64_t* prices,
    const int64_t* volumes,
    size_t num_rows
);

// ============================================================================
// VectorizedEngine: query execution entry point (Layer 3 facade)
// ============================================================================
class VectorizedEngine {
public:
    VectorizedEngine() = default;

    // TODO: Expand once the query planner is integrated.
    // For now, only primitive vector operations are exposed.
};

} // namespace zeptodb::execution
