# ZeptoDB SQL 레퍼런스

*최종 업데이트: 2026-03-22*

ZeptoDB는 재귀 하강 SQL 파서를 사용하며 나노초 타임스탬프 시맨틱을 따릅니다.
모든 정수 컬럼은 `int64`입니다. 부동소수점 값은 고정소수점 스케일 정수로 저장됩니다.
`NULL`은 내부적으로 `INT64_MIN`으로 표현됩니다.

---

## 목차

- [SELECT 문법](#select-문법)
- [WHERE 조건](#where-조건)
- [집계 함수](#집계-함수)
- [GROUP BY / HAVING / ORDER BY / LIMIT](#group-by--having--order-by--limit)
- [윈도우 함수](#윈도우-함수)
- [금융 함수](#금융-함수)
- [날짜/시간 함수](#날짜시간-함수)
- [JOIN](#join)
- [집합 연산](#집합-연산)
- [CASE WHEN](#case-when)
- [데이터 타입 및 타임스탬프 산술](#데이터-타입-및-타임스탬프-산술)

---

## 빠른 시작 예제

### 1. 기본 집계

```sql
-- symbol 1의 VWAP + 행 수
SELECT vwap(price, volume) AS vwap, count(*) AS n
FROM trades
WHERE symbol = 1
```

### 2. 5분 OHLCV 바 (kdb+ xbar 스타일)

```sql
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open,
       max(price)   AS high,
       min(price)   AS low,
       last(price)  AS close,
       sum(volume)  AS volume
FROM trades
WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)
ORDER BY bar ASC
```

### 3. 이동 평균 + EMA

```sql
SELECT timestamp, price,
       AVG(price) OVER (PARTITION BY symbol ROWS 20 PRECEDING) AS ma20,
       EMA(price, 12) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema12
FROM trades
WHERE symbol = 1
ORDER BY timestamp ASC
```

### 4. ASOF JOIN (거래 ↔ 호가)

```sql
SELECT t.symbol, t.price, q.bid, q.ask,
       t.timestamp - q.timestamp AS staleness_ns
FROM trades t
ASOF JOIN quotes q
ON t.symbol = q.symbol AND t.timestamp >= q.timestamp
WHERE t.symbol = 1
```

### 5. 최근 1시간 분별 거래량

```sql
SELECT DATE_TRUNC('min', timestamp) AS minute,
       sum(volume)         AS vol,
       vwap(price, volume) AS vwap
FROM trades
WHERE symbol = 1
  AND timestamp > NOW() - 3600000000000
GROUP BY DATE_TRUNC('min', timestamp)
ORDER BY minute ASC
```

### 6. CASE WHEN 조건부 집계

```sql
SELECT symbol,
       sum(CASE WHEN price > 15050 THEN volume ELSE 0 END) AS high_vol,
       sum(CASE WHEN price <= 15050 THEN volume ELSE 0 END) AS low_vol,
       count(*) AS total
FROM trades
GROUP BY symbol
```

### 7. UNION — 두 심볼 결과 합치기

```sql
SELECT symbol, price, volume FROM trades WHERE symbol = 1
UNION ALL
SELECT symbol, price, volume FROM trades WHERE symbol = 2
ORDER BY symbol ASC, timestamp ASC
```

---

## SELECT 문법

```
SELECT [DISTINCT] col_expr [AS alias], ...
FROM table_name [AS alias]
  [JOIN ...]
WHERE 조건
GROUP BY col_or_expr, ...
HAVING 조건
ORDER BY col [ASC|DESC], ...
LIMIT n
```

### 컬럼 표현식

```sql
-- 단순 컬럼
SELECT price FROM trades

-- 산술 연산: + - * /
SELECT price * volume AS notional FROM trades
SELECT (price - 15000) / 100 AS premium FROM trades
SELECT SUM(price * volume) AS total_notional FROM trades
SELECT AVG(price - open_price) AS avg_change FROM trades

-- 집계 내부에 산술
SELECT SUM(price * volume) / SUM(volume) AS manual_vwap FROM trades
```

### DISTINCT

```sql
SELECT DISTINCT symbol FROM trades
```

### 테이블 별칭

```sql
SELECT t.price, q.bid FROM trades t ASOF JOIN quotes q ...
```

---

## WHERE 조건

### 비교 연산자

```sql
WHERE price > 15000
WHERE price >= 15000
WHERE price < 15100
WHERE price <= 15100
WHERE price = 15000
WHERE price != 15000
```

### BETWEEN

```sql
WHERE timestamp BETWEEN 1711000000000000000 AND 1711003600000000000
WHERE price BETWEEN 15000 AND 15100
```

### AND / OR / NOT

```sql
WHERE symbol = 1 AND price > 15000
WHERE symbol = 1 OR symbol = 2
WHERE NOT price > 15100
WHERE NOT (price > 15100 OR volume < 50)
```

### IN

```sql
WHERE symbol IN (1, 2, 3)
WHERE price IN (15000, 15010, 15020)
```

### IS NULL / IS NOT NULL

ZeptoDB는 `INT64_MIN`을 NULL 센티넬로 사용합니다.

```sql
WHERE risk_score IS NULL
WHERE risk_score IS NOT NULL
```

### LIKE / NOT LIKE

int64 값의 10진수 문자열 표현에 대해 글로브 패턴 매칭을 적용합니다.

| 패턴 문자 | 의미 |
|---|---|
| `%` | 임의의 부분 문자열 (0개 이상) |
| `_` | 임의의 단일 문자 |

```sql
WHERE price LIKE '150%'         -- "150"으로 시작하는 가격
WHERE price NOT LIKE '%9'       -- "9"로 끝나지 않는 가격
WHERE price LIKE '1500_'        -- "1500"으로 시작하는 5자리 가격
WHERE timestamp LIKE '1711%'    -- 해당 접두사를 가진 타임스탬프
```

---

## 집계 함수

모든 집계는 NULL을 무시합니다. SELECT 목록이나 산술 표현식 안에 중첩 가능합니다.

| 함수 | 설명 |
|------|------|
| `COUNT(*)` | 전체 행 수 |
| `COUNT(col)` | NULL이 아닌 행 수 |
| `SUM(col)` | 합계 |
| `SUM(expr)` | 산술 표현식의 합계 (예: `SUM(price * volume)`) |
| `AVG(col)` | 평균 |
| `AVG(expr)` | 산술 표현식의 평균 |
| `MIN(col)` | 최솟값 |
| `MAX(col)` | 최댓값 |
| `FIRST(col)` | 첫 번째 값 |
| `LAST(col)` | 마지막 값 |
| `VWAP(price, volume)` | 거래량 가중 평균 가격 |

```sql
SELECT COUNT(*), SUM(volume), AVG(price), MIN(price), MAX(price),
       VWAP(price, volume), FIRST(price) AS open, LAST(price) AS close
FROM trades WHERE symbol = 1
```

---

## GROUP BY / HAVING / ORDER BY / LIMIT

### GROUP BY

```sql
-- 단일 컬럼
SELECT symbol, SUM(volume) FROM trades GROUP BY symbol

-- xbar: kdb+ 스타일 시간 버킷 (인수는 나노초)
SELECT xbar(timestamp, 300000000000) AS bar, SUM(volume)
FROM trades GROUP BY xbar(timestamp, 300000000000)

-- 다중 컬럼 (복합 키)
SELECT symbol, price, SUM(volume) AS vol
FROM trades GROUP BY symbol, price

-- 날짜/시간 함수를 키로 사용
SELECT DATE_TRUNC('hour', timestamp) AS hour, SUM(volume)
FROM trades GROUP BY DATE_TRUNC('hour', timestamp)
```

### HAVING

집계 후 적용됩니다. 결과 컬럼 별칭을 참조합니다.

```sql
SELECT symbol, SUM(volume) AS total_vol
FROM trades GROUP BY symbol
HAVING total_vol > 1000
```

### ORDER BY / LIMIT

```sql
SELECT symbol, SUM(volume) AS total_vol
FROM trades GROUP BY symbol
ORDER BY total_vol DESC
LIMIT 10
```

---

## 윈도우 함수

문법: `func(col) OVER ([PARTITION BY col] [ORDER BY col] [ROWS n PRECEDING])`

| 함수 | 설명 |
|------|------|
| `SUM(col) OVER (...)` | 누적 합계 |
| `AVG(col) OVER (...)` | 이동 평균 |
| `MIN/MAX(col) OVER (...)` | 이동 최솟값/최댓값 |
| `ROW_NUMBER() OVER (...)` | 파티션 내 행 번호 |
| `RANK() OVER (...)` | 순위 (동점 시 빈 순위) |
| `DENSE_RANK() OVER (...)` | 순위 (동점 시 빈 순위 없음) |
| `LAG(col, n) OVER (...)` | n행 이전 값 |
| `LEAD(col, n) OVER (...)` | n행 이후 값 |

```sql
SELECT price,
       AVG(price) OVER (PARTITION BY symbol ROWS 20 PRECEDING) AS ma20,
       LAG(price, 1)  OVER (PARTITION BY symbol ORDER BY timestamp) AS prev_price,
       LEAD(price, 1) OVER (PARTITION BY symbol ORDER BY timestamp) AS next_price
FROM trades
```

---

## 금융 함수

### EMA (지수 이동 평균)

```sql
SELECT EMA(price, 20) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema20
FROM trades
```

### DELTA / RATIO

```sql
SELECT DELTA(price) OVER (ORDER BY timestamp) AS price_change FROM trades
SELECT RATIO(price) OVER (ORDER BY timestamp) AS price_ratio  FROM trades
```

### xbar (시간 바 집계)

타임스탬프를 나노초 단위 고정 크기 구간으로 버킷화합니다.

```sql
-- 5분 OHLCV 캔들스틱
SELECT xbar(timestamp, 300000000000) AS bar,
       FIRST(price) AS open,  MAX(price) AS high,
       MIN(price)   AS low,   LAST(price) AS close,
       SUM(volume)  AS volume
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)
ORDER BY bar ASC
```

자주 사용하는 바 크기:

| 주기 | 나노초 |
|------|--------|
| 1초 | `1_000_000_000` |
| 1분 | `60_000_000_000` |
| 5분 | `300_000_000_000` |
| 1시간 | `3_600_000_000_000` |
| 1일 | `86_400_000_000_000` |

---

## 날짜/시간 함수

모든 ZeptoDB 타임스탬프는 유닉스 에포크 이후 **나노초** (int64)입니다.

### DATE_TRUNC

나노초 타임스탬프를 시간 단위 경계로 내림합니다.

```sql
DATE_TRUNC('unit', column_or_expr)
```

| 단위 | 버킷 크기 (ns) |
|------|---------------|
| `'ns'` | 1 |
| `'us'` | 1,000 |
| `'ms'` | 1,000,000 |
| `'s'` | 1,000,000,000 |
| `'min'` | 60,000,000,000 |
| `'hour'` | 3,600,000,000,000 |
| `'day'` | 86,400,000,000,000 |
| `'week'` | 604,800,000,000,000 |

```sql
SELECT DATE_TRUNC('min', timestamp) AS minute, SUM(volume) AS vol
FROM trades WHERE symbol = 1
GROUP BY DATE_TRUNC('min', timestamp)
ORDER BY minute ASC
```

### NOW()

쿼리 실행 시점의 나노초 타임스탬프를 반환합니다.

```sql
SELECT * FROM trades WHERE timestamp > NOW() - 60000000000
SELECT EPOCH_S(NOW()) - EPOCH_S(timestamp) AS age_sec FROM trades
```

### EPOCH_S / EPOCH_MS

나노초 타임스탬프를 초 또는 밀리초로 변환합니다.

```sql
SELECT EPOCH_S(timestamp)  AS ts_sec FROM trades WHERE symbol = 1
SELECT EPOCH_MS(timestamp) AS ts_ms  FROM trades WHERE symbol = 1
```

---

## JOIN

### ASOF JOIN (시계열, kdb+ 스타일)

왼쪽 각 행에 대해 `right.timestamp <= left.timestamp`를 만족하는 가장 최근 오른쪽 행을 찾습니다.

```sql
SELECT t.symbol, t.price, q.bid, q.ask,
       t.timestamp - q.timestamp AS staleness_ns
FROM trades t
ASOF JOIN quotes q
ON t.symbol = q.symbol AND t.timestamp >= q.timestamp
```

### Hash JOIN (동등 조인)

```sql
SELECT t.price, t.volume, r.risk_score
FROM trades t
JOIN risk_factors r ON t.symbol = r.symbol
```

### LEFT JOIN

왼쪽 모든 행을 반환하며, 매칭되지 않은 오른쪽 컬럼은 NULL(INT64_MIN)입니다.

```sql
SELECT t.price, t.volume, r.risk_score
FROM trades t
LEFT JOIN risk_factors r ON t.symbol = r.symbol
WHERE r.risk_score IS NOT NULL
```

### WINDOW JOIN (wj, kdb+ 스타일)

왼쪽 각 행에 대해 시간 윈도우 내 오른쪽 행을 집계합니다.

```sql
SELECT t.price,
       wj_avg(q.bid)   AS avg_bid,
       wj_count(q.bid) AS quote_count
FROM trades t
WINDOW JOIN quotes q
ON t.symbol = q.symbol
AND q.timestamp BETWEEN t.timestamp - 5000000000 AND t.timestamp + 5000000000
```

윈도우 집계: `wj_avg`, `wj_sum`, `wj_min`, `wj_max`, `wj_count`

---

## 집합 연산

모든 집합 연산은 양쪽 SELECT 목록의 컬럼 수가 동일해야 합니다.

```sql
-- UNION ALL: 중복 유지 연결
SELECT price FROM trades WHERE symbol = 1
UNION ALL
SELECT price FROM trades WHERE symbol = 2

-- UNION: 중복 제거 연결
SELECT price FROM trades WHERE symbol = 1
UNION
SELECT price FROM trades WHERE symbol = 2

-- INTERSECT: 양쪽에 존재하는 행
SELECT price FROM trades WHERE symbol = 1
INTERSECT
SELECT price FROM trades WHERE price > 15050

-- EXCEPT: 왼쪽에는 있지만 오른쪽에는 없는 행
SELECT price FROM trades WHERE symbol = 1
EXCEPT
SELECT price FROM trades WHERE price > 15050
```

---

## CASE WHEN

```
CASE
  WHEN 조건 THEN 산술_표현식
  [WHEN 조건 THEN 산술_표현식 ...]
  [ELSE 산술_표현식]
END [AS alias]
```

```sql
SELECT price,
       CASE WHEN price > 15050 THEN 1 ELSE 0 END AS is_high
FROM trades WHERE symbol = 1

SELECT price, volume,
       CASE
           WHEN price > 15050 THEN price * 2
           WHEN price > 15020 THEN price * 1
           ELSE 0
       END AS weighted_price
FROM trades

-- 집계 내 사용
SELECT SUM(CASE WHEN price > 15050 THEN volume ELSE 0 END) AS high_volume
FROM trades WHERE symbol = 1
```

---

## 데이터 타입 및 타임스탬프 산술

모든 컬럼은 스토리지 수준에서 `int64`입니다.

| 논리 타입 | 스토리지 | 비고 |
|-----------|----------|------|
| 정수 | `int64` | 직접 저장 |
| 가격 (float) | `int64` | 스케일: 150.25 → scale 100일 때 15025 |
| 타임스탬프 | `int64` | 유닉스 에포크 이후 나노초 |
| 심볼 ID | `int64` | 숫자형 심볼 식별자 |
| NULL | `INT64_MIN` | IS NULL 체크에 사용 |

### 타임스탬프 산술

```sql
WHERE timestamp > NOW() - 60000000000    -- 최근 1분
WHERE timestamp > NOW() - 300000000000   -- 최근 5분
```

단위 참조표:

| 단위 | 나노초 |
|------|--------|
| 1 ns | `1` |
| 1 μs | `1_000` |
| 1 ms | `1_000_000` |
| 1 s | `1_000_000_000` |
| 1 min | `60_000_000_000` |
| 1 hour | `3_600_000_000_000` |
| 1 day | `86_400_000_000_000` |

---

*참고: [Python 레퍼런스](PYTHON_REFERENCE_ko.md) · [C++ 레퍼런스](CPP_REFERENCE_ko.md) · [HTTP 레퍼런스](HTTP_REFERENCE_ko.md)*
