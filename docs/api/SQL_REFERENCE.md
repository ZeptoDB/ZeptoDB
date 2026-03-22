# APEX-DB SQL Reference

*Last updated: 2026-03-22*

APEX-DB uses a recursive descent SQL parser with nanosecond timestamp semantics.
All integer columns are `int64`. Floating-point values are stored as fixed-point scaled integers.
`NULL` is represented internally as `INT64_MIN`.

---

## Table of Contents

- [SELECT Syntax](#select-syntax)
- [WHERE Conditions](#where-conditions)
- [Aggregate Functions](#aggregate-functions)
- [GROUP BY / HAVING / ORDER BY / LIMIT](#group-by--having--order-by--limit)
- [Window Functions](#window-functions)
- [Financial Functions](#financial-functions)
- [Date/Time Functions](#datetime-functions)
- [JOINs](#joins)
- [Set Operations](#set-operations)
- [CASE WHEN](#case-when)
- [Data Types & Timestamp Arithmetic](#data-types--timestamp-arithmetic)

---

## Quick Start

### 1. Basic aggregation

```sql
-- VWAP + row count for symbol 1
SELECT vwap(price, volume) AS vwap, count(*) AS n
FROM trades
WHERE symbol = 1
```

### 2. 5-minute OHLCV bar (kdb+ xbar style)

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

### 3. Moving average + EMA

```sql
SELECT timestamp, price,
       AVG(price) OVER (PARTITION BY symbol ROWS 20 PRECEDING) AS ma20,
       EMA(price, 12) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema12
FROM trades
WHERE symbol = 1
ORDER BY timestamp ASC
```

### 4. ASOF JOIN (trades ↔ quotes)

```sql
SELECT t.symbol, t.price, q.bid, q.ask,
       t.timestamp - q.timestamp AS staleness_ns
FROM trades t
ASOF JOIN quotes q
ON t.symbol = q.symbol AND t.timestamp >= q.timestamp
WHERE t.symbol = 1
```

### 5. Per-minute volume with time filter

```sql
SELECT DATE_TRUNC('min', timestamp) AS minute,
       sum(volume)          AS vol,
       vwap(price, volume)  AS vwap
FROM trades
WHERE symbol = 1
  AND timestamp > NOW() - 3600000000000
GROUP BY DATE_TRUNC('min', timestamp)
ORDER BY minute ASC
```

### 6. Conditional aggregation with CASE WHEN

```sql
SELECT symbol,
       sum(CASE WHEN price > 15050 THEN volume ELSE 0 END) AS high_vol,
       sum(CASE WHEN price <= 15050 THEN volume ELSE 0 END) AS low_vol,
       count(*) AS total
FROM trades
GROUP BY symbol
```

### 7. UNION — combine two symbol results

```sql
SELECT symbol, price, volume FROM trades WHERE symbol = 1
UNION ALL
SELECT symbol, price, volume FROM trades WHERE symbol = 2
ORDER BY symbol ASC, timestamp ASC
```

---

## SELECT Syntax

```
SELECT [DISTINCT] col_expr [AS alias], ...
FROM table_name [AS alias]
  [JOIN ...]
WHERE condition
GROUP BY col_or_expr, ...
HAVING condition
ORDER BY col [ASC|DESC], ...
LIMIT n
```

### Column expressions

```sql
-- Plain column
SELECT price FROM trades

-- Arithmetic: + - * /
SELECT price * volume AS notional FROM trades
SELECT (price - 15000) / 100 AS premium FROM trades
SELECT SUM(price * volume) AS total_notional FROM trades
SELECT AVG(price - open_price) AS avg_change FROM trades

-- Aggregate with arithmetic inside
SELECT SUM(price * volume) / SUM(volume) AS manual_vwap FROM trades
```

### DISTINCT

```sql
SELECT DISTINCT symbol FROM trades
```

### Table alias

```sql
SELECT t.price, q.bid FROM trades t ASOF JOIN quotes q ...
```

---

## WHERE Conditions

### Comparison operators

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

APEX-DB uses `INT64_MIN` as the NULL sentinel.

```sql
WHERE risk_score IS NULL
WHERE risk_score IS NOT NULL
```

### LIKE / NOT LIKE

Glob-style pattern matching applied to the decimal string representation of int64 values.

| Pattern char | Meaning |
|---|---|
| `%` | Any substring (0 or more characters) |
| `_` | Any single character |

```sql
WHERE price LIKE '150%'         -- prices starting with "150"
WHERE price NOT LIKE '%9'       -- prices not ending in "9"
WHERE price LIKE '1500_'        -- 5-char prices starting with "1500"
WHERE timestamp LIKE '1711%'    -- timestamps with that prefix
```

---

## Aggregate Functions

All aggregates ignore NULL. Can be used in SELECT list or nested in expressions.

| Function | Description |
|----------|-------------|
| `COUNT(*)` | Total row count |
| `COUNT(col)` | Non-null row count |
| `SUM(col)` | Sum |
| `SUM(expr)` | Sum of arithmetic expression, e.g. `SUM(price * volume)` |
| `AVG(col)` | Average |
| `AVG(expr)` | Average of arithmetic expression |
| `MIN(col)` | Minimum |
| `MAX(col)` | Maximum |
| `FIRST(col)` | First value (by row order) |
| `LAST(col)` | Last value (by row order) |
| `VWAP(price, volume)` | Volume-weighted average price |

```sql
SELECT COUNT(*), SUM(volume), AVG(price), MIN(price), MAX(price),
       VWAP(price, volume), FIRST(price) AS open, LAST(price) AS close
FROM trades WHERE symbol = 1
```

---

## GROUP BY / HAVING / ORDER BY / LIMIT

### GROUP BY

```sql
-- Single column
SELECT symbol, SUM(volume) FROM trades GROUP BY symbol

-- xbar: kdb+ style time bucketing (arg = nanoseconds)
SELECT xbar(timestamp, 300000000000) AS bar, SUM(volume)
FROM trades GROUP BY xbar(timestamp, 300000000000)

-- Multi-column (composite key)
SELECT symbol, price, SUM(volume) AS vol
FROM trades GROUP BY symbol, price

-- Date/time function as key
SELECT DATE_TRUNC('hour', timestamp) AS hour, SUM(volume)
FROM trades GROUP BY DATE_TRUNC('hour', timestamp)
```

### HAVING

Applied after aggregation. References result column aliases.

```sql
SELECT symbol, SUM(volume) AS total_vol
FROM trades GROUP BY symbol
HAVING total_vol > 1000

SELECT symbol, AVG(price) AS avg_price
FROM trades GROUP BY symbol
HAVING avg_price > 15000 AND avg_price < 20000
```

### ORDER BY / LIMIT

```sql
SELECT symbol, SUM(volume) AS total_vol
FROM trades GROUP BY symbol
ORDER BY total_vol DESC
LIMIT 10

-- Multi-column ORDER BY
ORDER BY symbol ASC, price DESC
```

---

## Window Functions

Syntax: `func(col) OVER ([PARTITION BY col] [ORDER BY col] [ROWS n PRECEDING])`

| Function | Description |
|----------|-------------|
| `SUM(col) OVER (...)` | Running sum |
| `AVG(col) OVER (...)` | Moving average |
| `MIN(col) OVER (...)` | Moving minimum |
| `MAX(col) OVER (...)` | Moving maximum |
| `COUNT(col) OVER (...)` | Running count |
| `ROW_NUMBER() OVER (...)` | Row number within partition |
| `RANK() OVER (...)` | Rank (gaps on tie) |
| `DENSE_RANK() OVER (...)` | Rank (no gaps) |
| `LAG(col, n) OVER (...)` | Value n rows before |
| `LEAD(col, n) OVER (...)` | Value n rows ahead |

```sql
-- 20-row moving average
SELECT price,
       AVG(price) OVER (PARTITION BY symbol ROWS 20 PRECEDING) AS ma20
FROM trades

-- Rank by price descending
SELECT symbol, price,
       RANK() OVER (ORDER BY price DESC) AS rank
FROM trades

-- LAG / LEAD
SELECT price,
       LAG(price, 1)  OVER (PARTITION BY symbol ORDER BY timestamp) AS prev_price,
       LEAD(price, 1) OVER (PARTITION BY symbol ORDER BY timestamp) AS next_price
FROM trades
```

---

## Financial Functions

### EMA (Exponential Moving Average)

```sql
SELECT EMA(price, 20) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema20
FROM trades

-- Two EMAs (MACD components)
SELECT EMA(price, 12) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema12,
       EMA(price, 26) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema26
FROM trades
```

### DELTA / RATIO

```sql
-- Row-to-row difference
SELECT DELTA(price) OVER (PARTITION BY symbol ORDER BY timestamp) AS price_change
FROM trades

-- Row-to-row ratio (scaled int; multiply by 1e-6 for float interpretation)
SELECT RATIO(price) OVER (ORDER BY timestamp) AS price_ratio
FROM trades
```

### xbar (Time Bar Aggregation)

Buckets timestamps into fixed-size intervals. Argument is bucket size in **nanoseconds**.

```sql
-- 5-minute OHLCV candlestick
SELECT xbar(timestamp, 300000000000) AS bar,
       FIRST(price) AS open,
       MAX(price)   AS high,
       MIN(price)   AS low,
       LAST(price)  AS close,
       SUM(volume)  AS volume
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)
ORDER BY bar ASC

-- 1-hour VWAP bar
SELECT xbar(timestamp, 3600000000000) AS hour_bar,
       VWAP(price, volume) AS vwap
FROM trades GROUP BY xbar(timestamp, 3600000000000)
```

Common bar sizes:

| Period | Nanoseconds |
|--------|-------------|
| 1 second | `1_000_000_000` |
| 1 minute | `60_000_000_000` |
| 5 minutes | `300_000_000_000` |
| 1 hour | `3_600_000_000_000` |
| 1 day | `86_400_000_000_000` |

---

## Date/Time Functions

All APEX-DB timestamps are **nanoseconds since Unix epoch** (int64).

### DATE_TRUNC

Floors a nanosecond timestamp to a time unit boundary.

```sql
DATE_TRUNC('unit', column_or_expr)
```

| Unit | Bucket size (ns) |
|------|-----------------|
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

SELECT DATE_TRUNC('hour', timestamp) AS hour,
       FIRST(price) AS open, LAST(price) AS close
FROM trades
GROUP BY DATE_TRUNC('hour', timestamp)
```

### NOW()

Current nanosecond timestamp at query execution time (`std::chrono::system_clock`).

```sql
-- Last 60 seconds of trades
SELECT * FROM trades WHERE timestamp > NOW() - 60000000000

-- Age in seconds
SELECT EPOCH_S(NOW()) - EPOCH_S(timestamp) AS age_sec FROM trades
```

### EPOCH_S / EPOCH_MS

Convert nanosecond timestamp to seconds or milliseconds.

```sql
SELECT EPOCH_S(timestamp)  AS ts_sec FROM trades WHERE symbol = 1
SELECT EPOCH_MS(timestamp) AS ts_ms  FROM trades WHERE symbol = 1

-- Use in arithmetic
SELECT price, EPOCH_S(timestamp) * 1000 AS ts_ms_manual FROM trades
```

---

## JOINs

### ASOF JOIN (time-series, kdb+ style)

For each left row, finds the most recent right row where `right.timestamp <= left.timestamp`.

```sql
SELECT t.symbol, t.price, q.bid, q.ask,
       t.timestamp - q.timestamp AS staleness_ns
FROM trades t
ASOF JOIN quotes q
ON t.symbol = q.symbol AND t.timestamp >= q.timestamp
```

### Hash JOIN (equi-join)

Standard equi-join. NULL values in the join key are excluded.

```sql
SELECT t.price, t.volume, r.risk_score, r.sector
FROM trades t
JOIN risk_factors r ON t.symbol = r.symbol
```

### LEFT JOIN

Returns all left-side rows; unmatched right-side columns are NULL (INT64_MIN).

```sql
SELECT t.price, t.volume, r.risk_score
FROM trades t
LEFT JOIN risk_factors r ON t.symbol = r.symbol
WHERE r.risk_score IS NOT NULL
```

### WINDOW JOIN (wj, kdb+ style)

For each left row, aggregates right-side rows within a symmetric time window.

```sql
SELECT t.price,
       wj_avg(q.bid)   AS avg_bid,
       wj_avg(q.ask)   AS avg_ask,
       wj_count(q.bid) AS quote_count
FROM trades t
WINDOW JOIN quotes q
ON t.symbol = q.symbol
AND q.timestamp BETWEEN t.timestamp - 5000000000 AND t.timestamp + 5000000000
```

Window aggregates: `wj_avg`, `wj_sum`, `wj_min`, `wj_max`, `wj_count`

---

## Set Operations

All set operations require the same column count in both SELECT lists.

### UNION ALL

Concatenates results. Duplicates are kept.

```sql
SELECT symbol, price FROM trades WHERE symbol = 1
UNION ALL
SELECT symbol, price FROM trades WHERE symbol = 2
```

### UNION (DISTINCT)

Concatenates results and removes duplicate rows.

```sql
SELECT price FROM trades WHERE symbol = 1
UNION
SELECT price FROM trades WHERE symbol = 2
```

### INTERSECT

Returns rows present in both result sets.

```sql
SELECT price FROM trades WHERE symbol = 1
INTERSECT
SELECT price FROM trades WHERE price > 15050
```

### EXCEPT

Returns rows from the left result set that are not in the right.

```sql
SELECT price FROM trades WHERE symbol = 1
EXCEPT
SELECT price FROM trades WHERE price > 15050
```

---

## CASE WHEN

```
CASE
  WHEN condition THEN arithmetic_expr
  [WHEN condition THEN arithmetic_expr ...]
  [ELSE arithmetic_expr]
END [AS alias]
```

WHEN condition supports the same syntax as WHERE. THEN/ELSE support full arithmetic expressions.

```sql
-- Binary flag
SELECT price,
       CASE WHEN price > 15050 THEN 1 ELSE 0 END AS is_high
FROM trades WHERE symbol = 1

-- Arithmetic in THEN/ELSE
SELECT price, volume,
       CASE
           WHEN price > 15050 THEN price * 2
           WHEN price > 15020 THEN price * 1
           ELSE 0
       END AS weighted_price
FROM trades

-- Conditional aggregate
SELECT SUM(CASE WHEN price > 15050 THEN volume ELSE 0 END) AS high_volume
FROM trades WHERE symbol = 1
```

---

## Data Types & Timestamp Arithmetic

All columns in APEX-DB are `int64` at the storage level.

| Logical type | Storage | Notes |
|---|---|---|
| Integer | `int64` | Direct |
| Price (float) | `int64` | Scaled: 150.25 → 15025 at scale 100 |
| Timestamp | `int64` | Nanoseconds since Unix epoch |
| Symbol ID | `int64` | Numeric symbol identifier |
| NULL | `INT64_MIN` | Used for IS NULL checks |

### Timestamp arithmetic

```sql
-- 1 minute ago
NOW() - 60000000000

-- Last 5 minutes
WHERE timestamp > NOW() - 300000000000
```

Unit reference:

| Unit | Nanoseconds |
|------|-------------|
| 1 ns | `1` |
| 1 μs | `1_000` |
| 1 ms | `1_000_000` |
| 1 s | `1_000_000_000` |
| 1 min | `60_000_000_000` |
| 1 hour | `3_600_000_000_000` |
| 1 day | `86_400_000_000_000` |

---

*See also: [Python Reference](PYTHON_REFERENCE.md) · [C++ Reference](CPP_REFERENCE.md) · [HTTP Reference](HTTP_REFERENCE.md)*
