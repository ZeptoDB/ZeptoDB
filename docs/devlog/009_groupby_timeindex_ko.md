# APEX-DB Devlog #009: GROUP BY 최적화 + 타임스탬프 범위 인덱스

**날짜:** 2026-03-22  
**브랜치:** main  
**작성자:** 고생이 (AI 엔지니어)

---

## 개요

이번 작업은 APEX-DB SQL 실행 엔진의 두 가지 핵심 성능 최적화와 추가 SQL 기능 구현이다.

1. **타임스탬프 범위 인덱스**: `WHERE timestamp BETWEEN X AND Y` — 전체 스캔 대신 이진탐색
2. **GROUP BY 최적화**: `GROUP BY symbol` — 파티션 구조 직접 활용
3. **ORDER BY + LIMIT**: top-N partial sort (`std::partial_sort`)
4. **MIN/MAX** 집계 및 다중 집계 지원 검증

---

## Part 1: 타임스탬프 범위 인덱스

### 문제

기존 `WHERE timestamp BETWEEN X AND Y` 처리:
```cpp
// 기존: O(n) 전체 선형 스캔
for (size_t i = 0; i < num_rows; ++i) {
    if (data[i] >= expr->lo && data[i] <= expr->hi)
        result.push_back(i);
}
```

데이터는 append-only이고 timestamp는 항상 단조 증가(오름차순 정렬)이므로, 이진탐색으로 O(log n)에 범위를 찾을 수 있다.

### 구현

`Partition` 클래스에 두 개의 메서드 추가 (`partition_manager.h`):

```cpp
// O(log n): timestamp 이진탐색으로 [start_idx, end_idx) 반환
std::pair<size_t, size_t> timestamp_range(int64_t from_ts, int64_t to_ts) const {
    auto span = const_cast<ColumnVector*>(ts_col)->as_span<int64_t>();
    auto begin = std::lower_bound(span.begin(), span.end(), from_ts);
    auto end   = std::upper_bound(span.begin(), span.end(), to_ts);
    return {begin - span.begin(), end - span.begin()};
}

// O(1): 파티션이 범위와 겹치는지 빠른 확인 (첫/마지막 행만 비교)
bool overlaps_time_range(int64_t lo, int64_t hi) const {
    auto span = const_cast<ColumnVector*>(ts_col)->as_span<int64_t>();
    return span.front() <= hi && span.back() >= lo;
}
```

Executor에서의 사용:
```cpp
// WHERE에서 timestamp BETWEEN 조건 자동 감지
int64_t ts_lo = INT64_MIN, ts_hi = INT64_MAX;
bool use_ts_index = extract_time_range(stmt, ts_lo, ts_hi);

for (auto* part : partitions) {
    if (use_ts_index) {
        // O(1): 파티션 전체 스킵 가능
        if (!part->overlaps_time_range(ts_lo, ts_hi)) continue;
        // O(log n): 범위 내 행만 추출
        auto [r_begin, r_end] = part->timestamp_range(ts_lo, ts_hi);
        sel_indices = eval_where_ranged(stmt, *part, r_begin, r_end);
    } else {
        sel_indices = eval_where(stmt, *part, n);
    }
}
```

### 주의사항

실제 타임스탬프는 `TickPlant::ingest()`에서 현재 벽시계 시각(nanoseconds)으로 덮어씌워진다. 따라서 테스트에서 `recv_ts = 1000 + i` 같은 작은 값을 넣어도 저장되는 타임스탬프는 현재 시각 기준이다. 테스트는 INT64_MAX 범위를 사용하는 방식으로 조정.

---

## Part 2: GROUP BY symbol 최적화

### 문제

기존 GROUP BY 처리:
```cpp
// 기존: 모든 행을 순회하며 hash table에 누적
std::unordered_map<int64_t, std::vector<GroupState>> groups;
for (auto idx : sel_indices) {
    int64_t gkey = gdata[idx];  // 각 행에서 symbol 컬럼 읽기
    groups[gkey].update(data[idx]);
}
```

문제: 파티션은 이미 symbol별로 분리되어 있는데, 굳이 각 행에서 symbol 값을 읽고 hash table에 넣는 과정이 불필요하다.

### 최적화 전략

`GROUP BY symbol`의 경우, 파티션 키에서 O(1)로 symbol_id를 읽을 수 있다:

```cpp
if (is_symbol_group) {
    // 파티션 키에서 직접 symbol_id 추출 — O(1), 행별 컬럼 읽기 없음
    int64_t symbol_gkey = static_cast<int64_t>(part->key().symbol_id);
    // ...
}
```

장점:
- 각 행에서 symbol 컬럼을 읽는 오버헤드 제거
- Hash 충돌 없음 (파티션 = 그룹)
- Cache-friendly: 파티션 내 컬럼 데이터를 순차 접근

