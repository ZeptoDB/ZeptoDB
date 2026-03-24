// ============================================================================
// Layer 3: Vectorized Engine Implementation (Highway SIMD)
// ============================================================================
// Phase B v1: Highway SIMD 기본 구현
// Phase B v2: 추가 최적화
//   - filter_gt_i64_bitmask: StoreMaskBits → uint64_t 직접 기록
//   - sum_i64_fast: 스칼라 4-way 언롤 + prefetch (컴파일러 독립)
//   - sum_i64_simd_v2: 8-way SIMD 언롤 + prefetch
//   - sum_i64_masked: ctz 기반 sparse 합계
//   - vwap_fused: 4x 언롤 + 양 배열 prefetch
// ============================================================================

// Highway multi-target dispatch 패턴
// foreach_target.h가 이 파일을 여러 번 include함 (SSE4, AVX2, AVX512 등)
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "execution/vectorized_engine.cpp"

#include <hwy/foreach_target.h>
#include <hwy/highway.h>
#include <hwy/cache_control.h>

#include "zeptodb/execution/vectorized_engine.h"
#include "zeptodb/common/logger.h"

HWY_BEFORE_NAMESPACE();

namespace zeptodb::execution {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

// ============================================================================
// SIMD: sum_i64 v1 — 4x unroll + ReduceSum
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
// SIMD: sum_i64 v2 — 8x unroll + prefetch
// 근거: 4x 언롤 대비 ROB(Re-order Buffer)를 더 채워 메모리 레이턴시 숨기기
//       prefetch: 현재 위치 + 512바이트(L2 latency ~12cycle 가정)
// ============================================================================
int64_t sum_i64_simd_v2_impl(const int64_t* data, size_t n) {
    const hn::ScalableTag<int64_t> d;
    const size_t N = hn::Lanes(d);

    auto acc0 = hn::Zero(d);
    auto acc1 = hn::Zero(d);
    auto acc2 = hn::Zero(d);
    auto acc3 = hn::Zero(d);
    auto acc4 = hn::Zero(d);
    auto acc5 = hn::Zero(d);
    auto acc6 = hn::Zero(d);
    auto acc7 = hn::Zero(d);

    size_t i = 0;
    const size_t N8 = N * 8;
    // prefetch stride: 64 elements × 8 bytes = 512 bytes ≈ 8 cache lines ahead
    const size_t PREFETCH_STRIDE = 64;

    for (; i + N8 <= n; i += N8) {
        hwy::Prefetch(data + i + PREFETCH_STRIDE);
        acc0 = hn::Add(acc0, hn::LoadU(d, data + i + 0 * N));
        acc1 = hn::Add(acc1, hn::LoadU(d, data + i + 1 * N));
        acc2 = hn::Add(acc2, hn::LoadU(d, data + i + 2 * N));
        acc3 = hn::Add(acc3, hn::LoadU(d, data + i + 3 * N));
        acc4 = hn::Add(acc4, hn::LoadU(d, data + i + 4 * N));
        acc5 = hn::Add(acc5, hn::LoadU(d, data + i + 5 * N));
        acc6 = hn::Add(acc6, hn::LoadU(d, data + i + 6 * N));
        acc7 = hn::Add(acc7, hn::LoadU(d, data + i + 7 * N));
    }

    acc0 = hn::Add(hn::Add(acc0, acc1), hn::Add(acc2, acc3));
    acc4 = hn::Add(hn::Add(acc4, acc5), hn::Add(acc6, acc7));
    acc0 = hn::Add(acc0, acc4);

    for (; i + N <= n; i += N) {
        acc0 = hn::Add(acc0, hn::LoadU(d, data + i));
    }

    int64_t result = hn::ReduceSum(d, acc0);
    for (; i < n; ++i) result += data[i];
    return result;
}

// ============================================================================
// SIMD: filter_gt_i64 v1 — StoreMaskBits → SelectionVector
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

    alignas(8) uint8_t mask_bytes[8] = {};  // 최대 64개 lane / 8

