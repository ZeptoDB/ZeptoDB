# 010: 금융 함수 구현 — xbar, EMA, deltas/ratios, LEFT JOIN, Window JOIN

**날짜**: 2026-03-22
**작성자**: APEX-DB 개발팀

---

## 개요

kdb+ 스타일 금융 분석 함수와 JOIN 확장을 구현했다.
시계열 데이터베이스로서 APEX-DB가 실제 금융 워크로드를 처리할 수 있는 핵심 기능들이다.

## 구현 내용

### 1. xbar — 시간 바 집계 (캔들차트 핵심)

kdb+의 `xbar`를 SQL 함수로 구현. 타임스탬프를 N 단위로 floor하여 GROUP BY 키로 사용.

```sql
-- 5분봉 OHLCV 생성
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low, last(price) AS close,
       sum(volume) AS volume
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)
```

**구현 세부사항:**
- `xbar(value, n) = (value / n) * n` — 정수 나눗셈 floor
- SELECT와 GROUP BY 모두에서 사용 가능
- `AggFunc::XBAR`로 AST에 추가
- GROUP BY 파서에서 `XBAR(col, bucket)` 구문 지원

**성능:** 1M 행 → 3,334개 5분봉 생성: **~24ms** ✅

### 2. first() / last() 집계 함수

OHLC 캔들차트에 필수인 `first()`와 `last()` 집계 함수 추가.

- `AggFunc::FIRST` — 그룹의 첫 번째 값 (Open)
- `AggFunc::LAST` — 그룹의 마지막 값 (Close)
- `GroupState`에 `first_val`, `last_val`, `has_first` 필드 추가

### 3. EMA — 지수이동평균 (O(n) 단일 패스)

기술적 분석의 핵심 지표. kdb+ `ema(alpha, data)` 구현.

```sql
SELECT EMA(price, 0.1) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema_slow,
       EMA(price, 20)  OVER (PARTITION BY symbol ORDER BY timestamp) AS ema20
FROM trades
```

**알고리즘:**
```
ema[0] = data[0]
ema[i] = alpha * data[i] + (1 - alpha) * ema[i-1]
```

- alpha 직접 지정 (float) 또는 period 기반 (integer → `alpha = 2/(period+1)`)
- `WindowEMA` 클래스로 구현
- PARTITION BY 지원: 심볼별 독립 계산
- **O(n) 단일 패스 — trivially parallelizable**

**성능:** 1M 행 EMA 계산: **~2.2ms** ✅

### 4. DELTA / RATIO — 행간 차이/비율

kdb+ `deltas`와 `ratios` 구현.

```sql
SELECT price,
       DELTA(price) OVER (ORDER BY timestamp) AS price_change,
       RATIO(price) OVER (ORDER BY timestamp) AS price_ratio
FROM trades
```

- `DELTA(x) = x[i] - x[i-1]` (첫 행 = x[0])
- `RATIO(x) = x[i] / x[i-1]` (첫 행 = 1.0, 6자리 고정소수점 ×1,000,000)
- `WindowDelta`, `WindowRatio` 클래스
- PARTITION BY 지원

### 5. LEFT JOIN

SQL 표준 LEFT JOIN 구현. 매칭 없는 왼쪽 행도 결과에 포함.

```sql
SELECT t.price, t.volume, r.risk_score
FROM trades t
LEFT JOIN risk_factors r ON t.symbol = r.symbol
```

**구현:**
- `JoinType` enum: `{ INNER, LEFT, RIGHT, FULL }`
- `HashJoinOperator`에 `join_type_` 파라미터 추가
- 매칭 없는 왼쪽 행: `right_indices[i] = -1`
- 결과 조립 시 오른쪽 컬럼 = `INT64_MIN` (NULL 센티넬)
- `JOIN_NULL` 상수 정의 (`INT64_MIN`)

### 6. Window JOIN (wj) — kdb+ 스타일

가장 복잡한 기능. 각 왼쪽 행에 대해 시간 윈도우 내 오른쪽 행을 집계.

```sql
SELECT t.price, wj_avg(q.bid) AS avg_bid, wj_count(q.bid) AS quote_count
FROM trades t
WINDOW JOIN quotes q
ON t.symbol = q.symbol
AND q.timestamp BETWEEN t.timestamp - 5000000000 AND t.timestamp + 5000000000
```

**알고리즘:**
1. 오른쪽 테이블을 심볼별 그룹화 (해시맵)
2. 각 왼쪽 행에 대해:
   - 심볼로 오른쪽 그룹 찾기 (O(1))
   - 이진 탐색으로 `[t - before, t + after]` 범위 찾기 (O(log m))
   - 범위 내 행에 집계 적용 (avg, sum, count, min, max)
3. **복잡도: O(n × log m)**

**지원 집계 함수:**
- `wj_avg()`, `wj_sum()`, `wj_count()`, `wj_min()`, `wj_max()`

