// ============================================================================
// Layer 3: Vectorized Engine Implementation (Highway SIMD)
// ============================================================================
// Phase B: Highway SIMD 실제 구현
// - AVX-512 지원 서버에서 자동으로 최적 타겟 선택됨
// - filter_gt_i64: StoreMaskBits 기반 mask-based filtering
// - sum_i64: ReduceSum + vectorized accumulation (8x unroll)
// - vwap: f64 MulAdd pipeline
// ============================================================================

// Highway multi-target dispatch 패턴
// foreach_target.h가 이 파일을 여러 번 include함 (SSE4, AVX2, AVX512 등)
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "execution/vectorized_engine.cpp"

#include <hwy/foreach_target.h>
#include <hwy/highway.h>

#include "apex/execution/vectorized_engine.h"
#include "apex/common/logger.h"

HWY_BEFORE_NAMESPACE();

namespace apex::execution {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

// ============================================================================
// SIMD: sum_i64 — 8x unroll + ReduceSum
// ============================================================================
int64_t sum_i64_simd(const int64_t* data, size_t n) {
    const hn::ScalableTag<int64_t> d;
    const size_t N = hn::Lanes(d);

    auto acc0 = hn::Zero(d);
    auto acc1 = hn::Zero(d);
    auto acc2 = hn::Zero(d);
    auto acc3 = hn::Zero(d);

    size_t i = 0;
    const size_t N4 = N * 4;
    for (; i + N4 <= n; i += N4) {
        acc0 = hn::Add(acc0, hn::LoadU(d, data + i + 0 * N));
        acc1 = hn::Add(acc1, hn::LoadU(d, data + i + 1 * N));
        acc2 = hn::Add(acc2, hn::LoadU(d, data + i + 2 * N));
        acc3 = hn::Add(acc3, hn::LoadU(d, data + i + 3 * N));
    }

    acc0 = hn::Add(hn::Add(acc0, acc1), hn::Add(acc2, acc3));

    for (; i + N <= n; i += N) {
        acc0 = hn::Add(acc0, hn::LoadU(d, data + i));
    }

    int64_t result = hn::ReduceSum(d, acc0);

    // scalar tail
    for (; i < n; ++i) {
        result += data[i];
    }

    return result;
}

// ============================================================================
// SIMD: filter_gt_i64 — StoreMaskBits 기반
// ============================================================================
size_t filter_gt_i64_simd(
    const int64_t* column_data,
    size_t num_rows,
    int64_t threshold,
    uint32_t* out_indices
) {
    const hn::ScalableTag<int64_t> d;
    const size_t N = hn::Lanes(d);

    const auto thresh_vec = hn::Set(d, threshold);
    size_t out_count = 0;
    size_t i = 0;

    // StoreMaskBits가 필요한 바이트 수: ceil(N/8)
    // AVX-512 i64: N=8, 1 byte면 충분
    // AVX2 i64:    N=4, 1 byte
    // SSE4 i64:    N=2, 1 byte
    alignas(8) uint8_t mask_bytes[8] = {};  // 최대 64개 lane / 8

    for (; i + N <= num_rows; i += N) {
        const auto vals = hn::LoadU(d, column_data + i);
        const auto mask = hn::Gt(vals, thresh_vec);

        // mask → 비트로 변환
        hn::StoreMaskBits(d, mask, mask_bytes);
        uint64_t bits = 0;
        // N/8 바이트만 읽어서 합침
        const size_t nbytes = (N + 7) / 8;
        for (size_t b = 0; b < nbytes; ++b) {
            bits |= (static_cast<uint64_t>(mask_bytes[b]) << (b * 8));
        }

        // 비트 순회하며 인덱스 기록
        while (bits) {
            int k = __builtin_ctzll(bits);
            out_indices[out_count++] = static_cast<uint32_t>(i + static_cast<size_t>(k));
            bits &= bits - 1;
        }
    }

    // scalar tail
    for (; i < num_rows; ++i) {
        if (column_data[i] > threshold) {
            out_indices[out_count++] = static_cast<uint32_t>(i);
        }
    }

    return out_count;
}

// ============================================================================
// SIMD: vwap — f64 MulAdd 파이프라인
// ============================================================================
double vwap_simd(const int64_t* prices, const int64_t* volumes, size_t n) {
    const hn::ScalableTag<int64_t> di;
    const hn::ScalableTag<double> df;
    const size_t N = hn::Lanes(di);

    auto pv_acc0 = hn::Zero(df);
    auto pv_acc1 = hn::Zero(df);
    auto v_acc0  = hn::Zero(df);
    auto v_acc1  = hn::Zero(df);

    size_t i = 0;
    const size_t N2 = N * 2;
    for (; i + N2 <= n; i += N2) {
        const auto p0 = hn::ConvertTo(df, hn::LoadU(di, prices  + i));
        const auto v0 = hn::ConvertTo(df, hn::LoadU(di, volumes + i));
        const auto p1 = hn::ConvertTo(df, hn::LoadU(di, prices  + i + N));
        const auto v1 = hn::ConvertTo(df, hn::LoadU(di, volumes + i + N));

        pv_acc0 = hn::MulAdd(p0, v0, pv_acc0);
        pv_acc1 = hn::MulAdd(p1, v1, pv_acc1);
        v_acc0  = hn::Add(v_acc0, v0);
        v_acc1  = hn::Add(v_acc1, v1);
    }

    pv_acc0 = hn::Add(pv_acc0, pv_acc1);
    v_acc0  = hn::Add(v_acc0,  v_acc1);

    for (; i + N <= n; i += N) {
        const auto p = hn::ConvertTo(df, hn::LoadU(di, prices  + i));
        const auto v = hn::ConvertTo(df, hn::LoadU(di, volumes + i));
        pv_acc0 = hn::MulAdd(p, v, pv_acc0);
        v_acc0  = hn::Add(v_acc0, v);
    }

    double total_pv = hn::ReduceSum(df, pv_acc0);
    double total_v  = hn::ReduceSum(df, v_acc0);

    // scalar tail
    for (; i < n; ++i) {
        total_pv += static_cast<double>(prices[i]) * static_cast<double>(volumes[i]);
        total_v  += static_cast<double>(volumes[i]);
    }

    if (total_v == 0.0) return 0.0;
    return total_pv / total_v;
}

}  // namespace HWY_NAMESPACE
}  // namespace apex::execution

