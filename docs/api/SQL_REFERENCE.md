# ZeptoDB SQL Reference

*Last updated: 2026-03-24*
*31 SQL functions · 9 JOIN types · DDL/DML · s#/g#/p# indexes · Distributed query support · Statistical functions*

ZeptoDB uses a recursive descent SQL parser with nanosecond timestamp semantics.
All integer columns are `int64`. Floating-point values are stored as fixed-point scaled integers.
`NULL` is represented internally as `INT64_MIN`.

---

## Table of Contents

- [SELECT Syntax](#select-syntax)
- [CTE (WITH clause) & Subqueries](#cte-with-clause--subqueries)
- [WHERE Conditions](#where-conditions)
- [Aggregate Functions](#aggregate-functions)
- [GROUP BY / HAVING / ORDER BY / LIMIT](#group-by--having--order-by--limit)
- [Window Functions](#window-functions)
- [Financial Functions](#financial-functions)
- [Date/Time Functions](#datetime-functions)
- [String Functions](#string-functions)
- [JOINs](#joins)
- [Set Operations](#set-operations)
- [CASE WHEN](#case-when)
- [DDL (CREATE / ALTER / DROP TABLE)](#ddl-data-definition-language)
- [DML (INSERT / UPDATE / DELETE)](#dml-data-manipulation-language)
- [Index Attributes (s# / g# / p#)](#index-attributes-s--g--p)
- [EXPLAIN](#explain)
- [Catalog Queries (SHOW TABLES / DESCRIBE)](#catalog-queries-show-tables--describe)
- [Data Types & Timestamp Arithmetic](#data-types--timestamp-arithmetic)
- [Distributed Query Behavior](#distributed-query-behavior)
- [Known Limitations](#known-limitations)

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

### 8. CTE — multi-step aggregation

```sql
-- Step 1: per-minute VWAP bar
-- Step 2: rank bars by volume
WITH bars AS (
    SELECT DATE_TRUNC('min', timestamp) AS minute,
           VWAP(price, volume)           AS vwap,
           SUM(volume)                   AS vol
    FROM trades
    WHERE symbol = 1
    GROUP BY DATE_TRUNC('min', timestamp)
)
SELECT minute, vwap, vol
FROM bars
WHERE vol > 50000
ORDER BY vol DESC
LIMIT 10
```

### 9. FROM subquery — derived table

```sql
SELECT symbol, avg_price
FROM (
    SELECT symbol,
           AVG(price) AS avg_price
    FROM trades
    GROUP BY symbol
) AS summary
WHERE avg_price > 15000
```

---

## SELECT Syntax

```
[WITH cte_name AS (SELECT ...) [, cte_name2 AS (SELECT ...) ...]]
SELECT [DISTINCT] col_expr [AS alias], ...
FROM { table_name [AS alias] | (SELECT ...) AS alias }
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

## CTE (WITH clause) & Subqueries

### WITH clause (Common Table Expressions)

Named temporary result sets defined before the main SELECT. Makes complex multi-step queries readable and avoids nesting.

```
WITH name AS (SELECT ...) [, name2 AS (SELECT ...) ...]
SELECT ... FROM name
```

```sql
-- Single CTE
WITH daily AS (
    SELECT symbol,
           DATE_TRUNC('day', timestamp) AS day,
           SUM(volume)                  AS vol
    FROM trades
    GROUP BY symbol, DATE_TRUNC('day', timestamp)
)
SELECT symbol, SUM(vol) AS total_vol
FROM daily
GROUP BY symbol
ORDER BY total_vol DESC
```

```sql
-- Multiple chained CTEs (b references a)
WITH a AS (
    SELECT symbol, SUM(volume) AS total
    FROM trades
    GROUP BY symbol
),
b AS (
    SELECT symbol, total
    FROM a
    WHERE total > 1000
)
SELECT symbol, total FROM b ORDER BY total DESC
```

```sql
-- CTE + UNION ALL
WITH highs AS (
    SELECT symbol, price FROM trades WHERE price > 15050
)
SELECT symbol, price FROM highs
UNION ALL
SELECT symbol, price FROM trades WHERE symbol = 2
```

### FROM subquery (derived table)

Use a SELECT as the FROM source by wrapping it in parentheses with an alias.

```sql
SELECT symbol, avg_price
FROM (
    SELECT symbol, AVG(price) AS avg_price
    FROM trades
    GROUP BY symbol
) AS summary
WHERE avg_price > 15000
ORDER BY avg_price DESC
```

```sql
-- Aggregation over subquery
SELECT SUM(vol) AS grand_total
FROM (
    SELECT symbol, SUM(volume) AS vol
    FROM trades
    WHERE price > 15000
    GROUP BY symbol
) AS sub
```

### Supported clauses on virtual tables

All standard clauses work on CTE / subquery results:

| Clause | Supported |
|--------|-----------|
| `WHERE` | ✅ All operators (=, !=, >, <, BETWEEN, IN, IS NULL, LIKE, AND, OR, NOT) |
| `GROUP BY` | ✅ Single and multi-column |
| `HAVING` | ✅ Post-aggregation filter |
| `ORDER BY` | ✅ Single and multi-column, ASC/DESC |
| `LIMIT` | ✅ |
| `DISTINCT` | ✅ |
| `SELECT *` | ✅ Pass-through all source columns |
| Arithmetic | ✅ `price * volume AS notional` |
| Aggregates | ✅ SUM, AVG, MIN, MAX, COUNT, FIRST, LAST |

### Limitations

- No correlated subqueries (`WHERE col = (SELECT ...)`)
- No subqueries inside SELECT expressions or WHERE conditions
- VWAP, XBAR, window functions, and JOIN not yet supported on virtual tables

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

Supports multi-partition routing: `WHERE symbol IN (...)` scans only the listed partitions.

```sql
WHERE symbol IN (1, 2, 3)
WHERE price IN (15000, 15010, 15020)

-- Multi-partition aggregation
SELECT symbol, SUM(volume) FROM trades
WHERE symbol IN (1, 2, 3) GROUP BY symbol
```

### IS NULL / IS NOT NULL

ZeptoDB uses `INT64_MIN` as the NULL sentinel.

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
| `COUNT(DISTINCT col)` | Distinct value count |
| `SUM(col)` | Sum |
| `SUM(expr)` | Sum of arithmetic expression, e.g. `SUM(price * volume)` |
| `AVG(col)` | Average |
| `AVG(expr)` | Average of arithmetic expression |
| `MIN(col)` | Minimum |
| `MAX(col)` | Maximum |
| `FIRST(col)` | First value (by row order) — kdb+ `first` |
| `LAST(col)` | Last value (by row order) — kdb+ `last` |
| `VWAP(price, volume)` | Volume-weighted average price |
| `STDDEV(col)` | Population standard deviation |
| `VARIANCE(col)` | Population variance |
| `MEDIAN(col)` | Median (50th percentile) |
| `PERCENTILE(col, N)` | Nth percentile (0-100). Alias: `PERCENTILE_CONT` |

```sql
SELECT COUNT(*), COUNT(DISTINCT symbol), SUM(volume), AVG(price),
       MIN(price), MAX(price), VWAP(price, volume),
       FIRST(price) AS open, LAST(price) AS close
FROM trades WHERE symbol = 1
```

### Statistical Functions

```sql
-- Standard deviation and variance per symbol
SELECT symbol, STDDEV(price) AS sd, VARIANCE(price) AS var
FROM trades GROUP BY symbol

-- Median price
SELECT MEDIAN(price) AS median_price FROM trades WHERE symbol = 1

-- P90 latency (percentile)
SELECT PERCENTILE(price, 90) AS p90,
       PERCENTILE(price, 99) AS p99
FROM trades WHERE symbol = 1

-- PERCENTILE_CONT alias also works
SELECT PERCENTILE_CONT(price, 50) AS p50 FROM trades WHERE symbol = 1
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

### ORDER BY / LIMIT / OFFSET

```sql
SELECT symbol, SUM(volume) AS total_vol
FROM trades GROUP BY symbol
ORDER BY total_vol DESC
LIMIT 10

-- Pagination with OFFSET
SELECT * FROM trades WHERE symbol = 1
ORDER BY timestamp DESC
LIMIT 50 OFFSET 100

-- OFFSET without ORDER BY (stable row order within partitions)
SELECT * FROM trades LIMIT 50 OFFSET 200

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

All ZeptoDB timestamps are **nanoseconds since Unix epoch** (int64).

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

### INTERVAL

Duration literal that evaluates to nanoseconds. Use with `NOW()` for readable time-range queries.

```sql
INTERVAL 'N unit'
```

| Unit | Aliases |
|------|---------|
| `nanoseconds` | `ns`, `nanosecond` |
| `microseconds` | `us`, `microsecond` |
| `milliseconds` | `ms`, `millisecond` |
| `seconds` | `s`, `sec`, `second` |
| `minutes` | `m`, `min`, `minute` |
| `hours` | `h`, `hour` |
| `days` | `d`, `day` |
| `weeks` | `w`, `week` |

```sql
-- Last 5 minutes of trades
SELECT * FROM trades WHERE timestamp > NOW() - INTERVAL '5 minutes'

-- Last 2 hours
SELECT * FROM trades WHERE timestamp > NOW() - INTERVAL '2 hours'

-- In SELECT expressions
SELECT NOW() - INTERVAL '1 day' AS yesterday FROM trades LIMIT 1
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

### RIGHT JOIN

Returns all right-side rows; unmatched left-side columns are NULL.

```sql
SELECT t.price, r.risk_score
FROM trades t
RIGHT JOIN risk_factors r ON t.symbol = r.symbol
```

### FULL OUTER JOIN

Returns all rows from both sides; unmatched columns are NULL.

```sql
SELECT t.price, r.risk_score
FROM trades t
FULL OUTER JOIN risk_factors r ON t.symbol = r.symbol
```

### UNION JOIN (uj, kdb+ style)

Merges columns from both tables, concatenates all rows. Missing columns filled with NULL.

```sql
SELECT * FROM trades t UNION JOIN quotes q
```

### PLUS JOIN (pj, kdb+ style)

Additive join — matching rows have numeric columns summed.

```sql
SELECT * FROM trades t
PLUS JOIN adjustments a ON t.symbol = a.symbol
```

### AJ0 (left-columns-only ASOF JOIN)

Like ASOF JOIN but returns only the left table's columns plus matched right values.

```sql
SELECT t.price, t.volume, q.bid
FROM trades t
AJ0 JOIN quotes q ON t.symbol = q.symbol AND t.timestamp >= q.timestamp
```

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

CASE WHEN can be used standalone or nested inside aggregate functions:

```sql
-- Standalone: binary flag
SELECT price,
       CASE WHEN price > 15050 THEN 1 ELSE 0 END AS is_high
FROM trades WHERE symbol = 1

-- Inside SUM: conditional aggregation
SELECT SUM(CASE WHEN price > 15050 THEN volume ELSE 0 END) AS high_volume,
       SUM(CASE WHEN price <= 15050 THEN volume ELSE 0 END) AS low_volume
FROM trades WHERE symbol = 1

-- Inside SUM with GROUP BY: per-group conditional count
SELECT symbol,
       SUM(CASE WHEN price > 15050 THEN 1 ELSE 0 END) AS high_count
FROM trades GROUP BY symbol

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

## String Functions

### SUBSTR

Extracts a substring from the decimal string representation of an int64 column.

```sql
SUBSTR(column, start, length)
```

- `start` is 1-based (first character = 1)
- Result is converted back to int64

```sql
-- Extract first 3 digits of price
SELECT SUBSTR(price, 1, 3) AS price_prefix FROM trades WHERE symbol = 1

-- Extract last 2 digits
SELECT SUBSTR(price, 4, 2) AS price_suffix FROM trades
```

---

## Data Types & Timestamp Arithmetic

ZeptoDB supports fixed-size numeric columns and dictionary-encoded strings.

| Logical type | Storage | DDL keyword | Notes |
|---|---|---|---|
| Integer | `int64` | `INT64`, `BIGINT` | Direct |
| Integer (32-bit) | `int32` | `INT32`, `INT` | Direct |
| Float | `double` | `FLOAT64`, `DOUBLE` | Native IEEE 754 |
| Float (32-bit) | `float` | `FLOAT32`, `FLOAT` | Native IEEE 754 |
| Timestamp | `int64` | `TIMESTAMP`, `TIMESTAMP_NS` | Nanoseconds since Unix epoch |
| Symbol ID | `uint32` | `SYMBOL` | Numeric symbol identifier (legacy) |
| String | `uint32` (dict code) | `STRING`, `VARCHAR`, `TEXT` | Dictionary-encoded (LowCardinality) |
| Boolean | `uint8` | `BOOL`, `BOOLEAN` | 0 or 1 |
| NULL | `INT64_MIN` | — | Used for IS NULL checks |

### String (Dictionary-Encoded)

String columns use dictionary encoding internally — each unique string is assigned a `uint32` code.
This is optimal for low-cardinality columns (symbol, exchange, side, currency).

```sql
-- Insert with string symbol
INSERT INTO trades (symbol, price, volume) VALUES ('AAPL', 150.25, 100)

-- Query with string symbol
SELECT price FROM trades WHERE symbol = 'AAPL'
SELECT symbol, SUM(volume) FROM trades GROUP BY symbol

-- Works with all operations: aggregation, JOIN, ORDER BY, LIMIT, etc.
SELECT VWAP(price, volume) FROM trades WHERE symbol = 'GOOGL'
```

Integer symbol IDs (legacy) remain fully supported:
```sql
INSERT INTO trades VALUES (1, 15000, 100, 1711234567000000000)
SELECT price FROM trades WHERE symbol = 1
```

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

## DDL (Data Definition Language)

### CREATE TABLE

```sql
CREATE TABLE orders (
    symbol INT64,
    price INT64,
    volume INT64,
    timestamp TIMESTAMP_NS
)
```

Supported types: `INT64`, `INT32`, `FLOAT64`, `FLOAT32`, `TIMESTAMP_NS`, `SYMBOL`, `BOOL`

### DROP TABLE

```sql
DROP TABLE orders
DROP TABLE IF EXISTS orders
```

### ALTER TABLE

```sql
-- Add column
ALTER TABLE orders ADD COLUMN risk_score INT64

-- Drop column
ALTER TABLE orders DROP COLUMN risk_score

-- Set TTL (auto-evict old partitions)
ALTER TABLE trades SET TTL 30 DAYS
ALTER TABLE trades SET TTL 24 HOURS

-- Set index attribute (s#/g#/p#)
ALTER TABLE trades SET ATTRIBUTE price GROUPED    -- g# hash index
ALTER TABLE trades SET ATTRIBUTE exchange PARTED   -- p# parted index
ALTER TABLE trades SET ATTRIBUTE timestamp SORTED  -- s# sorted index
```

---

## DML (Data Manipulation Language)

### INSERT

```sql
-- Single row
INSERT INTO trades VALUES (1, 15000, 100, 1711234567000000000)

-- Multi-row
INSERT INTO trades VALUES
    (1, 15050, 200, 1711234568000000000),
    (2, 16000, 300, 1711234569000000000)

-- Column list (timestamp auto-generated)
INSERT INTO trades (symbol, price, volume) VALUES (1, 15100, 150)
```

### UPDATE

```sql
UPDATE trades SET price = 15200 WHERE symbol = 1 AND price > 15100
```

### DELETE

```sql
DELETE FROM trades WHERE symbol = 1 AND timestamp < 1711000000000000000
```

---

## Index Attributes (s# / g# / p#)

kdb+ compatible column attributes for query acceleration.

| Attribute | Type | Complexity | Best For |
|-----------|------|-----------|----------|
| `s#` (SORTED) | Binary search | O(log n) | Range queries (`BETWEEN`, `>`, `<`) on monotonic columns |
| `g#` (GROUPED) | Hash map | O(1) | Equality queries (`= X`) on high-cardinality columns |
| `p#` (PARTED) | Range map | O(1) | Equality queries on low-cardinality clustered columns |

### Setting attributes

```sql
ALTER TABLE trades SET ATTRIBUTE price GROUPED     -- g# hash index
ALTER TABLE trades SET ATTRIBUTE exchange PARTED    -- p# parted index
ALTER TABLE trades SET ATTRIBUTE timestamp SORTED   -- s# binary search
```

### Performance impact

| Query | No Index | g# Index | Speedup |
|-------|----------|----------|---------|
| `WHERE price = 15500` (1M rows) | 904μs | **3.3μs** | **274x** |

The executor automatically uses the best available index for WHERE conditions.
Index selection priority: timestamp range → s# sorted → g#/p# equality → full scan.

---

## EXPLAIN

Shows the query execution plan without running the query.

```sql
EXPLAIN SELECT count(*) FROM trades WHERE symbol = 1 AND price > 15000
```

Output includes: scan type (full/indexed/parallel), index used, estimated rows, partition count.

---

## Catalog Queries (SHOW TABLES / DESCRIBE)

### SHOW TABLES

Lists all tables registered in the schema registry.

```sql
SHOW TABLES
```

Response columns: `name` (string), `rows` (int64 — total row count across all partitions).

```json
{"columns":["name","rows"],"data":[["trades",50000],["quotes",30000]],"rows":2}
```

### DESCRIBE

Returns the column definitions for a table.

```sql
DESCRIBE trades
```

Response columns: `column` (string), `type` (string — INT64, FLOAT64, TIMESTAMP, SYMBOL, etc.).

```json
{"columns":["column","type"],"data":[["symbol","SYMBOL"],["price","INT64"],["volume","INT64"],["timestamp","TIMESTAMP"]],"rows":4}
```

---

## Distributed Query Behavior

In a multi-node cluster, the `QueryCoordinator` routes queries using a tiered strategy:

### Routing tiers

| Tier | Condition | Behavior |
|------|-----------|----------|
| **A** | `WHERE symbol = X` | Direct routing to the owning node (zero scatter overhead) |
| **A-1** | `WHERE symbol IN (1,2,3)` | Scatter to all nodes, each filters locally, merge results |
| **A-2** | ASOF/WINDOW JOIN + symbol filter | Route to symbol's node (both tables co-located) |
| **B** | No symbol filter | Scatter-gather to all nodes, merge with appropriate strategy |

### Merge strategies

| Strategy | Used when | Merge logic |
|----------|-----------|-------------|
| **CONCAT** | `GROUP BY symbol` | Each node owns its symbols → concatenate results |
| **MERGE_GROUP_BY** | `GROUP BY` non-symbol key (e.g. xbar) | Re-aggregate partial results across nodes |
| **SCALAR_AGG** | No GROUP BY, all columns are aggregates | SUM→sum, COUNT→sum, MIN→min, MAX→max, AVG→sum/count |
| **CONCAT** (default) | Non-aggregate SELECT | Concatenate all rows, apply post-merge ORDER BY/LIMIT |

### Distributed support for SQL features

| Feature | Distributed support | Notes |
|---------|-------------------|-------|
| `SUM(CASE WHEN ...)` | ✅ Full | CASE WHEN serialized to scatter SQL via `unparse_case_when` |
| `WHERE symbol IN (...)` | ✅ Full | Tier A-1: scatter + local filter + merge |
| `ORDER BY` | ✅ Full | Post-merge sort on coordinator |
| `HAVING` | ✅ Full | Stripped from scatter SQL, applied post-merge |
| `LIMIT` | ✅ Full | Applied post-merge after ORDER BY |
| `AVG` | ✅ Full | Rewritten to SUM+COUNT, reconstructed post-merge |
| `VWAP` | ✅ Full | Rewritten to SUM(price×vol)+SUM(vol), reconstructed |
| `FIRST/LAST` | ✅ Full | Fetches all data, sorts by timestamp, executes locally |
| `COUNT(DISTINCT)` | ✅ Full | Fetches all data, executes locally |
| Window functions | ✅ Full | Fetches all data, executes locally |
| CTE / Subquery | ✅ Full | Fetches all data, executes locally |
| `STDDEV/VARIANCE/MEDIAN/PERCENTILE` | ✅ Full | Fetches all data, executes locally |
| `SHOW TABLES` | ✅ Full | Scatter to all nodes, sum row counts |
| `DESCRIBE` | ✅ Full | Execute on any node (schema replicated via DDL broadcast) |
| `CREATE/DROP/ALTER TABLE` | ✅ Full | DDL broadcast to all nodes |

---

## Known Limitations

| Feature | Status |
|---------|--------|
| Correlated subqueries (`WHERE col = (SELECT ...)`) | Not planned |
| Subqueries in SELECT/WHERE expressions | Not planned |
| JOINs on CTE/subquery virtual tables | Planned |
| Window functions on virtual tables | Planned |
| Float columns (native double storage) | Planned |
| String columns | Planned |
| PERCENTILE_CONT / MEDIAN | Planned |
| STDDEV / VARIANCE | Planned |

---

*See also: [Python Reference](PYTHON_REFERENCE.md) · [C++ Reference](CPP_REFERENCE.md) · [HTTP Reference](HTTP_REFERENCE.md)*