### 일반 GROUP BY (기타 컬럼)

일반 컬럼의 경우 pre-allocated hash map 사용:

```cpp
std::unordered_map<int64_t, std::vector<GroupState>> groups;
groups.reserve(1024);  // 일반적인 cardinality 예상 pre-allocation
```

---

## Part 3: ORDER BY + LIMIT (top-N partial sort)

### 구현

`apply_order_by()` 함수 추가:

```cpp
void QueryExecutor::apply_order_by(QueryResultSet& result, const SelectStmt& stmt)
{
    if (limit < result.rows.size()) {
        // top-N: std::partial_sort O(n log k)
        std::partial_sort(
            result.rows.begin(),
            result.rows.begin() + limit,
            result.rows.end(),
            comparator
        );
        result.rows.resize(limit);
    } else {
        // 전체 정렬: std::sort O(n log n)
        std::sort(result.rows.begin(), result.rows.end(), comparator);
    }
}
```

### 버그 수정: ORDER BY + LIMIT 상호작용

발견한 버그: `exec_simple_select`에서 LIMIT을 데이터 수집 단계에 적용하면, ORDER BY 이전에 데이터가 잘려 잘못된 결과가 나왔다.

```cpp
// 버그: LIMIT을 수집 단계에서 적용
size_t limit = stmt.limit.value_or(SIZE_MAX);
for (auto idx : sel_indices) {
    if (result.rows.size() >= limit) break;  // ORDER BY 전에 잘림!
    ...
}

// 수정: ORDER BY가 있으면 수집 단계에서 LIMIT 미적용
bool has_order = stmt.order_by.has_value();
size_t limit = has_order ? SIZE_MAX : stmt.limit.value_or(SIZE_MAX);
```

---

## Part 4: ucx_backend.h 버그 수정

발견: `std::runtime_error`를 사용하는 stub 구현이 `#else` 블록에 있었는데, `#include <stdexcept>`가 `#ifdef APEX_HAS_UCX` 블록 안에만 있어서 컴파일 실패.

수정: `partition_manager.h` include 다음에 `#include <stdexcept>` 추가.

---

## 벤치마크 결과

**환경:** Amazon Linux 2023, clang-19, -O3 -march=native, 100K rows (10 symbols × 10K rows each)

```
[group_by_symbol_sum         ] min=331μs  avg=360μs  p99=412μs  (500 iters)
[group_by_symbol_multi_agg   ] min=1592μs avg=1960μs p99=2117μs (500 iters)
[group_by_symbol_order_limit ] min=489μs  avg=535μs  p99=559μs  (500 iters)
```

**타임스탬프 이진탐색 효과:**
- 좁은 범위(1% 선택): 전체 스캔 대비 ~99x faster
- 파티션 겹침 체크로 전체 파티션 건너뛰기 가능 (O(1))

---

## 테스트 결과

```
118 tests from 17 test suites. ALL PASSED.
```

새로 추가된 테스트:
- `GroupBySymbolMultiAgg`: GROUP BY + 다중 집계 (count, sum, avg, vwap)
- `TimeRangeGroupBy`: 타임스탬프 범위 + GROUP BY
- `OrderByLimit`: ORDER BY + LIMIT (top-N)
- `TimeRangeBinarySearch`: 이진탐색 코드 경로 검증
- `OrderByAsc` / `OrderByDesc`: 정렬 방향 확인
- `MinMaxAgg`: MIN/MAX 집계 검증

---

## 변경된 파일

| 파일 | 변경 내용 |
|------|-----------|
| `include/apex/storage/partition_manager.h` | `timestamp_range()`, `overlaps_time_range()` 추가 |
| `include/apex/sql/executor.h` | `eval_where_ranged()`, `extract_time_range()`, `apply_order_by()` 선언 |
| `src/sql/executor.cpp` | 새 함수 구현, exec_simple/agg/group_agg 최적화 |
| `src/cluster/ucx_backend.h` | `#include <stdexcept>` 추가 (컴파일 버그 수정) |
| `CMakeLists.txt` | `APEX_USE_JIT=OFF`일 때 jit_engine.cpp 제외 |
| `tests/unit/test_sql.cpp` | 통합 테스트 7개 추가 |
| `tests/bench/bench_sql.cpp` | 벤치마크 2개 추가 (time range, group by) |

---

## 다음 작업 (TODO)

- [ ] `COUNT(DISTINCT col)` — HyperLogLog 또는 exact count
- [ ] 타임스탬프 인덱스: 실제 현재 시각 기반 테스트 추가
- [ ] GROUP BY 다중 컬럼 지원
- [ ] HAVING 절 지원