    for (; i + N <= num_rows; i += N) {
        const auto vals = hn::LoadU(d, column_data + i);
        const auto mask = hn::Gt(vals, thresh_vec);

        hn::StoreMaskBits(d, mask, mask_bytes);
        uint64_t bits = 0;
        const size_t nbytes = (N + 7) / 8;
        for (size_t b = 0; b < nbytes; ++b) {
            bits |= (static_cast<uint64_t>(mask_bytes[b]) << (b * 8));
        }

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
// SIMD: filter_gt_i64 v2 — BitMask 직접 기록 (인덱스 변환 없음)
//
// 핵심 최적화:
//   1) StoreMaskBits 결과를 uint64_t 배열에 OR 기록 → 인덱스 변환 루프 제거
//   2) 출력 대역폭: N rows → N/8 bytes (8x 감소)
//   3) 비트마스크는 캐시 효율이 매우 높음: 1M행 → 128KB (L2 캐시 내 유지)
//   4) 다운스트림에서 popcount/ctz로 소비 → 분기 예측 없음
// ============================================================================
void filter_gt_i64_bitmask_impl(
    const int64_t* column_data,
    size_t num_rows,
    int64_t threshold,
    uint64_t* out_bits
) {
    const hn::ScalableTag<int64_t> d;
    const size_t N = hn::Lanes(d);

    const auto thresh_vec = hn::Set(d, threshold);

    // StoreMaskBits는 ceil(N/8) 바이트 기록
    // N=8(AVX-512): 1 byte, N=4(AVX2): 1 byte, N=2(SSE4): 1 byte
    alignas(8) uint8_t mask_bytes[8] = {};
    const size_t nbytes = (N + 7) / 8;

    size_t i = 0;
    // 한 uint64_t 블록 = 64 행
    // N이 64보다 작을 수 있으므로 bit_pos로 위치 추적
    size_t bit_pos = 0;  // 현재 기록 중인 비트 위치 (0..num_rows-1)

    for (; i + N <= num_rows; i += N, bit_pos += N) {
        const auto vals = hn::LoadU(d, column_data + i);
        const auto mask = hn::Gt(vals, thresh_vec);

        hn::StoreMaskBits(d, mask, mask_bytes);

        // mask_bytes의 비트를 out_bits의 올바른 위치에 OR 기록
        // bit_pos는 항상 N의 배수이므로 바이트 경계 정렬 보장 (N >= 8)
        uint64_t word_idx = bit_pos >> 6;   // bit_pos / 64
        uint32_t bit_off  = bit_pos & 63;   // bit_pos % 64

        // N개 비트를 word에 기록 (최대 2 word에 걸칠 수 있음)
        uint64_t bits = 0;
        for (size_t b = 0; b < nbytes; ++b) {
            bits |= (static_cast<uint64_t>(mask_bytes[b]) << (b * 8));
        }
        // N개만 유효 (나머지 상위 비트 마스킹)
        if (N < 64) {
            bits &= (1ULL << N) - 1ULL;
        }

        out_bits[word_idx] |= (bits << bit_off);
        // 비트가 64비트 경계를 넘는 경우 (bit_off + N > 64)
        if (bit_off + N > 64) {
            out_bits[word_idx + 1] |= (bits >> (64 - bit_off));
        }
    }

    // scalar tail
    for (; i < num_rows; ++i) {
        if (column_data[i] > threshold) {
            out_bits[i >> 6] |= (1ULL << (i & 63));
        }
    }
}

// ============================================================================
// SIMD: vwap v1 — f64 MulAdd 파이프라인 (2x unroll)
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

// ============================================================================
// SIMD: vwap v2 — 4x unroll + prefetch (양 배열 동시)
//
// 최적화 근거:
//   1) 4x unroll: OOO 프로세서의 ROB를 채워 메모리 → FMA 레이턴시 숨기기
//   2) 양 배열(prices, volumes) 동시 prefetch → 메모리 대역폭 최대 활용
//   3) FMA 파이프라인: Skylake+ FMA throughput 0.5 CPI (포트 0/1)
//      4 accumulators → 독립 FMA 체인 → 최대 처리량 달성
// ============================================================================
double vwap_fused_impl(const int64_t* prices, const int64_t* volumes, size_t n) {
    const hn::ScalableTag<int64_t> di;
    const hn::ScalableTag<double> df;
    const size_t N = hn::Lanes(di);

    auto pv0 = hn::Zero(df);
    auto pv1 = hn::Zero(df);
    auto pv2 = hn::Zero(df);
    auto pv3 = hn::Zero(df);
    auto v0  = hn::Zero(df);
    auto v1  = hn::Zero(df);
    auto v2  = hn::Zero(df);
    auto v3  = hn::Zero(df);

    size_t i = 0;
    const size_t N4 = N * 4;
    const size_t PREFETCH_STRIDE = 64; // 64 elements × 8 bytes = 512 bytes

    for (; i + N4 <= n; i += N4) {
        // 두 배열 모두 prefetch — 메모리 컨트롤러 활용도 극대화
        hwy::Prefetch(prices  + i + PREFETCH_STRIDE);
        hwy::Prefetch(volumes + i + PREFETCH_STRIDE);

        const auto p0_ = hn::ConvertTo(df, hn::LoadU(di, prices  + i + 0 * N));
        const auto p1_ = hn::ConvertTo(df, hn::LoadU(di, prices  + i + 1 * N));
        const auto p2_ = hn::ConvertTo(df, hn::LoadU(di, prices  + i + 2 * N));
        const auto p3_ = hn::ConvertTo(df, hn::LoadU(di, prices  + i + 3 * N));

        const auto v0_ = hn::ConvertTo(df, hn::LoadU(di, volumes + i + 0 * N));
        const auto v1_ = hn::ConvertTo(df, hn::LoadU(di, volumes + i + 1 * N));
        const auto v2_ = hn::ConvertTo(df, hn::LoadU(di, volumes + i + 2 * N));
        const auto v3_ = hn::ConvertTo(df, hn::LoadU(di, volumes + i + 3 * N));

        pv0 = hn::MulAdd(p0_, v0_, pv0);
        pv1 = hn::MulAdd(p1_, v1_, pv1);
        pv2 = hn::MulAdd(p2_, v2_, pv2);
        pv3 = hn::MulAdd(p3_, v3_, pv3);

        v0 = hn::Add(v0, v0_);
        v1 = hn::Add(v1, v1_);
        v2 = hn::Add(v2, v2_);
        v3 = hn::Add(v3, v3_);
    }

    // reduce
    pv0 = hn::Add(hn::Add(pv0, pv1), hn::Add(pv2, pv3));
    v0  = hn::Add(hn::Add(v0, v1),   hn::Add(v2, v3));

    for (; i + N <= n; i += N) {
        const auto p_ = hn::ConvertTo(df, hn::LoadU(di, prices  + i));
        const auto v_ = hn::ConvertTo(df, hn::LoadU(di, volumes + i));
        pv0 = hn::MulAdd(p_, v_, pv0);
        v0  = hn::Add(v0, v_);
    }

    double total_pv = hn::ReduceSum(df, pv0);
    double total_v  = hn::ReduceSum(df, v0);

    for (; i < n; ++i) {
        total_pv += static_cast<double>(prices[i]) * static_cast<double>(volumes[i]);
        total_v  += static_cast<double>(volumes[i]);
    }

    if (total_v == 0.0) return 0.0;
    return total_pv / total_v;
}

}  // namespace HWY_NAMESPACE
}  // namespace zeptodb::execution

