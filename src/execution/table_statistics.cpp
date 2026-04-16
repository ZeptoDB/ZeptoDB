#include "zeptodb/execution/table_statistics.h"
#include "zeptodb/common/logger.h"
#include <bit>
#include <cmath>

namespace zeptodb::execution {

// ============================================================================
// ColumnStats — HyperLogLog helpers
// ============================================================================

uint64_t ColumnStats::hll_hash(int64_t value) {
    // FNV-1a 64-bit
    uint64_t h = 14695981039346656037ULL;
    auto* p = reinterpret_cast<const uint8_t*>(&value);
    for (size_t i = 0; i < 8; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

void ColumnStats::hll_add(int64_t value) {
    uint64_t h = hll_hash(value);
    size_t idx = h >> (64 - HLL_P);  // top 6 bits → register index
    uint64_t w = h << HLL_P | (1ULL << (HLL_P - 1));  // remaining bits
    uint8_t rho = static_cast<uint8_t>(std::countl_zero(w) + 1);
    if (rho > hll_registers_[idx]) hll_registers_[idx] = rho;
}

void ColumnStats::hll_merge(const uint8_t other[HLL_M]) {
    for (size_t i = 0; i < HLL_M; ++i) {
        if (other[i] > hll_registers_[i]) hll_registers_[i] = other[i];
    }
}

size_t ColumnStats::hll_estimate() const {
    // alpha_m for m=64
    constexpr double alpha = 0.709;
    double sum = 0.0;
    size_t zeros = 0;
    for (size_t i = 0; i < HLL_M; ++i) {
        sum += std::pow(2.0, -static_cast<double>(hll_registers_[i]));
        if (hll_registers_[i] == 0) ++zeros;
    }
    double est = alpha * HLL_M * HLL_M / sum;
    // Small range correction
    if (est <= 2.5 * HLL_M && zeros > 0) {
        est = HLL_M * std::log(static_cast<double>(HLL_M) / zeros);
    }
    return static_cast<size_t>(est + 0.5);
}

void ColumnStats::update(int64_t value) {
    if (value < min_val) min_val = value;
    if (value > max_val) max_val = value;
    ++row_count;
    hll_add(value);
    distinct_approx = hll_estimate();
}

void ColumnStats::merge(const ColumnStats& other) {
    if (other.row_count == 0) return;
    if (other.min_val < min_val) min_val = other.min_val;
    if (other.max_val > max_val) max_val = other.max_val;
    row_count += other.row_count;
    null_count += other.null_count;
    hll_merge(other.hll_registers_);
    distinct_approx = hll_estimate();
}

// ============================================================================
// PartitionStats
// ============================================================================

void PartitionStats::record_append(const std::string& col, int64_t value) {
    if (sealed) return;
    column_stats[col].update(value);
    if (col == "timestamp") {
        if (value < ts_min) ts_min = value;
        if (value > ts_max) ts_max = value;
    }
}

void PartitionStats::seal() {
    sealed = true;
}

// ============================================================================
// TableStatistics
// ============================================================================

void TableStatistics::update_partition(const storage::Partition* part) {
    if (!part) return;
    std::lock_guard<std::mutex> lock(mutex_);

    PartitionStats ps;
    ps.row_count = part->num_rows();
    if (ps.row_count == 0) {
        partition_stats_[part] = ps;
        return;
    }

    for (const auto& col_ptr : part->columns()) {
        if (!col_ptr || col_ptr->size() == 0) continue;
        auto span = const_cast<storage::ColumnVector*>(col_ptr.get())->as_span<int64_t>();
        auto& cs = ps.column_stats[col_ptr->name()];
        for (size_t i = 0; i < span.size(); ++i) {
            cs.update(span[i]);
        }
    }

    // Timestamp range from the timestamp column
    const auto* ts_col = part->get_column("timestamp");
    if (ts_col && ts_col->size() > 0) {
        auto ts_span = const_cast<storage::ColumnVector*>(ts_col)->as_span<int64_t>();
        ps.ts_min = ts_span.front();
        ps.ts_max = ts_span.back();
    }

    if (part->state() == storage::Partition::State::SEALED) {
        ps.sealed = true;
    }

    size_t rows = ps.row_count;
    partition_stats_[part] = std::move(ps);
    ZEPTO_INFO("TableStatistics: updated partition, rows={}", rows);
}

PartitionStats TableStatistics::aggregate(const std::string& /*table*/) const {
    std::lock_guard<std::mutex> lock(mutex_);
    PartitionStats agg;
    for (const auto& [_, ps] : partition_stats_) {
        agg.row_count += ps.row_count;
        if (ps.ts_min < agg.ts_min) agg.ts_min = ps.ts_min;
        if (ps.ts_max > agg.ts_max) agg.ts_max = ps.ts_max;
        for (const auto& [col, cs] : ps.column_stats) {
            agg.column_stats[col].merge(cs);
        }
    }
    return agg;
}

size_t TableStatistics::estimate_rows(const std::string& /*table*/) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& [_, ps] : partition_stats_) {
        total += ps.row_count;
    }
    return total;
}

} // namespace zeptodb::execution
