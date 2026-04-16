#include "zeptodb/execution/cost_model.h"

namespace zeptodb::execution {

double CostModel::selectivity_eq(const ColumnStats& stats) {
    if (stats.distinct_approx == 0) return 1.0;
    double sel = 1.0 / static_cast<double>(stats.distinct_approx);
    if (stats.row_count > 0) {
        double min_sel = 1.0 / static_cast<double>(stats.row_count);
        if (sel < min_sel) sel = min_sel;
    }
    return sel;
}

double CostModel::selectivity_range(const ColumnStats& stats, int64_t lo, int64_t hi) {
    if (stats.max_val <= stats.min_val) return 1.0;
    double range = static_cast<double>(hi) - static_cast<double>(lo);
    double total = static_cast<double>(stats.max_val) - static_cast<double>(stats.min_val);
    return std::clamp(range / total, 0.0, 1.0);
}

double CostModel::selectivity_in(const ColumnStats& stats, size_t list_size) {
    if (stats.distinct_approx == 0) return 1.0;
    double sel = static_cast<double>(list_size) / static_cast<double>(stats.distinct_approx);
    return std::clamp(sel, 0.0, 1.0);
}

CostEstimate CostModel::estimate_seq_scan(size_t rows, size_t num_columns) {
    CostEstimate e;
    e.io_cost = static_cast<double>(rows) * static_cast<double>(num_columns) * SEQ_COST;
    e.est_rows = rows;
    return e;
}

CostEstimate CostModel::estimate_index_scan(size_t total_rows, double selectivity) {
    CostEstimate e;
    double log_rows = (total_rows > 1) ? std::log2(static_cast<double>(total_rows)) : 1.0;
    e.io_cost = log_rows * INDEX_PROBE + selectivity * static_cast<double>(total_rows) * RANDOM_COST;
    e.est_rows = static_cast<size_t>(selectivity * static_cast<double>(total_rows) + 0.5);
    return e;
}

CostEstimate CostModel::estimate_hash_join(size_t build_rows, size_t probe_rows) {
    CostEstimate e;
    e.cpu_cost = static_cast<double>(build_rows) * HASH_BUILD
               + static_cast<double>(probe_rows) * HASH_PROBE;
    e.est_rows = probe_rows;  // upper bound (1:1 assumption)
    return e;
}

CostEstimate CostModel::estimate_sort(size_t rows) {
    CostEstimate e;
    double log_rows = (rows > 1) ? std::log2(static_cast<double>(rows)) : 1.0;
    e.cpu_cost = static_cast<double>(rows) * log_rows * SORT_COST;
    e.est_rows = rows;
    return e;
}

CostEstimate CostModel::estimate_aggregate(size_t rows, size_t groups) {
    CostEstimate e;
    e.cpu_cost = static_cast<double>(rows) * AGG_COST;
    e.est_rows = (groups > 0) ? groups : rows;
    return e;
}

bool CostModel::prefer_index_scan(size_t total_rows, double selectivity) {
    auto seq = estimate_seq_scan(total_rows, 1);
    auto idx = estimate_index_scan(total_rows, selectivity);
    return idx.total() < seq.total();
}

} // namespace zeptodb::execution