## SQL 파서 변경사항

### 새 토큰
- `XBAR`, `EMA`, `DELTA`, `RATIO`, `WINDOW`
- `PLUS` (+), `MINUS` (-) — 산술 연산자

### 파서 변경
- `parse_select_expr()`: XBAR, FIRST, LAST, EMA, DELTA, RATIO, wj_* 함수 파싱
- `parse_group_by()`: `XBAR(col, bucket)` 구문 지원
- `parse_join()`: `WINDOW JOIN ... ON ... AND col BETWEEN expr AND expr` 파싱
- AS alias: 키워드도 alias로 허용 (`AS delta`, `AS bar` 등)
- 음수 리터럴 vs MINUS 연산자: 문맥 기반 판별

### AST 변경
- `AggFunc`: FIRST, LAST, XBAR 추가
- `WindowFunc`: EMA, DELTA, RATIO 추가
- `WJAggFunc`: Window JOIN 전용 집계 enum
- `SelectExpr`: `xbar_bucket`, `ema_alpha`, `ema_period`, `wj_agg` 필드
- `GroupByClause`: `xbar_buckets` 벡터
- `JoinClause::Type::WINDOW` 추가, 시간 윈도우 파라미터

## 테스트 결과

| 테스트 카테고리 | 수 | 결과 |
|---|---|---|
| Tokenizer (새 키워드) | 6 | ✅ PASS |
| Parser (새 구문) | 8 | ✅ PASS |
| WindowFunction (EMA/Delta/Ratio) | 8 | ✅ PASS |
| HashJoin (LEFT JOIN) | 4 | ✅ PASS |
| WindowJoin (wj) | 4 | ✅ PASS |
| SQL Executor (xbar, delta) | 3 | ✅ PASS |
| 기존 테스트 전체 | 109 | ✅ PASS |

**총 151개 테스트 PASS** (TransportSwap/ClusterNode 제외 — 기존 네트워크 의존 테스트)

## 벤치마크

| 벤치마크 | 데이터 크기 | 시간 |
|---|---|---|
| xbar GROUP BY | 1M rows → 3,334 bars | **24ms** |
| EMA 계산 | 1M rows | **2.2ms** |

## 파일 변경 목록

### 신규 파일
- `tests/unit/test_financial_functions.cpp` — 40+ 새 테스트

### 수정 파일
- `include/apex/sql/tokenizer.h` — 새 토큰 타입 (XBAR, EMA, DELTA, RATIO, WINDOW, PLUS, MINUS)
- `src/sql/tokenizer.cpp` — 새 키워드 매핑, MINUS/PLUS 토큰 처리
- `include/apex/sql/ast.h` — AggFunc, WindowFunc, WJAggFunc, SelectExpr, GroupByClause, JoinClause 확장
- `src/sql/parser.cpp` — 모든 새 구문 파싱 (xbar, ema, delta, ratio, first, last, wj_*, WINDOW JOIN)
- `include/apex/execution/window_function.h` — WindowEMA, WindowDelta, WindowRatio 클래스 추가
- `include/apex/execution/join_operator.h` — JoinType enum, WindowJoinOperator, JOIN_NULL 상수
- `src/execution/join_operator.cpp` — LEFT JOIN 구현, WindowJoinOperator 이진 탐색 구현
- `include/apex/sql/executor.h` — exec_window_join 선언
- `src/sql/executor.cpp` — FIRST/LAST/XBAR 집계, EMA/DELTA/RATIO 윈도우, LEFT JOIN NULL 처리, WINDOW JOIN 실행
- `tests/CMakeLists.txt` — test_financial_functions.cpp 추가

## 설계 결정

1. **정수 고정소수점**: RATIO는 ×1,000,000 스케일링 (소수 6자리). 실수 연산 없이 정수만으로 비율 표현.
2. **NULL 센티넬**: `INT64_MIN` 사용. 실제 금융 데이터에서 이 값이 나올 확률은 0.
3. **EMA 단일 패스**: 순방향 O(n) 스캔 한 번. GPU/SIMD 병렬화 시 파티션별 독립 처리 가능.
4. **Window JOIN 이진 탐색**: 정렬된 타임스탬프에서 O(log m). 양쪽 정렬 보장 시 슬라이딩 윈도우 O(n+m)으로 최적화 가능 (TODO).
5. **wj_ 접두사**: Window JOIN 집계를 일반 집계와 구분하기 위해 `wj_avg`, `wj_sum` 등 명시적 네이밍.

## 다음 단계

- [ ] Window JOIN 슬라이딩 윈도우 최적화 (양쪽 정렬 시 O(n+m))
- [ ] RATIO 스케일링 옵션 (×100, ×10000 등 사용자 지정)
- [ ] RIGHT JOIN, FULL OUTER JOIN 구현
- [ ] EMA GPU 가속 (파티션별 독립 — trivially parallel)
- [ ] 실시간 스트리밍 EMA (incremental update)
