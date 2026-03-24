#pragma once
// ============================================================================
// ZeptoDB: JOIN Operator Framework
// ============================================================================
// 제네릭 조인 인터페이스 + ASOF JOIN 구현 + HashJoin + WindowJoin
//
// ASOF JOIN 알고리즘:
//   - 두 포인터 병합 (O(n+m)), 양쪽이 timestamp 기준 정렬되어 있다고 가정
//   - 왼쪽 행의 timestamp에 대해 right_time <= left_time 인 가장 늦은 오른쪽 행 매칭
//
// HashJoin (INNER / LEFT):
//   - Build/Probe 해시 조인
//   - LEFT JOIN: 매칭 없는 왼쪽 행은 right_indices = -1 → NULL 센티넬(INT64_MIN)
//
// WindowJoin (kdb+ wj):
//   - 각 왼쪽 행에 대해 시간 윈도우 [t-before, t+after] 안의 오른쪽 행 집계
//   - 이진 탐색으로 O(n log m)
// ============================================================================

#include "zeptodb/storage/column_store.h"
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <cstring>
#include <climits>

namespace zeptodb::execution {

using zeptodb::storage::ColumnVector;

// ============================================================================
// JoinType: 조인 타입 (INNER, LEFT, RIGHT, FULL)
// ============================================================================
enum class JoinType {
    INNER,
    LEFT,
    RIGHT,
    FULL,
};

// ============================================================================
// WJAggType: Window JOIN 집계 타입
// ============================================================================
enum class WJAggType {
    AVG,
    SUM,
    COUNT,
    MIN,
    MAX,
};

// NULL 센티넬 값 (INT64_MIN = 오른쪽 테이블 매칭 없음)
constexpr int64_t JOIN_NULL = INT64_MIN;

// ============================================================================
// JoinResult: 조인 결과 인덱스 쌍
// right_indices[i] = -1 → LEFT JOIN에서 매칭 없음 (오른쪽 컬럼 = JOIN_NULL)
// ============================================================================
struct JoinResult {
    std::vector<int64_t> left_indices;    // 왼쪽 테이블 매칭 행 인덱스
    std::vector<int64_t> right_indices;   // 오른쪽 테이블 매칭 행 인덱스 (-1 = NULL)
    size_t               match_count = 0;
};

// ============================================================================
// WindowJoinResult: WINDOW JOIN 결과
// 각 왼쪽 행에 대해 집계 값 하나씩
// ============================================================================
struct WindowJoinResult {
    std::vector<int64_t> left_indices;    // 왼쪽 행 인덱스 (0..n-1)
    std::vector<int64_t> agg_values;      // 집계 결과 (없으면 0)
    std::vector<int64_t> match_counts;    // 매칭된 오른쪽 행 수
};

// ============================================================================
// JoinOperator: 제네릭 조인 인터페이스
// ============================================================================
class JoinOperator {
public:
    virtual ~JoinOperator() = default;

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
class AsofJoinOperator : public JoinOperator {
public:
    JoinResult execute(
        const ColumnVector& left_key,
        const ColumnVector& right_key,
        const ColumnVector* left_time  = nullptr,
        const ColumnVector* right_time = nullptr
    ) override;

private:
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
// JoinType::LEFT 지원:
//   - 매칭 없는 왼쪽 행: right_indices[i] = -1 기록
//   - 결과 조합 시 오른쪽 컬럼 = JOIN_NULL (INT64_MIN)
// ============================================================================
class HashJoinOperator : public JoinOperator {
public:
    explicit HashJoinOperator(JoinType type = JoinType::INNER)
        : join_type_(type) {}

    void set_join_type(JoinType t) { join_type_ = t; }
    JoinType join_type() const { return join_type_; }

    JoinResult execute(
        const ColumnVector& left_key,
        const ColumnVector& right_key,
        const ColumnVector* left_time  = nullptr,
        const ColumnVector* right_time = nullptr
    ) override;

private:
    JoinType join_type_ = JoinType::INNER;
};

// ============================================================================
// WindowJoinOperator: kdb+ wj 스타일 시간 윈도우 조인
// ============================================================================
// 알고리즘:
//   오른쪽 테이블이 (symbol, timestamp) 기준으로 정렬되어 있다고 가정
//   각 왼쪽 행에 대해:
//     1. symbol로 오른쪽 그룹 필터링 (해시맵, O(1))
//     2. 이진 탐색으로 [t - before, t + after] 범위 찾기
//     3. 범위 내 오른쪽 행에 agg 집계 적용
//   복잡도: O(n * log m) — 이진 탐색
//   양쪽 모두 정렬 보장 시 슬라이딩 윈도우로 O(n + m) 가능
// ============================================================================
class WindowJoinOperator {
public:
    explicit WindowJoinOperator(
        WJAggType agg_type    = WJAggType::AVG,
        int64_t window_before = 0,
        int64_t window_after  = 0)
        : agg_type_(agg_type)
        , window_before_(window_before)
        , window_after_(window_after) {}

    /// WINDOW JOIN 실행
    /// @param left_key/right_key  심볼 키
    /// @param left_time/right_time 타임스탬프
    /// @param right_val           집계 대상 값 컬럼
    WindowJoinResult execute(
        const int64_t* left_key,  size_t ln,
        const int64_t* right_key, size_t rn,
        const int64_t* left_time,
        const int64_t* right_time,
        const int64_t* right_val
    );

private:
    WJAggType agg_type_;
    int64_t   window_before_;
    int64_t   window_after_;

    int64_t aggregate_window(
        const int64_t* right_time,
        const int64_t* right_val,
        const std::vector<size_t>& right_group_indices,
        size_t begin, size_t end,
        int64_t& out_count
    );
};

} // namespace zeptodb::execution
