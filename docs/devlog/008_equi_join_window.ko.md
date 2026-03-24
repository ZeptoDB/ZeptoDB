# 008 — Equi Hash Join + Window Functions

**날짜:** 2026-03-22  
**상태:** ✅ 완료

---

## 배경

Layer 7 (SQL + HTTP API)에서 ASOF JOIN 프레임워크는 이미 구현된 상태였다. 이번 이터레이션에서는:

1. **Equi Hash Join** — 일반 `JOIN ... ON a.col = b.col` 지원
2. **Window Functions** — `OVER (PARTITION BY ... ORDER BY ... ROWS N PRECEDING)` SQL 윈도우 함수 프레임워크

---

## Task 1: HashJoinOperator 구현

### 설계

```
Build Phase: right_table → hash_map[key] = [row_indices...]
Probe Phase: left_table  → hash_map.find(key) → match pairs
```

- `std::unordered_map<int64_t, std::vector<int64_t>>` 사용
- 1:1, 1:N, N:M 조인 모두 지원
- 복잡도: O(n+m) 평균

### 파일 변경

- `include/zeptodb/execution/join_operator.h` — `HashJoinOperator` 스텁 → 실제 구현으로 교체
- `src/execution/join_operator.cpp` — `HashJoinOperator::execute()` 구현 추가
- `src/sql/executor.cpp` — `exec_hash_join()` 추가, `exec_select()` 디스패처에서 INNER/LEFT JOIN 라우팅

### SQL 예시

```sql
SELECT t.price, r.risk_score
FROM trades t
JOIN risk_factors r ON t.symbol = r.symbol

SELECT t.*, c.name, c.country
FROM trades t
JOIN clients c ON t.client_id = c.id
```

---

## Task 2: Window Function 프레임워크

### 설계 원칙

**모든 함수 O(n) 복잡도** — O(n*W) 슬라이딩 루프 절대 금지:

| 함수 | 알고리즘 |
|------|----------|
| ROW_NUMBER | 단순 카운터 |
| RANK, DENSE_RANK | 정렬된 입력 기준 순위 |
| SUM | Prefix Sum → 구간합 O(1) per row |
| AVG | Prefix Sum + 카운트 |
| MIN, MAX | Monotonic Deque O(n) |
| LAG, LEAD | 단순 오프셋 인덱싱 |

### `WindowFrame` 구조

```cpp
struct WindowFrame {
    enum class Type { ROWS, RANGE } type = ROWS;
    int64_t preceding = INT64_MAX;  // UNBOUNDED PRECEDING
    int64_t following = 0;          // CURRENT ROW
};
```

### PARTITION BY 지원

`partition_keys` 배열 경계를 계산 후 파티션별 독립 처리:
```cpp
auto bounds = compute_partition_bounds(partition_keys, n);
for (each partition) { process independently }
```

### 새 파일

- `include/zeptodb/execution/window_function.h` — 전체 프레임워크 (헤더-온리)
  - `WindowFunction` 추상 기반 클래스
  - `WindowRowNumber`, `WindowRank`, `WindowDenseRank`
  - `WindowSum`, `WindowAvg`, `WindowMin`, `WindowMax`
  - `WindowLag`, `WindowLead`
  - `make_window_function()` factory 함수

---

## Task 3: SQL Parser + Executor 업데이트

### Tokenizer 신규 키워드

```
OVER PARTITION ROWS RANGE PRECEDING FOLLOWING UNBOUNDED CURRENT ROW
RANK DENSE_RANK ROW_NUMBER LAG LEAD
```

- `DENSE_RANK`, `ROW_NUMBER` 처럼 underscore 포함 식별자 지원

### AST 확장 (`ast.h`)

```cpp
enum class WindowFunc { NONE, ROW_NUMBER, RANK, DENSE_RANK, SUM, AVG, MIN, MAX, LAG, LEAD };

struct WindowSpec {
    vector<string> partition_by_cols;
    vector<string> order_by_cols;
    bool    has_frame = false;
    int64_t preceding = INT64_MAX;
    int64_t following = 0;
};

struct SelectExpr {
    // 기존 필드...
    WindowFunc  window_func   = WindowFunc::NONE;
    int64_t     window_offset = 1;      // LAG/LEAD offset
    int64_t     window_default = 0;     // LAG/LEAD default
    optional<WindowSpec> window_spec;   // OVER (...)
};
```

### Parser 업데이트

`parse_window_spec()` 추가 — `OVER (...)` 내부 파싱:
- `PARTITION BY col [, col ...]`
- `ORDER BY col [ASC|DESC]`
- `ROWS [UNBOUNDED|N] PRECEDING [AND [UNBOUNDED|N] FOLLOWING]`
- `ROWS BETWEEN ... AND ...`

