// ============================================================================
// ZeptoDB: JOIN Operator Implementation
// ============================================================================
// ASOF JOIN: 이진 탐색 + 두 포인터 병합 알고리즘
// Hash JOIN: INNER / LEFT (NULL 센티넬 INT64_MIN 사용)
// Window JOIN: 이진 탐색 기반 시간 윈도우 집계 O(n log m)
// ============================================================================

#include "zeptodb/execution/join_operator.h"
#include "zeptodb/execution/flat_hash_map.h"
#include "zeptodb/storage/column_store.h"

#include <algorithm>
#include <vector>
#include <cstdint>
#include <climits>
#include <cmath>

namespace zeptodb::execution {

// ============================================================================
// AsofJoinOperator::execute
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
    FlatHashMap<std::vector<int64_t>> right_groups(rn);
    for (size_t i = 0; i < rn; ++i) {
        right_groups[rk[i]].push_back(static_cast<int64_t>(i));
    }

    // 각 그룹 내에서 타임스탬프 기준 정렬
    right_groups.for_each([rt](int64_t /*sym*/, std::vector<int64_t>& indices) {
        std::sort(indices.begin(), indices.end(), [rt](int64_t a, int64_t b) {
            return rt[a] < rt[b];
        });
    });

    // 각 왼쪽 행에 대해 ASOF 매칭 (이진 탐색)
    for (size_t li = 0; li < ln; ++li) {
        int64_t sym  = lk[li];
        int64_t l_ts = lt[li];

        auto* rindices = right_groups.find(sym);
        if (!rindices) continue;

        auto pos = std::upper_bound(
            rindices->begin(), rindices->end(), l_ts,
            [rt](int64_t ts_val, int64_t ridx) {
                return ts_val < rt[ridx];
            }
        );

        if (pos != rindices->begin()) {
            --pos;
            result.left_indices.push_back(static_cast<int64_t>(li));
            result.right_indices.push_back(*pos);
            ++result.match_count;
        }
    }

    return result;
}

