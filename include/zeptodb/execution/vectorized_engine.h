#pragma once
// ============================================================================
// Layer 3: Vectorized Execution Engine
// ============================================================================
// 문서 근거: layer3_execution_engine.md
//   - DataBlock Pipeline (8192 rows)
//   - SIMD (Highway) 기반 필터/집계
//   - L1/L2 캐시 핫 유지
//
// Phase B v2 최적화 추가:
//   - BitMask: SelectionVector 대체 — 인덱스 대신 비트마스크로 필터 결과 저장
//   - filter_gt_i64_bitmask: StoreMaskBits → uint64_t 배열 직접 기록 (분기 없음)
//   - sum_i64_fast: 수동 4-way 언롤 + prefetch (컴파일러 독립적 최적화)
//   - sum_i64_simd_v2: 8-way SIMD 언롤 + prefetch
//   - sum_i64_masked: 비트마스크 기반 선택적 합계 (gather 없이 ctz 순회)
//   - vwap_fused: 4x SIMD 언롤 + prefetch 양 배열 (단일 패스)
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
// SelectionVector: 필터 결과 인덱스 배열 (v1 — 하위 호환 유지)
// ============================================================================
class SelectionVector {
public:
    explicit SelectionVector(size_t max_size);

    void add(uint32_t idx) { indices_[size_++] = idx; }
    void reset() { size_ = 0; }

    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] const uint32_t* data() const { return indices_.get(); }
    [[nodiscard]] uint32_t operator[](size_t i) const { return indices_[i]; }

    // SIMD 필터가 indices_에 직접 기록한 후 크기를 설정
    void set_size(size_t n) { size_ = n; }

private:
    std::unique_ptr<uint32_t[]> indices_;
    size_t size_ = 0;
};

// ============================================================================
// BitMask: 필터 결과를 비트 배열로 저장 (v2 — Phase B 최적화)
//
// 설계 근거:
//   SelectionVector는 필터 통과 시 uint32_t 인덱스를 기록 → 메모리 쓰기 + 분기 발생.
//   BitMask는 SIMD StoreMaskBits 결과를 uint64_t 배열에 직접 OR 기록하므로:
//     1) 쓰기 대역폭: N행당 N bytes(인덱스) → N/8 bytes(비트) 로 8x 감소
//     2) 분기 없음: ctz(bits)로 집계 시 분기 예측 불필요
//     3) 캐시 효율: 1M행 기준 4MB(인덱스) → 128KB(비트마스크)
// ============================================================================
class BitMask {
public:
    /// num_rows 행에 대한 비트마스크 생성 (전부 0으로 초기화)
    explicit BitMask(size_t num_rows);

    /// 비트마스크 전체를 0으로 초기화
    void clear();

    /// i번째 비트 설정
    void set(size_t i) {
        bits_[i >> 6] |= (1ULL << (i & 63));
    }

    /// i번째 비트 검사
    [[nodiscard]] bool test(size_t i) const {
        return (bits_[i >> 6] >> (i & 63)) & 1ULL;
    }

    /// 설정된 비트 수 (통과한 행 수)
    [[nodiscard]] size_t popcount() const;

    /// 내부 uint64_t 배열 접근 (SIMD 직접 기록용)
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
// Operators v1: 기존 인터페이스 — 하위 호환 유지
// ============================================================================

/// 컬럼에 대한 스칼라 필터 (예: price > 10000)
/// SelectionVector에 통과한 행 인덱스를 기록
void filter_gt_i64(
    const int64_t* column_data,
    size_t num_rows,
    int64_t threshold,
    SelectionVector& result
);

/// 컬럼 합계 (int64)
int64_t sum_i64(
    const int64_t* column_data,
    size_t num_rows
);

/// Selection 적용된 합계
int64_t sum_i64_selected(
    const int64_t* column_data,
    const SelectionVector& selection
);

/// VWAP 계산: sum(price * volume) / sum(volume)
double vwap(
    const int64_t* prices,
    const int64_t* volumes,
    size_t num_rows
);

// ============================================================================
// Operators v2: Phase B 최적화 변형
// ============================================================================

/// 비트마스크 기반 필터 (v2)
/// — SelectionVector 대신 BitMask에 직접 기록
/// — 분기 없음, 캐시 효율 8x 향상
void filter_gt_i64_bitmask(
    const int64_t* column_data,
    size_t num_rows,
    int64_t threshold,
    BitMask& result
);

/// 수동 4-way 언롤 합계 (스칼라, 컴파일러 독립 최적화)
/// — 4개 누산기로 연산 레이턴시 숨기기
/// — prefetch로 메모리 레이턴시 숨기기
/// — 자동 벡터화 없이도 scalar ILP 극대화
[[nodiscard]] int64_t sum_i64_fast(
    const int64_t* column_data,
    size_t num_rows
);

/// 8-way SIMD 언롤 + prefetch 합계 (v2)
[[nodiscard]] int64_t sum_i64_simd_v2(
    const int64_t* column_data,
    size_t num_rows
);

/// 비트마스크 기반 선택적 합계
/// — gather 없이 ctz(bits)로 통과 행만 합산
/// — 선택률 낮을수록 효율적 (sparse 최적화)
[[nodiscard]] int64_t sum_i64_masked(
    const int64_t* column_data,
    const BitMask& mask
);

/// VWAP 융합 파이프라인 (v2, __int128 정수 누산기)
/// — price/volume 두 배열을 단일 패스로 처리 (기존과 동일)
/// — 4x SIMD 언롤 + 양 배열 prefetch
/// — __int128로 정수 오버플로우 방지 (float 변환 최소화)
[[nodiscard]] double vwap_fused(
    const int64_t* prices,
    const int64_t* volumes,
    size_t num_rows
);

// ============================================================================
// VectorizedEngine: 쿼리 실행 엔트리포인트 (Layer 3 facade)
// ============================================================================
class VectorizedEngine {
public:
    VectorizedEngine() = default;

    // TODO: Query Planner 통합 후 확장
    // 현재는 primitive 벡터 연산만 노출
};

} // namespace zeptodb::execution
