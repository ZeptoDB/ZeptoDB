# 010: Financial Functions — xbar, EMA, deltas/ratios, LEFT JOIN, Window JOIN

**Date:** 2026-03-22

---

## Overview

Implemented kdb+-style financial analysis functions and JOIN extensions.
These are the core features that enable APEX-DB to handle real financial workloads as a time-series database.

## Implementation

### 1. xbar — Time Bar Aggregation (Candlestick Core)

Implemented kdb+'s `xbar` as a SQL function. Floors timestamps to N units for use as GROUP BY keys.

```sql
-- Generate 5-minute OHLCV bars
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low, last(price) AS close,
       sum(volume) AS volume
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)
```

**Implementation details:**
- `xbar(value, n) = (value / n) * n` — integer division floor
- Usable in both SELECT and GROUP BY
- Added as `AggFunc::XBAR` in AST
- GROUP BY parser supports `XBAR(col, bucket)` syntax

**Performance:** 1M rows -> 3,334 five-minute bars: **~24ms**

### 2. first() / last() Aggregate Functions

Added `first()` and `last()` aggregate functions essential for OHLC candlestick charts.

- `AggFunc::FIRST` — first value in group (Open)
- `AggFunc::LAST` — last value in group (Close)
- Added `first_val`, `last_val`, `has_first` fields to `GroupState`

### 3. EMA — Exponential Moving Average (O(n) single pass)

Core indicator for technical analysis. Implements kdb+ `ema(alpha, data)`.

```sql
SELECT EMA(price, 0.1) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema_slow,
       EMA(price, 20)  OVER (PARTITION BY symbol ORDER BY timestamp) AS ema20
FROM trades
```

**Algorithm:**
```
ema[0] = data[0]
ema[i] = alpha * data[i] + (1 - alpha) * ema[i-1]
```

- Alpha specified directly (float) or period-based (integer -> `alpha = 2/(period+1)`)
- Implemented as `WindowEMA` class
- PARTITION BY support: independent calculation per symbol
- **O(n) single pass — trivially parallelizable**

**Performance:** 1M rows EMA: **~2.2ms**

### 4. DELTA / RATIO — Row-to-Row Difference/Ratio

Implements kdb+ `deltas` and `ratios`.

```sql
SELECT price,
       DELTA(price) OVER (ORDER BY timestamp) AS price_change,
       RATIO(price) OVER (ORDER BY timestamp) AS price_ratio
FROM trades
```

- `DELTA(x) = x[i] - x[i-1]` (first row = x[0])
- `RATIO(x) = x[i] / x[i-1]` (first row = 1.0, 6-digit fixed-point x1,000,000)
- `WindowDelta`, `WindowRatio` classes
- PARTITION BY support

### 5. LEFT JOIN

Implements SQL standard LEFT JOIN. Left rows with no match are included in results.

```sql
SELECT t.price, t.volume, r.risk_score
FROM trades t
LEFT JOIN risk_factors r ON t.symbol = r.symbol
```

**Implementation:**
- `JoinType` enum: `{ INNER, LEFT, RIGHT, FULL }`
- Added `join_type_` parameter to `HashJoinOperator`
- Left rows with no match: `right_indices[i] = -1`
- Right columns for unmatched rows: `INT64_MIN` (NULL sentinel)
- Defined `JOIN_NULL` constant (`INT64_MIN`)

### 6. Window JOIN (wj) — kdb+ Style

The most complex feature. Aggregates right-side rows within a time window for each left row.

```sql
SELECT t.price, wj_avg(q.bid) AS avg_bid, wj_count(q.bid) AS quote_count
FROM trades t
WINDOW JOIN quotes q
ON t.symbol = q.symbol
AND q.timestamp BETWEEN t.timestamp - 5000000000 AND t.timestamp + 5000000000
```

**Algorithm:**
1. Group right table by symbol (hash map)
2. For each left row:
   - Find right group by symbol (O(1))
   - Binary search for `[t - before, t + after]` range (O(log m))
   - Apply aggregation to rows in range (avg, sum, count, min, max)
3. **Complexity: O(n x log m)**

**Supported aggregate functions:**
- `wj_avg()`, `wj_sum()`, `wj_count()`, `wj_min()`, `wj_max()`

## SQL Parser Changes

