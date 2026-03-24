# ZeptoDB Phase 007: SQL Parser + HTTP API + JOIN Framework

**Date:** 2026-03-22
**Phase:** 007 — SQL/HTTP/JOIN

---

## Overview

After completing the core layers (E, B, A, D, C), we added the interface that actual clients can use. This phase implements:

1. **SQL Parser** — recursive descent parser (no yacc/bison)
2. **Query Executor** — AST -> ZeptoPipeline API translation
3. **JOIN Framework** — ASOF JOIN (finance core) + HashJoin stub
4. **HTTP API Server** — ClickHouse-compatible port 8123

---

## Part 1: SQL Parser

### Design Principles
- **Fully self-implemented**: No flex/bison/ANTLR dependencies. Pure C++ recursive descent parser
- **Practical subset**: Only SQL actually used in financial databases
- **Tokenizer -> AST -> Executor** pipeline

### Supported SQL Syntax

```sql
-- Basic SELECT + WHERE
SELECT price, volume FROM trades WHERE symbol = 1 AND price > 15000

-- Aggregate functions (COUNT, SUM, AVG, MIN, MAX, VWAP)
SELECT count(*), sum(volume), avg(price) FROM trades WHERE symbol = 1
SELECT VWAP(price, volume) FROM trades  -- finance-specific

-- GROUP BY
SELECT symbol, sum(volume) FROM trades GROUP BY symbol

-- ASOF JOIN (finance core)
SELECT t.price, t.volume, q.bid, q.ask
FROM trades t
ASOF JOIN quotes q ON t.symbol = q.symbol AND t.timestamp >= q.timestamp

-- Time range
SELECT * FROM trades WHERE price BETWEEN 15000 AND 16000

-- Standard JOIN (stub)
SELECT t.price, q.bid FROM trades t JOIN quotes q ON t.symbol = q.symbol

-- Sort + limit
SELECT price FROM trades ORDER BY price DESC LIMIT 100
```

### Parsing Performance

| Query Type | Average | P99 |
|-----------|---------|-----|
| Simple SELECT | 1.17us | 1.23us |
| Aggregate query | 1.79us | 1.85us |
| GROUP BY | 1.39us | 1.43us |
| ASOF JOIN | 4.56us | 4.63us |
| Complex JOIN | 5.06us | 5.14us |

All queries under 50us target achieved.

---

## Part 2: Query Executor

### APEX Schema Specifics

ZeptoDB partitions have no `symbol` column. `SymbolId` is encoded in `PartitionKey.symbol_id`.
Therefore `WHERE symbol = N` conditions are handled as **partition-level filtering, not column-level evaluation**.

```cpp
// Detect symbol condition -> use get_partitions_for_symbol()
if (has_where_symbol(stmt, sym_filter, alias)) {
    auto parts = pm.get_partitions_for_symbol(sym_filter);
    // Scan only these partitions
}
```

### GROUP BY symbol Handling

For GROUP BY symbol, `symbol_id` is read directly from the partition key:

```cpp
bool is_symbol_group = (group_col == "symbol");
int64_t gkey = is_symbol_group
    ? static_cast<int64_t>(part->key().symbol_id)
    : gdata[idx];
```

### VWAP Aggregation

SQL supports `VWAP(price, volume)` as an aggregate function:

```sql
SELECT VWAP(price, volume) FROM trades WHERE symbol = 1
```

Internally computes `sum(price * volume) / sum(volume)`.

---

## Part 3: JOIN Framework

### Generic Interface

```cpp
class JoinOperator {
public:
    virtual JoinResult execute(
        const ColumnVector& left_key,
        const ColumnVector& right_key,
        const ColumnVector* left_time  = nullptr,  // for ASOF
        const ColumnVector* right_time = nullptr
    ) = 0;
};
```

### ASOF JOIN Algorithm

Implemented with **two-pointer + binary search** combination:
1. Group right table by symbol
2. Sort by timestamp within each group (safety guard)
3. For each left row, use `upper_bound` for O(log m) matching

```
Left: trades [t=100, t=200, t=300]  (symbol=1)
Right: quotes [t=50, t=150, t=250] (symbol=1)

ASOF matching:
  trade(t=100) -> quote(t=50)   <- most recent where t <= 100
  trade(t=200) -> quote(t=150)  <- most recent where t <= 200
  trade(t=300) -> quote(t=250)  <- most recent where t <= 300
```

### HashJoin Stub

```cpp
class HashJoinOperator : public JoinOperator {
public:
    JoinResult execute(...) override {
        throw std::runtime_error("HashJoinOperator: not yet implemented");
    }
};
```

Full hash table build + probe implementation planned for a future phase.

---

## Part 4: HTTP API

### ClickHouse-Compatible Server

- **Port**: 8123 (ClickHouse default port)
- **Library**: cpp-httplib v0.18.3 (header-only)
- **Direct Grafana/client connectivity**

### Endpoints

```
POST /        — SQL query execution (body: SQL string)
GET  /        — SQL query (query parameter)
GET  /ping    — health check -> "Ok"
GET  /stats   — pipeline statistics (JSON)
```

### Response Format

```json
{
    "columns": ["price", "volume"],
    "data": [[15000, 100], [15010, 200]],
    "rows": 2,
    "rows_scanned": 100000,
    "execution_time_us": 52.30
}
```

---

## Part 5: Test Results

```
=== New Tests (32) ===
Tokenizer.*       7/7  PASS
Parser.*         12/12 PASS
AsofJoin.*        4/4  PASS
SqlExecutorTest.* 9/9  PASS

=== Existing Tests Maintained ===
C++ unit tests: 76/76 PASS
Python tests:   31/31 PASS
```

---

## Part 6: Benchmark Results

### SQL Parsing Speed

| Query | avg | Target |
|-------|-----|--------|
| Simple SELECT | 1.17us | <50us OK |
| Aggregate | 1.79us | <50us OK |
| ASOF JOIN parse | 4.56us | <50us OK |

### SQL Execution Overhead (vs direct C++ API)

| Operation | SQL | Direct C++ |
|-----------|-----|-----------|
| VWAP (100K rows) | 112us | 50us |
| COUNT | 13us | 0.12us |
| SUM (100K rows) | 52us | N/A |

SQL overhead = parsing (~2us) + AST interpretation + function pointer dispatch

### ASOF JOIN Performance

| Data Size | Processing Time |
|-----------|----------------|
| N=1,000 | 149us |
| N=10,000 | 1.5ms |
| N=1,000,000 | 53ms |

---

## Architecture Layer Update

```
Layer 6: HTTP API (zepto_server)
    |
Layer 5: SQL Parser + Executor (zepto_sql)
    |
Layer 4: Transpiler (Python DSL, zepto_py)
    |
Layer 3: Vectorized Engine (zepto_execution)
    |
Layer 2: Ingestion (zepto_ingestion)
    |
Layer 1: Storage (zepto_storage)
```

---

## Next Phase

- **HashJoin full implementation** — hash table build + probe (SIMD optimized)
- **Query planner enhancement** — JOIN reordering, pushdown optimization
- **HTTP API extension** — TSV output mode, async queries
- **ASOF JOIN SIMD optimization** — vectorize binary search