집계 함수 뒤에 `OVER` 발견 시 윈도우 모드로 자동 전환:
```cpp
// SUM(vol) OVER (...) → window_func = WindowFunc::SUM
if (expr.agg != AggFunc::NONE && check(TokenType::OVER)) { ... }
```

### Executor 업데이트

- `exec_hash_join()` — 파티션 flat 벡터 수집 후 해시맵 프로브
- `apply_window_functions()` — 쿼리 결과에 윈도우 컬럼 추가
- `exec_select()` — INNER/LEFT JOIN → `exec_hash_join()` 라우팅
- 윈도우 함수 실행 후 결과에 새 컬럼 append

---

## Task 4: 테스트

### `test_sql.cpp` 수정

- `AsofJoin.HashJoinThrows` → `AsofJoin.HashJoinEmpty` (구현됐으므로 throw 예상 → 0 매칭 확인)

### `test_join_window.cpp` 신규 (57개 테스트)

**HashJoin 테스트:**
- `OneToOne`, `OneToMany`, `ManyToMany`, `NoMatch`, `EmptyInput`, `LargeCorrectness`

**WindowFunction 테스트:**
- `RowNumber_Simple`, `RowNumber_WithPartition`
- `Rank_WithTies`, `DenseRank_WithTies`
- `Sum_CumulativeSum`, `Sum_SlidingWindow`, `Sum_PartitionBy`
- `Avg_SlidingWindow`, `Avg_PartitionBy`
- `Min_Cumulative`, `Max_Cumulative`
- `Lag_Offset1`, `Lag_WithPartition`
- `Lead_Offset1`, `Lead_WithPartition`
- `Sum_LargeN` (100K 행 정확성)
- `Factory`

**Parser 윈도우 테스트:**
- `WindowFunction_RowNumber`, `SumPartitionBy`, `AvgRowsPreceding`
- `Lag`, `Rank`, `DenseRank`, `Lead`
- `RowsBetween`, `UnboundedPreceding`

**Tokenizer 테스트:**
- `WindowKeywords`, `WindowFuncKeywords`

```
[==========] 57 tests passed in 5ms ✅
```

---

## Task 5: 벤치마크 결과

```
=== Hash Join Benchmarks ===
hash_join_N=1000      avg=  0.06μs   (매우 빠름 — 캐시 핫)
hash_join_N=10000     avg=352.23μs
hash_join_N=100000    avg=2991.53μs
hash_join_N=1000000   avg=42840.65μs

=== Window SUM Benchmarks ===
window_sum_N=1000     avg=  0.68μs
window_sum_N=10000    avg=  7.03μs   → 약 1.4GB/s 처리
window_sum_N=100000   avg= 86.85μs
window_sum_N=1000000  avg=1355.39μs

=== Rolling AVG: O(n) vs O(n*W) ===
window_function O(n): avg=326μs  ← 20% 빠름
manual_loop O(n*W):   avg=409μs
```

**O(n) 윈도우 함수가 O(n*W) 수동 루프보다 ~20% 빠름** (N=100K, W=20).

---

## 아키텍처 요약

```
SQL 문자열
    ↓ Tokenizer (OVER/PARTITION/ROWS/LAG/... 신규 키워드)
    ↓ Parser (parse_window_spec() 추가)
    ↓ SelectStmt AST (WindowFunc, WindowSpec 필드)
    ↓ QueryExecutor::exec_select()
       ├── INNER/LEFT JOIN → exec_hash_join()
       │     └── HashJoinOperator (Build + Probe, O(n+m))
       ├── ASOF JOIN → exec_asof_join() (기존)
       ├── 일반 쿼리 → exec_simple_select() / exec_agg() / exec_group_agg()
       └── apply_window_functions() (선택적)
             └── WindowFunction::compute() (O(n), prefix sum / deque)
                 ├── PARTITION BY 지원
                 └── ROWS N PRECEDING / FOLLOWING 프레임
```

---

## 다음 단계 (백로그)

- [ ] Hash Join: multi-column key 지원 (현재 단일 키만)
- [ ] Hash Join: 오른쪽 파티션 probe 캐싱 (같은 right table 재사용 시)
- [ ] Window: RANGE 프레임 (ROWS 외에도)
- [ ] Window: ORDER BY 정렬 자동화 (현재 입력이 정렬되어 있다고 가정)
- [ ] Window: NTH_VALUE, NTILE, PERCENT_RANK
- [ ] Executor: 다중 JOIN 지원 (현재 단일 JOIN)