HWY_AFTER_NAMESPACE();

// ============================================================================
// HWY_ONCE: Dispatch table + public API
// ============================================================================
#if HWY_ONCE

#include <numeric>  // popcount

namespace zeptodb::execution {

HWY_EXPORT(sum_i64_simd);
HWY_EXPORT(sum_i64_simd_v2_impl);
HWY_EXPORT(filter_gt_i64_simd);
HWY_EXPORT(filter_gt_i64_bitmask_impl);
HWY_EXPORT(vwap_simd);
HWY_EXPORT(vwap_fused_impl);

// ============================================================================
// SelectionVector
// ============================================================================
SelectionVector::SelectionVector(size_t max_size)
    : indices_(std::make_unique<uint32_t[]>(max_size))
{
}

// ============================================================================
// BitMask
// ============================================================================
BitMask::BitMask(size_t num_rows)
    : num_rows_(num_rows)
    , num_words_((num_rows + 63) / 64)
{
    bits_ = std::make_unique<uint64_t[]>(num_words_);
    std::fill(bits_.get(), bits_.get() + num_words_, 0ULL);
}

void BitMask::clear() {
    std::fill(bits_.get(), bits_.get() + num_words_, 0ULL);
}

size_t BitMask::popcount() const {
    size_t total = 0;
    for (size_t i = 0; i < num_words_; ++i) {
        total += static_cast<size_t>(__builtin_popcountll(bits_[i]));
    }
    return total;
}

// ============================================================================
// Public API v1 — 기존 인터페이스
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

// ============================================================================
// Public API v2 — Phase B 최적화
// ============================================================================

void filter_gt_i64_bitmask(
    const int64_t* column_data,
    size_t num_rows,
    int64_t threshold,
    BitMask& result
) {
    result.clear();
    HWY_DYNAMIC_DISPATCH(filter_gt_i64_bitmask_impl)(
        column_data, num_rows, threshold, result.data()
    );
}

// sum_i64_fast: 스칼라 4-way 언롤 + prefetch
// noinline으로 컴파일러 자동 인라인/재최적화 방지 (벤치마크 정확성)
__attribute__((noinline))
int64_t sum_i64_fast(const int64_t* data, size_t n) {
    int64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t i = 0;

    // 16 elements (128 bytes) 단위 처리
    // prefetch: 64 elements(512 bytes) 앞 — L2 레이턴시(~12ns) 숨기기
    for (; i + 16 <= n; i += 16) {
        __builtin_prefetch(data + i + 64, 0, 1);
        // 4 누산기에 분산 → 연산 레이턴시(ADD: 1CPI) 스케줄러 숨기기
        s0 += data[i+ 0] + data[i+ 4] + data[i+ 8] + data[i+12];
        s1 += data[i+ 1] + data[i+ 5] + data[i+ 9] + data[i+13];
        s2 += data[i+ 2] + data[i+ 6] + data[i+10] + data[i+14];
        s3 += data[i+ 3] + data[i+ 7] + data[i+11] + data[i+15];
    }

    // scalar tail
    for (; i < n; ++i) s0 += data[i];

    return s0 + s1 + s2 + s3;
}

int64_t sum_i64_simd_v2(const int64_t* column_data, size_t num_rows) {
    return HWY_DYNAMIC_DISPATCH(sum_i64_simd_v2_impl)(column_data, num_rows);
}

int64_t sum_i64_masked(const int64_t* data, const BitMask& mask) {
    int64_t total = 0;
    const uint64_t* bits = mask.data();
    const size_t num_words = mask.num_words();

    // 각 uint64_t 워드의 비트를 ctz로 순회
    // popcount가 적을수록(선택률 낮을수록) 빠름
    for (size_t w = 0; w < num_words; ++w) {
        uint64_t word = bits[w];
        while (word) {
            int bit = __builtin_ctzll(word);
            size_t row = (w << 6) + static_cast<size_t>(bit);
            total += data[row];
            word &= word - 1;  // 최하위 비트 제거
        }
    }
    return total;
}

double vwap_fused(const int64_t* prices, const int64_t* volumes, size_t n) {
    return HWY_DYNAMIC_DISPATCH(vwap_fused_impl)(prices, volumes, n);
}

}  // namespace zeptodb::execution

#endif  // HWY_ONCE