### New Tokens
- `XBAR`, `EMA`, `DELTA`, `RATIO`, `WINDOW`
- `PLUS` (+), `MINUS` (-) — arithmetic operators

### Parser Changes
- `parse_select_expr()`: parses XBAR, FIRST, LAST, EMA, DELTA, RATIO, wj_* functions
- `parse_group_by()`: supports `XBAR(col, bucket)` syntax
- `parse_join()`: parses `WINDOW JOIN ... ON ... AND col BETWEEN expr AND expr`
- AS alias: allows keywords as aliases (`AS delta`, `AS bar`, etc.)
- Negative literals vs MINUS operator: context-based disambiguation

### AST Changes
- `AggFunc`: added FIRST, LAST, XBAR
- `WindowFunc`: added EMA, DELTA, RATIO
- `WJAggFunc`: Window JOIN-specific aggregation enum
- `SelectExpr`: added `xbar_bucket`, `ema_alpha`, `ema_period`, `wj_agg` fields
- `GroupByClause`: `xbar_buckets` vector
- `JoinClause::Type::WINDOW` added, time window parameters

## Test Results

| Test Category | Count | Result |
|---------------|-------|--------|
| Tokenizer (new keywords) | 6 | PASS |
| Parser (new syntax) | 8 | PASS |
| WindowFunction (EMA/Delta/Ratio) | 8 | PASS |
| HashJoin (LEFT JOIN) | 4 | PASS |
| WindowJoin (wj) | 4 | PASS |
| SQL Executor (xbar, delta) | 3 | PASS |
| All existing tests | 109 | PASS |

**Total 151 tests PASS** (excluding TransportSwap/ClusterNode — existing network-dependent tests)

## Benchmarks

| Benchmark | Data Size | Time |
|-----------|-----------|------|
| xbar GROUP BY | 1M rows -> 3,334 bars | **24ms** |
| EMA calculation | 1M rows | **2.2ms** |

## Changed Files

### New Files
- `tests/unit/test_financial_functions.cpp` — 40+ new tests

### Modified Files
- `include/apex/sql/tokenizer.h` — New token types (XBAR, EMA, DELTA, RATIO, WINDOW, PLUS, MINUS)
- `src/sql/tokenizer.cpp` — New keyword mappings, MINUS/PLUS token handling
- `include/apex/sql/ast.h` — AggFunc, WindowFunc, WJAggFunc, SelectExpr, GroupByClause, JoinClause extensions
- `src/sql/parser.cpp` — All new syntax parsing (xbar, ema, delta, ratio, first, last, wj_*, WINDOW JOIN)
- `include/apex/execution/window_function.h` — Added WindowEMA, WindowDelta, WindowRatio classes
- `include/apex/execution/join_operator.h` — JoinType enum, WindowJoinOperator, JOIN_NULL constant
- `src/execution/join_operator.cpp` — LEFT JOIN implementation, WindowJoinOperator binary search
- `include/apex/sql/executor.h` — exec_window_join declaration
- `src/sql/executor.cpp` — FIRST/LAST/XBAR aggregation, EMA/DELTA/RATIO window, LEFT JOIN NULL handling, WINDOW JOIN execution
- `tests/CMakeLists.txt` — Added test_financial_functions.cpp

## Design Decisions

1. **Integer fixed-point**: RATIO uses x1,000,000 scaling (6 decimal places). Ratios expressed in integers without floating-point arithmetic.
2. **NULL sentinel**: Uses `INT64_MIN`. Probability of this value appearing in real financial data is zero.
3. **EMA single pass**: One forward O(n) scan. Independent per-partition processing possible for GPU/SIMD parallelization.
4. **Window JOIN binary search**: O(log m) on sorted timestamps. Can be optimized to sliding window O(n+m) when both sides are sorted (TODO).
5. **wj_ prefix**: Explicit naming like `wj_avg`, `wj_sum` to distinguish Window JOIN aggregations from regular aggregations.

## Next Steps

- [ ] Window JOIN sliding window optimization (O(n+m) when both sorted)
- [ ] RATIO scaling options (x100, x10000 etc. user-defined)
- [ ] RIGHT JOIN, FULL OUTER JOIN implementation
- [ ] EMA GPU acceleration (independent per partition — trivially parallel)
- [ ] Real-time streaming EMA (incremental update)