// ============================================================================
// AsofJoinOperator::asof_match_symbol (두 포인터, O(n+m))
// ============================================================================
void AsofJoinOperator::asof_match_symbol(
    const int64_t* l_times, const int64_t* /*l_keys*/,
    const int64_t* r_times, const int64_t* /*r_keys*/,
    size_t l_count, size_t r_count,
    int64_t /*symbol*/,
    JoinResult& result)
{
    size_t ri = 0;
    size_t last_valid_ri = SIZE_MAX;

    for (size_t li = 0; li < l_count; ++li) {
        int64_t l_ts = l_times[li];

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
// HashJoinOperator::execute — INNER / LEFT / RIGHT JOIN
// ============================================================================
// INNER: 매칭된 쌍만 반환
// LEFT:  매칭 없는 왼쪽 행도 포함 (right_indices[i] = -1)
// RIGHT: 매칭 없는 오른쪽 행도 포함 (left_indices[i] = -1)
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
    result.left_indices.reserve(std::min(ln, rn));
    result.right_indices.reserve(std::min(ln, rn));

    if (join_type_ == JoinType::RIGHT) {
        // RIGHT: build on left, probe with right
        FlatHashMap<std::vector<int64_t>> hash_map(ln);
        for (size_t li = 0; li < ln; ++li)
            hash_map[lk[li]].push_back(static_cast<int64_t>(li));

        for (size_t ri = 0; ri < rn; ++ri) {
            auto* bucket = hash_map.find(rk[ri]);
            if (!bucket) {
                result.left_indices.push_back(-1LL);  // left NULL
                result.right_indices.push_back(static_cast<int64_t>(ri));
                ++result.match_count;
                continue;
            }
            for (int64_t li : *bucket) {
                result.left_indices.push_back(li);
                result.right_indices.push_back(static_cast<int64_t>(ri));
                ++result.match_count;
            }
        }
    } else if (join_type_ == JoinType::FULL) {
        // FULL OUTER: LEFT JOIN + unmatched right rows
        FlatHashMap<std::vector<int64_t>> hash_map(rn);
        for (size_t ri = 0; ri < rn; ++ri)
            hash_map[rk[ri]].push_back(static_cast<int64_t>(ri));

        std::vector<bool> right_matched(rn, false);
        for (size_t li = 0; li < ln; ++li) {
            auto* bucket = hash_map.find(lk[li]);
            if (!bucket) {
                result.left_indices.push_back(static_cast<int64_t>(li));
                result.right_indices.push_back(-1LL);
                ++result.match_count;
                continue;
            }
            for (int64_t ri : *bucket) {
                result.left_indices.push_back(static_cast<int64_t>(li));
                result.right_indices.push_back(ri);
                right_matched[static_cast<size_t>(ri)] = true;
                ++result.match_count;
            }
        }
        for (size_t ri = 0; ri < rn; ++ri) {
            if (!right_matched[ri]) {
                result.left_indices.push_back(-1LL);
                result.right_indices.push_back(static_cast<int64_t>(ri));
                ++result.match_count;
            }
        }
    } else {
        // INNER / LEFT: build on right, probe with left
        FlatHashMap<std::vector<int64_t>> hash_map(rn);
        for (size_t ri = 0; ri < rn; ++ri)
            hash_map[rk[ri]].push_back(static_cast<int64_t>(ri));

        for (size_t li = 0; li < ln; ++li) {
            auto* bucket = hash_map.find(lk[li]);
            if (!bucket) {
                if (join_type_ == JoinType::LEFT) {
                    result.left_indices.push_back(static_cast<int64_t>(li));
                    result.right_indices.push_back(-1LL);
                    ++result.match_count;
                }
                continue;
            }
            for (int64_t ri : *bucket) {
                result.left_indices.push_back(static_cast<int64_t>(li));
                result.right_indices.push_back(ri);
                ++result.match_count;
            }
        }
    }

    return result;
}

// ============================================================================
// WindowJoinOperator::execute
// ============================================================================
// 알고리즘:
//   1. 오른쪽 테이블 심볼별 그룹화 (해시맵)
//   2. 각 왼쪽 행:
//      a. 심볼로 오른쪽 그룹 찾기
//      b. 이진 탐색으로 [t_left - before, t_left + after] 경계 찾기
//      c. 범위 내 오른쪽 행에 집계 함수 적용
//   3. 결과 반환
// ============================================================================
WindowJoinResult WindowJoinOperator::execute(
    const int64_t* left_key,  size_t ln,
    const int64_t* right_key, size_t rn,
    const int64_t* left_time,
    const int64_t* right_time,
    const int64_t* right_val)
{
    WindowJoinResult result;
    result.left_indices.resize(ln);
    result.agg_values.resize(ln, 0);
    result.match_counts.resize(ln, 0);

    // 오른쪽 테이블 심볼별 인덱스 그룹화 (타임스탬프 오름차순 정렬됨 가정)
    FlatHashMap<std::vector<size_t>> right_groups(rn);
    for (size_t i = 0; i < rn; ++i) {
        right_groups[right_key[i]].push_back(i);
    }

    // 각 그룹 타임스탬프 기준 정렬 (안전 보장)
    right_groups.for_each([right_time](int64_t /*sym*/, std::vector<size_t>& indices) {
        std::sort(indices.begin(), indices.end(),
            [right_time](size_t a, size_t b) {
                return right_time[a] < right_time[b];
            });
    });

    // 각 왼쪽 행 처리
    for (size_t li = 0; li < ln; ++li) {
        result.left_indices[li] = static_cast<int64_t>(li);

        auto* rgroup = right_groups.find(left_key[li]);
        if (!rgroup) {
            // 매칭 없음 → 집계 = 0, count = 0
            continue;
        }

        const auto& rg = *rgroup;
        int64_t t_lo = left_time[li] - window_before_;
        int64_t t_hi = left_time[li] + window_after_;

        // 이진 탐색으로 [t_lo, t_hi] 범위 찾기
        // lower_bound: right_time[idx] >= t_lo
        size_t begin = static_cast<size_t>(std::lower_bound(
            rg.begin(), rg.end(), t_lo,
            [right_time](size_t ridx, int64_t val) {
                return right_time[ridx] < val;
            }) - rg.begin());

        // upper_bound: right_time[idx] > t_hi
        size_t end = static_cast<size_t>(std::upper_bound(
            rg.begin(), rg.end(), t_hi,
            [right_time](int64_t val, size_t ridx) {
                return val < right_time[ridx];
            }) - rg.begin());

        if (begin >= end) continue; // 범위 내 행 없음

        int64_t cnt = 0;
        result.agg_values[li] = aggregate_window(
            right_time, right_val, rg, begin, end, cnt);
        result.match_counts[li] = cnt;
    }

    return result;
}

// ============================================================================
// WindowJoinOperator::aggregate_window
// ============================================================================
int64_t WindowJoinOperator::aggregate_window(
    const int64_t* /*right_time*/,
    const int64_t* right_val,
    const std::vector<size_t>& right_group_indices,
    size_t begin, size_t end,
    int64_t& out_count)
{
    out_count = static_cast<int64_t>(end - begin);

    switch (agg_type_) {
        case WJAggType::COUNT:
            return out_count;

        case WJAggType::SUM: {
            int64_t sum = 0;
            for (size_t i = begin; i < end; ++i) {
                sum += right_val[right_group_indices[i]];
            }
            return sum;
        }

        case WJAggType::AVG: {
            if (out_count == 0) return 0;
            double sum = 0.0;
            for (size_t i = begin; i < end; ++i) {
                sum += static_cast<double>(right_val[right_group_indices[i]]);
            }
            return static_cast<int64_t>(sum / out_count);
        }

        case WJAggType::MIN: {
            int64_t minv = INT64_MAX;
            for (size_t i = begin; i < end; ++i) {
                minv = std::min(minv, right_val[right_group_indices[i]]);
            }
            return minv == INT64_MAX ? 0 : minv;
        }

        case WJAggType::MAX: {
            int64_t maxv = INT64_MIN;
            for (size_t i = begin; i < end; ++i) {
                maxv = std::max(maxv, right_val[right_group_indices[i]]);
            }
            return maxv == INT64_MIN ? 0 : maxv;
        }
    }
    return 0;
}

} // namespace zeptodb::execution
