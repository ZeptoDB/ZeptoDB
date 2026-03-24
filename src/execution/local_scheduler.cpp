// ============================================================================
// ZeptoDB: LocalQueryScheduler Implementation
// ============================================================================

#include "zeptodb/execution/local_scheduler.h"
#include "zeptodb/execution/parallel_scan.h"
#include "zeptodb/storage/column_store.h"

#include <algorithm>
#include <climits>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace zeptodb::execution {

// ============================================================================
// PartialAggResult 헬퍼 구현
// ============================================================================

void PartialAggResult::resize(size_t ncols) {
    sum.assign(ncols, 0);
    count.assign(ncols, 0);
    d_sum.assign(ncols, 0.0);
    min_val.assign(ncols, INT64_MAX);
    max_val.assign(ncols, INT64_MIN);
    vwap_pv.assign(ncols, 0);
    vwap_vol.assign(ncols, 0);
    first_val.assign(ncols, 0);
    last_val.assign(ncols, 0);
    has_first.assign(ncols, false);
    rows_scanned = 0;
    group_partials.clear();
}

void PartialAggResult::merge(const PartialAggResult& other) {
    size_t ncols = sum.size();
    if (ncols == 0) {
        *this = other;
        return;
    }

    rows_scanned += other.rows_scanned;

    for (size_t i = 0; i < ncols && i < other.sum.size(); ++i) {
        sum[i]    += other.sum[i];
        count[i]  += other.count[i];
        d_sum[i]  += other.d_sum[i];
        min_val[i] = std::min(min_val[i], other.min_val[i]);
        max_val[i] = std::max(max_val[i], other.max_val[i]);
        vwap_pv[i]  += other.vwap_pv[i];
        vwap_vol[i] += other.vwap_vol[i];
        if (!has_first[i] && other.has_first[i]) {
            first_val[i] = other.first_val[i];
            has_first[i] = true;
        }
        if (other.has_first[i]) {
            last_val[i] = other.last_val[i];
        }
    }

    // GROUP BY: 재귀 머지
    for (const auto& [gkey, src_ptr] : other.group_partials) {
        if (!src_ptr) continue;
        auto it = group_partials.find(gkey);
        if (it == group_partials.end()) {
            group_partials[gkey] = std::make_shared<PartialAggResult>(*src_ptr);
        } else {
            if (it->second) it->second->merge(*src_ptr);
        }
    }
}

// 직렬화: 기본 구현 (분산 스케줄러 구현 시 확장 예정)
std::vector<uint8_t> PartialAggResult::serialize() const {
    // TODO(distributed): 실제 직렬화 구현
    // 현재는 데이터 크기만 인코딩하는 placeholder
    std::vector<uint8_t> buf;
    buf.reserve(64);
    // magic + ncols
    uint32_t ncols = static_cast<uint32_t>(sum.size());
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&ncols);
    buf.insert(buf.end(), p, p + sizeof(ncols));
    return buf;
}

PartialAggResult PartialAggResult::deserialize(const uint8_t* /*buf*/, size_t /*len*/) {
    // TODO(distributed): 실제 역직렬화 구현
    return {};
}

// ============================================================================
// LocalQueryScheduler
// ============================================================================

LocalQueryScheduler::LocalQueryScheduler(zeptodb::core::ZeptoPipeline& pipeline,
                                          size_t num_threads)
    : pipeline_(pipeline)
    , pool_(num_threads)
{}

