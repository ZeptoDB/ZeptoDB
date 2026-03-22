// ============================================================================
// APEX-DB: JOIN Operator Implementation
// ============================================================================
// ASOF JOIN: 이진 탐색 + 두 포인터 병합 알고리즘
// ============================================================================

#include "apex/execution/join_operator.h"
#include "apex/storage/column_store.h"

#include <algorithm>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace apex::execution {

// ============================================================================
// AsofJoinOperator::execute
//
// 알고리즘:
//   1) 오른쪽 테이블의 심볼별 인덱스 그룹화
//   2) 각 왼쪽 행에 대해:
//      a) 오른쪽 그룹에서 rt <= lt 인 최대값 이진 탐색으로 O(log m)
//   전체 복잡도: O(n log m) — 정렬이 보장되는 경우
//
// 정렬 가정: 같은 심볼의 오른쪽 행은 타임스탬프 오름차순
// ============================================================================
JoinResult AsofJoinOperator::execute(
    const ColumnVector& left_key,
    const ColumnVector& right_key,
    const ColumnVector* left_time,
    const ColumnVector* right_time)
{
    if (!left_time || !right_time) {
        throw std::runtime_error("AsofJoinOperator: left_time and right_time are required");
    }

    const size_t ln = left_key.size();
    const size_t rn = right_key.size();

    const int64_t* lk = static_cast<const int64_t*>(left_key.raw_data());
    const int64_t* rk = static_cast<const int64_t*>(right_key.raw_data());
    const int64_t* lt = static_cast<const int64_t*>(left_time->raw_data());
    const int64_t* rt = static_cast<const int64_t*>(right_time->raw_data());

    JoinResult result;
    result.left_indices.reserve(ln);
    result.right_indices.reserve(ln);

    // 심볼별 오른쪽 인덱스 그룹화
    // 각 그룹은 타임스탬프 오름차순으로 정렬되어 있다고 가정
    std::unordered_map<int64_t, std::vector<int64_t>> right_groups;
    right_groups.reserve(32);
    for (size_t i = 0; i < rn; ++i) {
        right_groups[rk[i]].push_back(static_cast<int64_t>(i));
    }

    // 각 그룹 내에서 타임스탬프 기준 정렬 (안전장치)
    for (auto& [sym, indices] : right_groups) {
        std::sort(indices.begin(), indices.end(), [rt](int64_t a, int64_t b) {
            return rt[a] < rt[b];
        });
    }

    // 각 왼쪽 행에 대해 ASOF 매칭 (이진 탐색)
    for (size_t li = 0; li < ln; ++li) {
        int64_t sym    = lk[li];
        int64_t l_ts   = lt[li];

        auto it = right_groups.find(sym);
        if (it == right_groups.end()) continue;

        const auto& rindices = it->second;
        // upper_bound: rt[ri] <= l_ts 인 마지막 원소 찾기
        // rindices는 rt 기준 오름차순 정렬
        auto pos = std::upper_bound(
            rindices.begin(), rindices.end(), l_ts,
            [rt](int64_t ts_val, int64_t ridx) {
                return ts_val < rt[ridx];
            }
        );

        if (pos != rindices.begin()) {
            --pos; // 마지막으로 rt[ri] <= l_ts 인 원소
            result.left_indices.push_back(static_cast<int64_t>(li));
            result.right_indices.push_back(*pos);
            ++result.match_count;
        }
    }

    return result;
}

// ============================================================================
// asof_match_symbol: 단일 심볼에 대한 ASOF 매칭 (두 포인터, O(n+m))
// 정렬된 두 시퀀스에 대해 최적화된 두 포인터 알고리즘
// ============================================================================
void AsofJoinOperator::asof_match_symbol(
    const int64_t* l_times, const int64_t* /*l_keys*/,
    const int64_t* r_times, const int64_t* /*r_keys*/,
    size_t l_count, size_t r_count,
    int64_t /*symbol*/,
    JoinResult& result)
{
    // 두 포인터 알고리즘 (양쪽 모두 정렬됨 가정)
    size_t ri = 0;
    size_t last_valid_ri = SIZE_MAX;

    for (size_t li = 0; li < l_count; ++li) {
        int64_t l_ts = l_times[li];

        // 오른쪽 포인터를 l_ts를 초과하지 않는 지점까지 전진
        while (ri < r_count && r_times[ri] <= l_ts) {
            last_valid_ri = ri;
            ++ri;
        }

        if (last_valid_ri != SIZE_MAX) {
            result.left_indices.push_back(static_cast<int64_t>(li));
            result.right_indices.push_back(static_cast<int64_t>(last_valid_ri));
            ++result.match_count;
        }
    }
}

// ============================================================================
// HashJoinOperator::execute
//
// 알고리즘:
//   Build phase: 오른쪽 테이블을 스캔하여 hash_map[key] = {row0, row1, ...} 구축
//   Probe phase: 왼쪽 테이블을 스캔하여 해시맵에서 매칭 조회
//
// 복잡도: O(n + m) 평균
// N:M 조인: 한 키에 여러 오른쪽 행이 있으면 카테시안 곱 생성
// ============================================================================
JoinResult HashJoinOperator::execute(
    const ColumnVector& left_key,
    const ColumnVector& right_key,
    const ColumnVector* /*left_time*/,
    const ColumnVector* /*right_time*/)
{
    const size_t ln = left_key.size();
    const size_t rn = right_key.size();

    const int64_t* lk = static_cast<const int64_t*>(left_key.raw_data());
    const int64_t* rk = static_cast<const int64_t*>(right_key.raw_data());

    JoinResult result;
    // 최소한 min(ln, rn) 만큼은 예약
    result.left_indices.reserve(std::min(ln, rn));
    result.right_indices.reserve(std::min(ln, rn));

    // ── Build phase: key → [right_row_index, ...] ──
    std::unordered_map<int64_t, std::vector<int64_t>> hash_map;
    hash_map.reserve(rn * 2); // 충돌 최소화를 위해 2배 예약

    for (size_t ri = 0; ri < rn; ++ri) {
        hash_map[rk[ri]].push_back(static_cast<int64_t>(ri));
    }

    // ── Probe phase: 왼쪽 테이블 스캔 → 해시맵 조회 ──
    for (size_t li = 0; li < ln; ++li) {
        auto it = hash_map.find(lk[li]);
        if (it == hash_map.end()) continue;

        // 매칭된 오른쪽 행 전부 (1:N, N:M 지원)
        for (int64_t ri : it->second) {
            result.left_indices.push_back(static_cast<int64_t>(li));
            result.right_indices.push_back(ri);
            ++result.match_count;
        }
    }

    return result;
}

} // namespace apex::execution