HWY_AFTER_NAMESPACE();

// ============================================================================
// HWY_ONCE: Dispatch table + public API
// ============================================================================
#if HWY_ONCE

namespace apex::execution {

HWY_EXPORT(sum_i64_simd);
HWY_EXPORT(filter_gt_i64_simd);
HWY_EXPORT(vwap_simd);

// ============================================================================
// SelectionVector
// ============================================================================
SelectionVector::SelectionVector(size_t max_size)
    : indices_(std::make_unique<uint32_t[]>(max_size))
{
}

// ============================================================================
// Public API
// ============================================================================

void filter_gt_i64(
    const int64_t* column_data,
    size_t num_rows,
    int64_t threshold,
    SelectionVector& result
) {
    result.reset();
    size_t count = HWY_DYNAMIC_DISPATCH(filter_gt_i64_simd)(
        column_data, num_rows, threshold,
        const_cast<uint32_t*>(result.data())
    );
    result.set_size(count);
}

int64_t sum_i64(const int64_t* column_data, size_t num_rows) {
    return HWY_DYNAMIC_DISPATCH(sum_i64_simd)(column_data, num_rows);
}

int64_t sum_i64_selected(
    const int64_t* column_data,
    const SelectionVector& selection
) {
    // gather 연산은 scatter/gather가 필요하고 구현 복잡도 증가
    // 선택된 행 수가 상대적으로 적으면 scalar가 더 빠름 → scalar 유지
    int64_t total = 0;
    for (size_t i = 0; i < selection.size(); ++i) {
        total += column_data[selection[i]];
    }
    return total;
}

double vwap(
    const int64_t* prices,
    const int64_t* volumes,
    size_t num_rows
) {
    return HWY_DYNAMIC_DISPATCH(vwap_simd)(prices, volumes, num_rows);
}

}  // namespace apex::execution

#endif  // HWY_ONCE
