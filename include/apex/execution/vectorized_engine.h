#pragma once
// ============================================================================
// Layer 3: Vectorized Execution Engine
// ============================================================================
// 문서 근거: layer3_execution_engine.md
//   - DataBlock Pipeline (8192 rows)
//   - SIMD (Highway) 기반 필터/집계
//   - L1/L2 캐시 핫 유지
// ============================================================================

#include "apex/common/types.h"
#include "apex/storage/column_store.h"

#include <functional>
#include <memory>
#include <vector>
#include <span>

namespace apex::execution {

// ============================================================================
// SelectionVector: 필터 결과 인덱스 배열
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
// Operators: 벡터화 파이프라인 연산자
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
// VectorizedEngine: 쿼리 실행 엔트리포인트 (Layer 3 facade)
// ============================================================================
class VectorizedEngine {
public:
    VectorizedEngine() = default;

    // TODO: Query Planner 통합 후 확장
    // 현재는 primitive 벡터 연산만 노출
};

} // namespace apex::execution