// ============================================================================
// execute_fragment: 단일 Fragment 실행 (WorkerPool 태스크 내부)
// ============================================================================
// 단순 필터(symbol, time range)를 적용하고 집계를 계산.
// 복잡한 WHERE 조건(eval_expr)은 executor 레벨에서 처리.
// ============================================================================
PartialAggResult LocalQueryScheduler::execute_fragment(const QueryFragment& frag) {
    auto& pm = pipeline_.partition_manager();
    auto all_parts = pm.get_all_partitions();

    size_t ncols = frag.agg_types.size();
    PartialAggResult result;
    result.resize(ncols);

    for (uint32_t pid : frag.partition_ids) {
        if (pid >= static_cast<uint32_t>(all_parts.size())) continue;
        auto* part = all_parts[pid];
        if (!part) continue;

        // 심볼 필터
        if (frag.symbol_filter >= 0 &&
            static_cast<int64_t>(part->key().symbol_id) != frag.symbol_filter)
            continue;

        size_t n = part->num_rows();

        // 타임스탬프 범위 필터
        size_t row_begin = frag.row_begin;
        size_t row_end   = std::min(frag.row_end, n);

        bool use_ts = (frag.ts_lo != INT64_MIN || frag.ts_hi != INT64_MAX);
        if (use_ts) {
            if (!part->overlaps_time_range(frag.ts_lo, frag.ts_hi)) continue;
            auto [rb, re] = part->timestamp_range(frag.ts_lo, frag.ts_hi);
            row_begin = std::max(row_begin, rb);
            row_end   = std::min(row_end, re);
        }

        if (row_begin >= row_end) continue;
        result.rows_scanned += row_end - row_begin;

        // GROUP BY 처리
        if (frag.has_group_by) {
            const int64_t* gdata = nullptr;
            int64_t symbol_gkey = 0;
            bool is_sym_group = (frag.group_by_column == "symbol");
            if (is_sym_group) {
                symbol_gkey = static_cast<int64_t>(part->key().symbol_id);
            } else {
                const auto* cv = part->get_column(frag.group_by_column);
                if (!cv) continue;
                gdata = static_cast<const int64_t*>(cv->raw_data());
            }

            for (size_t ri = row_begin; ri < row_end; ++ri) {
                int64_t gkey = is_sym_group ? symbol_gkey : (gdata ? gdata[ri] : 0);
                if (frag.group_xbar_bucket > 0) {
                    gkey = (gkey / frag.group_xbar_bucket) * frag.group_xbar_bucket;
                }

                auto& sub_ptr = result.group_partials[gkey];
                if (!sub_ptr) {
                    sub_ptr = std::make_shared<PartialAggResult>();
                    sub_ptr->resize(ncols);
                }
                auto& sub = *sub_ptr;

                for (size_t ci = 0; ci < ncols; ++ci) {
                    if (ci >= frag.agg_types.size()) break;
                    AggType at = frag.agg_types[ci];
                    if (at == AggType::NONE) continue;
                    const std::string& col_name = ci < frag.agg_columns.size()
                        ? frag.agg_columns[ci] : "";
                    const auto* cv = col_name.empty() ? nullptr
                        : part->get_column(col_name);
                    const int64_t* data = cv
                        ? static_cast<const int64_t*>(cv->raw_data()) : nullptr;

                    switch (at) {
                        case AggType::COUNT: sub.count[ci]++; break;
                        case AggType::SUM:
                            if (data) sub.sum[ci] += data[ri]; break;
                        case AggType::AVG:
                            if (data) {
                                sub.d_sum[ci] += static_cast<double>(data[ri]);
                                sub.count[ci]++;
                            } break;
                        case AggType::MIN:
                            if (data)
                                sub.min_val[ci] = std::min(sub.min_val[ci], data[ri]);
                            break;
                        case AggType::MAX:
                            if (data)
                                sub.max_val[ci] = std::max(sub.max_val[ci], data[ri]);
                            break;
                        case AggType::VWAP: {
                            const std::string& v_col = ci < frag.agg_args2.size()
                                ? frag.agg_args2[ci] : "";
                            const auto* vcv = v_col.empty() ? nullptr
                                : part->get_column(v_col);
                            const int64_t* vd = vcv
                                ? static_cast<const int64_t*>(vcv->raw_data()) : nullptr;
                            if (data && vd) {
                                sub.vwap_pv[ci]  += data[ri] * vd[ri];
                                sub.vwap_vol[ci] += vd[ri];
                            }
                            break;
                        }
                        case AggType::FIRST:
                        case AggType::LAST:
                            if (data) {
                                if (!sub.has_first[ci]) {
                                    sub.first_val[ci] = data[ri];
                                    sub.has_first[ci] = true;
                                }
                                sub.last_val[ci] = data[ri];
                            } break;
                        case AggType::XBAR:
                            if (data && !sub.has_first[ci]) {
                                int64_t b = ci < frag.xbar_buckets.size()
                                    ? frag.xbar_buckets[ci] : 0;
                                sub.first_val[ci] = b > 0
                                    ? (data[ri] / b) * b : data[ri];
                                sub.has_first[ci] = true;
                            } break;
                        default: break;
                    }
                }
            }
            continue; // next partition
        }

        // 일반 집계 (GROUP BY 없음)
        for (size_t ci = 0; ci < ncols; ++ci) {
            if (ci >= frag.agg_types.size()) break;
            AggType at = frag.agg_types[ci];
            if (at == AggType::NONE) continue;
            const std::string& col_name = ci < frag.agg_columns.size()
                ? frag.agg_columns[ci] : "";
            const auto* cv = col_name.empty() ? nullptr
                : part->get_column(col_name);
            const int64_t* data = cv
                ? static_cast<const int64_t*>(cv->raw_data()) : nullptr;

            for (size_t ri = row_begin; ri < row_end; ++ri) {
                switch (at) {
                    case AggType::COUNT: result.count[ci]++; break;
                    case AggType::SUM:
                        if (data) result.sum[ci] += data[ri]; break;
                    case AggType::AVG:
                        if (data) {
                            result.d_sum[ci] += static_cast<double>(data[ri]);
                            result.count[ci]++;
                        } break;
                    case AggType::MIN:
                        if (data)
                            result.min_val[ci] = std::min(result.min_val[ci], data[ri]);
                        break;
                    case AggType::MAX:
                        if (data)
                            result.max_val[ci] = std::max(result.max_val[ci], data[ri]);
                        break;
                    case AggType::VWAP: {
                        const std::string& v_col = ci < frag.agg_args2.size()
                            ? frag.agg_args2[ci] : "";
                        const auto* vcv = v_col.empty() ? nullptr
                            : part->get_column(v_col);
                        const int64_t* vd = vcv
                            ? static_cast<const int64_t*>(vcv->raw_data()) : nullptr;
                        if (data && vd) {
                            result.vwap_pv[ci]  += data[ri] * vd[ri];
                            result.vwap_vol[ci] += vd[ri];
                        }
                        break;
                    }
                    case AggType::FIRST:
                    case AggType::LAST:
                        if (data) {
                            if (!result.has_first[ci]) {
                                result.first_val[ci] = data[ri];
                                result.has_first[ci] = true;
                            }
                            result.last_val[ci] = data[ri];
                        } break;
                    case AggType::XBAR:
                        if (data && !result.has_first[ci]) {
                            int64_t b = ci < frag.xbar_buckets.size()
                                ? frag.xbar_buckets[ci] : 0;
                            result.first_val[ci] = b > 0
                                ? (data[ri] / b) * b : data[ri];
                            result.has_first[ci] = true;
                        } break;
                    default: break;
                }
            }
        }
    }
    return result;
}

// ============================================================================
// scatter: Fragment 목록을 WorkerPool 으로 분산 실행
// ============================================================================
std::vector<PartialAggResult> LocalQueryScheduler::scatter(
    const std::vector<QueryFragment>& fragments)
{
    if (fragments.empty()) return {};

    size_t n = fragments.size();
    std::vector<PartialAggResult> results(n);

    std::latch done(static_cast<ptrdiff_t>(n));
    for (size_t i = 0; i < n; ++i) {
        pool_.submit(
            [this, &fragments, &results, i, &done]() {
                results[i] = execute_fragment(fragments[i]);
                done.count_down();
            },
            WorkerPool::Priority::HIGH
        );
    }
    done.wait();
    return results;
}

// ============================================================================
// gather: 부분 결과들을 단일 PartialAggResult 로 머지
// ============================================================================
PartialAggResult LocalQueryScheduler::gather(
    std::vector<PartialAggResult>&& partials)
{
    if (partials.empty()) return {};

    PartialAggResult merged = std::move(partials[0]);
    for (size_t i = 1; i < partials.size(); ++i) {
        merged.merge(partials[i]);
    }
    return merged;
}

} // namespace zeptodb::execution
