#pragma once
// ============================================================================
// APEX-DB: JOIN Operator Framework
// ============================================================================
// 제네릭 조인 인터페이스 + ASOF JOIN 구현 + HashJoin 스텁
//
// ASOF JOIN 알고리즘:
//   - 두 포인터 병합 (O(n+m)), 양쪽이 timestamp 기준 정렬되어 있다고 가정
//   - 왼쪽 행마다 timestamp <= right_time 중 가장 큰 오른쪽 행 매칭
//   - 심볼 키로 먼저 필터링, 타임스탬프로 ASOF 매칭
// ============================================================================

#include "apex/storage/column_store.h"
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <cstring>

namespace apex::execution {

using apex::storage::ColumnVector;

// ============================================================================
// JoinResult: 조인 결과 인덱스 쌍
// ============================================================================
struct JoinResult {
    std::vector<int64_t> left_indices;    // 왼쪽 테이블 매칭 행 인덱스
    std::vector<int64_t> right_indices;   // 오른쪽 테이블 매칭 행 인덱스
    size_t               match_count = 0;
};

// ============================================================================
// JoinOperator: 제네릭 조인 인터페이스
// ============================================================================
class JoinOperator {
public:
    virtual ~JoinOperator() = default;

    /// 조인 실행
    /// @param left_key    왼쪽 테이블 키 컬럼 (심볼 등)
    /// @param right_key   오른쪽 테이블 키 컬럼
    /// @param left_time   ASOF용: 왼쪽 타임스탬프 (optional)
    /// @param right_time  ASOF용: 오른쪽 타임스탬프 (optional)
    virtual JoinResult execute(
        const ColumnVector& left_key,
        const ColumnVector& right_key,
        const ColumnVector* left_time  = nullptr,
        const ColumnVector* right_time = nullptr
    ) = 0;
};

// ============================================================================
// AsofJoinOperator: ASOF JOIN — 두 포인터 병합, O(n+m)
// ============================================================================
// 가정:
//   - 두 테이블 모두 (key, timestamp) 기준 정렬
//   - 왼쪽 행의 timestamp에 대해 right_time <= left_time 인 가장 늦은 오른쪽 행 매칭
//   - 키가 같아야 매칭됨 (symbol 필터)
// ============================================================================
class AsofJoinOperator : public JoinOperator {
public:
    JoinResult execute(
        const ColumnVector& left_key,
        const ColumnVector& right_key,
        const ColumnVector* left_time  = nullptr,
        const ColumnVector* right_time = nullptr
    ) override;

private:
    // 단일 심볼에 대해 ASOF 매칭 수행
    void asof_match_symbol(
        const int64_t* l_times, const int64_t* l_keys,
        const int64_t* r_times, const int64_t* r_keys,
        size_t l_count, size_t r_count,
        int64_t symbol,
        JoinResult& result
    );
};

// ============================================================================
// HashJoinOperator: Hash JOIN — Build/Probe 해시 조인
// ============================================================================
// 알고리즘:
//   1. Build phase: 오른쪽 테이블 스캔 → hash_map[key] = [row_indices...]
//   2. Probe phase: 왼쪽 테이블 스캔 → 해시맵 조회 → 매칭된 모든 쌍 반환
//
// 복잡도: O(n+m) 평균 (해시 충돌 없을 때)
// N:M 조인, 1:1, 1:N 모두 지원
// left_time / right_time 파라미터는 향후 확장용 (현재 미사용)
// ============================================================================
class HashJoinOperator : public JoinOperator {
public:
    JoinResult execute(
        const ColumnVector& left_key,
        const ColumnVector& right_key,
        const ColumnVector* left_time  = nullptr,
        const ColumnVector* right_time = nullptr
    ) override;
};

} // namespace apex::execution
