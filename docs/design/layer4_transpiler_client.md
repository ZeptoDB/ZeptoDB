# Layer 4: Client & Transpilation Layer

*Last updated: 2026-03-22 (SQL Phase 2/3 — arithmetic, CASE WHEN, multi-GROUP BY, date/time functions, LIKE, UNION/INTERSECT/EXCEPT)*

This document covers the client interface layer of ZeptoDB: HTTP API, Python
DSL/ecosystem, C++ direct API, SQL support, and the migration toolkit.

---

## 1. Implemented Interfaces (3 types)

### 1-A. HTTP API (port 8123, ClickHouse-compatible)

```
POST /          SQL query execution → JSON response
GET  /ping      Health check (ClickHouse-compatible)
GET  /health    Kubernetes liveness probe
GET  /ready     Kubernetes readiness probe
GET  /stats     Pipeline statistics
GET  /metrics   Prometheus OpenMetrics
```

```bash
curl -X POST http://localhost:8123/ \
  -d 'SELECT vwap(price, volume) FROM trades WHERE symbol = 1'
# → {"columns":["vwap"],"data":[[15037.2]],"rows":1,"execution_time_us":52.3}
```

**Implementation:** `cpp-httplib` header-only, lightweight, Grafana ClickHouse plugin compatible.

### 1-B. Python DSL (pybind11 + Lazy Evaluation)

**Original design:** nanobind → **Actual implementation:** pybind11 (stability first)

```python
import zeptodb

db = zeptodb.Pipeline()
db.start()
db.ingest(symbol=1, price=15000, volume=100)
db.drain()

# Direct calls
result = db.vwap(symbol=1)          # C++ direct, 50μs
result = db.count(symbol=1)         # 0.12μs

# Zero-copy numpy (core feature)
prices  = db.get_column(symbol=1, name="price")   # numpy, 522ns, no copy
volumes = db.get_column(symbol=1, name="volume")

# Lazy DSL (Polars style)
from zepto_py.dsl import DataFrame
df = DataFrame(db, symbol=1)
result = df[df['price'] > 15000]['volume'].sum().collect()
```

**Polars vs ZeptoDB comparison (100K rows):**
| | APEX | Polars | Ratio |
|---|---|---|---|
| VWAP | 56.9μs | 228.7μs | **4x** |
| COUNT | 716ns | 26.3μs | **37x** |
| get_column | 522ns | 760ns | **1.5x** |

### 1-C. C++ Direct API

```cpp
ZeptoPipeline pipeline;
pipeline.start();

// C++ direct — lowest latency
auto result = pipeline.query_vwap(symbol=1);  // 51μs
auto col = pipeline.partition_manager()
    .get_or_create(1, ts)
    .get_column("price");  // Direct pointer, 0 overhead
```

---

## 2. Python Ecosystem — zepto_py Package ✅ (Completed 2026-03-22)

The `zepto_py` package provides seamless data exchange between ZeptoDB and the
scientific Python stack. It closes the Research-to-Production gap: analysts
prototype in Jupyter notebooks and ingest data into ZeptoDB for production-scale
real-time queries without any serialization overhead.

### Package structure

```
zepto_py/
├── __init__.py       — Public API surface
├── connection.py     — HTTP client (ApexConnection, QueryResult)
├── dataframe.py      — Vectorized ingest/export converters
├── arrow.py          — ArrowSession: zero-copy Arrow / DuckDB
├── streaming.py      — StreamingSession: high-throughput batch ingest
└── utils.py          — Dependency checker (check_dependencies, versions)
```

### Ingest paths (fastest → most flexible)

```python
import zepto_py as apex

# 1. from_arrow() — Arrow buffer direct (vectorized ingest_batch)
import pyarrow as pa
tbl = pa.table({"sym": [1,2], "price": [150.0, 200.0], "volume": [100, 200]})
zeptodb.from_arrow(tbl, pipeline)

# 2. from_polars_arrow() — Polars Arrow buffer → ingest_batch (zero-copy)
import polars as pl
df_pl = pl.DataFrame({"sym": [1], "price": [150.0], "volume": [100]})
zeptodb.from_polars_arrow(df_pl, pipeline)

# 3. from_polars() — .to_numpy() zero-copy → ingest_batch
zeptodb.from_polars(df_pl, pipeline, batch_size=100_000)

# 4. from_pandas() — numpy vectorized extraction → ingest_batch
import pandas as pd
df_pd = pd.DataFrame({"sym": [1], "price": [150.0], "volume": [100]})
zeptodb.from_pandas(df_pd, pipeline, price_scale=100)  # store cents

# 5. StreamingSession — batch ingest with progress + error handling
sess = zeptodb.StreamingSession(pipeline, batch_size=50_000, on_error="skip")
sess.ingest_pandas(df_pd, show_progress=True)
sess.ingest_polars(df_pl, use_arrow=True)
sess.ingest_iter(tick_generator())   # memory-efficient generator
```

All ingest functions are **vectorized** — no Python-level row iteration:
1. Extract columns as numpy arrays (`series.to_numpy()` / `batch.to_numpy()`)
2. Apply `price_scale` if float→int64 conversion needed
3. Call `pipeline.ingest_batch(symbols, prices, volumes)` once per chunk

### Export paths (zero-copy)

```python
from zepto_py import ArrowSession

sess = ArrowSession(pipeline)

tbl    = sess.to_arrow(symbol=1)                          # pa.Table
reader = sess.to_record_batch_reader(symbol=1)            # streaming
conn   = sess.to_duckdb(symbol=1, table_name="trades")    # DuckDB zero-copy
df_pl  = sess.to_polars_zero_copy(symbol=1)               # Polars via Arrow
```

### HTTP client (query → DataFrame)

```python
db = zeptodb.connect("localhost", 8123)
df  = db.query_pandas("SELECT sym, avg(price) FROM trades GROUP BY sym")
df  = db.query_polars("SELECT * FROM trades WHERE sym=1 LIMIT 1000")
arr = db.query_numpy("SELECT price FROM trades WHERE sym=1")
db.ingest_pandas(trades_df)
db.ingest_polars(trades_pl)
```

### Arrow column-level ingest (maximum zero-copy)

```python
sess = ArrowSession(pipeline)
sess.ingest_arrow_columnar(
    sym_arr   = pa.array([1, 1, 2], type=pa.int64()),
    price_arr = pa.array([15000, 15001, 16000], type=pa.int64()),
    vol_arr   = pa.array([100, 200, 150], type=pa.int64()),
)
```

### Interoperability matrix

| ZeptoDB → | pandas | polars | numpy | Arrow | DuckDB |
|-----------|--------|--------|-------|-------|--------|
| HTTP query | `query_pandas()` | `query_polars()` | `query_numpy()` | — | — |
| Pipeline export | `to_pandas()` | `to_polars()` | `get_column()` | `to_arrow()` | `to_duckdb()` |
| Zero-copy | numpy view | via Arrow | direct | yes | Arrow register |

| → ZeptoDB | pandas | polars | Arrow | generator |
|-----------|--------|--------|-------|-----------|
| Vectorized | `from_pandas()` | `from_polars()` | `from_arrow()` | `ingest_iter()` |
| Streaming | `StreamingSession` | `StreamingSession` | `ArrowSession` | `ingest_iter()` |

### Arrow null handling

`pa.Array.to_numpy(zero_copy_only=False)` fills nulls with NaN for float types,
causing numpy cast warnings. Fixed via helper functions in `dataframe.py`:

```python
def _arrow_col_to_numpy(col: pa.Array) -> np.ndarray:
    # pc.if_else(pc.is_null(col), 0, col) before extraction — fills nulls with 0
    ...

def _arrow_col_to_int64(col: pa.Array) -> np.ndarray:
    # _arrow_col_to_numpy + astype(int64, copy=False)
    ...
```

### Type mapping: ZeptoDB ↔ Arrow

| ZeptoDB Type | Arrow Type |
|---|---|
| BOOLEAN | pa.bool_() |
| TINYINT / SMALLINT / INTEGER / BIGINT | pa.int8/16/32/64() |
| REAL / DOUBLE | pa.float32/64() |
| VARCHAR | pa.large_utf8() |
| TIMESTAMP | pa.timestamp("ns", tz="UTC") |
| DATE | pa.date32() |

### Test coverage (208 tests, all passing)

```
tests/python/
├── test_ingest_batch.py       47  — from_pandas/polars/arrow, _require_cols
├── test_arrow_integration.py  46  — ArrowSession, type mapping, DuckDB, roundtrips
├── test_pandas_integration.py 20  — query_to_pandas, VWAP, OHLCV, connection
├── test_polars_integration.py 16  — query_to_polars, VWAP, ASOF, window
└── test_streaming.py          41  — StreamingSession: pandas/polars/iter/stats/perf
```

### Python Packaging — `pip install zeptodb` ✅ (2026-03-23)

**Distribution name:** `zeptodb` · **Import name:** `zepto_py` · **Version:** 0.1.0

```
# Build from source
pip install build twine
python -m build          # produces dist/zeptodb-0.1.0-py3-none-any.whl
                         #         and dist/zeptodb-0.1.0.tar.gz
twine check dist/*       # → PASSED

# Install locally
pip install dist/zeptodb-0.1.0-py3-none-any.whl

# Install with optional extras
pip install "zeptodb[all]"      # numpy, pandas, polars, pyarrow, duckdb
pip install "zeptodb[pandas]"   # numpy + pandas only
pip install "zeptodb[polars]"   # polars only
pip install "zeptodb[duckdb]"   # duckdb only
```

**pyproject.toml key settings:**

| Field | Value |
|-------|-------|
| `build-backend` | `setuptools.build_meta` (PEP 517/518) |
| `name` | `zeptodb` |
| `license` | `Apache-2.0` (SPDX string) |
| `requires-python` | `>=3.9` |
| `dependencies` | `[]` — zero mandatory deps |

The wheel is a **pure-Python** `py3-none-any` wheel. When the C++ extension (`zepto_core.so`) is available (built separately via CMake), it is imported automatically at runtime — the Python wheel ships only the client/integration layer.

To publish to PyPI: `twine upload dist/*` (requires a PyPI account and `~/.pypirc` token).

---

## 3. SQL Support (current implementation)

### 3-A. Core SELECT / Aggregation / JOIN

```sql
-- Basic aggregation
SELECT count(*), sum(volume), avg(price), vwap(price, volume)
FROM trades WHERE symbol = 1

-- GROUP BY (single and multi-column)
SELECT symbol, sum(volume) FROM trades GROUP BY symbol
SELECT symbol, price, SUM(volume) FROM trades GROUP BY symbol, price

-- DISTINCT
SELECT DISTINCT symbol FROM trades

-- HAVING (post-aggregation filter)
SELECT symbol, SUM(volume) AS total_vol
FROM trades GROUP BY symbol HAVING total_vol > 1000

-- IN / IS NULL / NOT
SELECT * FROM trades WHERE symbol IN (1, 2, 3)
SELECT * FROM trades WHERE risk_score IS NOT NULL
SELECT * FROM trades WHERE NOT price > 15100

-- ASOF JOIN (time-series key operation)
SELECT t.price, q.bid, q.ask
FROM trades t
ASOF JOIN quotes q ON t.symbol = q.symbol AND t.timestamp >= q.timestamp

-- Hash JOIN / LEFT JOIN
SELECT t.price, r.risk_score
FROM trades t JOIN risk_factors r ON t.symbol = r.symbol

-- Window functions
SELECT symbol, price,
       AVG(price) OVER (PARTITION BY symbol ROWS 20 PRECEDING) AS ma20,
       LAG(price, 1) OVER (PARTITION BY symbol) AS prev_price,
       RANK() OVER (ORDER BY price DESC) AS rank
FROM trades

-- Financial functions (kdb+ compatible)
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low,   last(price) AS close,
       sum(volume) AS volume
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)

-- EMA, DELTA, RATIO
SELECT EMA(price, 0.1) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema,
       DELTA(price) OVER (ORDER BY timestamp) AS change
FROM trades

-- Window JOIN (wj)
SELECT t.price, wj_avg(q.bid) AS avg_bid
FROM trades t
WINDOW JOIN quotes q ON t.symbol = q.symbol
AND q.timestamp BETWEEN t.timestamp - 5000000000 AND t.timestamp + 5000000000
```

### 3-B. Phase 2: SELECT Arithmetic + CASE WHEN + Multi-Column GROUP BY (2026-03-22)

```sql
-- SELECT arithmetic: full expression trees in column list
SELECT symbol,
       price * volume        AS notional,
       (price - 15000) / 100 AS premium,
       SUM(price * volume)   AS total_notional,
       AVG(price - 15000)    AS avg_premium
FROM trades WHERE symbol = 1

-- CASE WHEN: conditional column expressions
SELECT symbol, price,
       CASE WHEN price > 15050 THEN 1 ELSE 0 END            AS is_high,
       CASE WHEN volume > 105 THEN price * 2 ELSE price END  AS adj_price
FROM trades WHERE symbol = 1

-- Multi-column GROUP BY: composite VectorHash key
SELECT symbol, price, SUM(volume) AS vol
FROM trades GROUP BY symbol, price
ORDER BY price ASC

-- Arithmetic in GROUP BY key (xbar + arithmetic combined)
SELECT xbar(timestamp, 60000000000) AS min_bar,
       SUM(price * volume) AS total_notional
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 60000000000)
```

**Implementation notes:**
- `ArithExpr` tree: `Kind::COLUMN | LITERAL | BINARY | FUNC` nodes
- Binary operators: `ADD (+)`, `SUB (-)`, `MUL (*)`, `DIV (/)`
- `eval_arith(node, part, row_idx)` recursive evaluator in executor
- Arithmetic inside aggregates: `agg_val` lambda wraps `eval_arith` or direct column read
- Both serial (`exec_agg`, `exec_group_agg`) and parallel paths support all Phase 2 features

### 3-C. Phase 3: Date/Time Functions + LIKE + Set Operations (2026-03-22)

```sql
-- DATE_TRUNC: floor timestamp to time unit
-- Units: 'ns', 'us', 'ms', 's', 'min', 'hour', 'day', 'week'
SELECT DATE_TRUNC('min', timestamp) AS minute, SUM(volume) AS vol
FROM trades WHERE symbol = 1
GROUP BY DATE_TRUNC('min', timestamp)

SELECT DATE_TRUNC('hour', timestamp) AS hour,
       first(price) AS open, last(price) AS close
FROM trades GROUP BY DATE_TRUNC('hour', timestamp)

-- NOW(): current timestamp (nanoseconds, system_clock)
SELECT * FROM trades WHERE timestamp > NOW() - 60000000000

-- EPOCH_S / EPOCH_MS: convert nanosecond timestamp to seconds/milliseconds
SELECT EPOCH_S(timestamp) AS ts_sec,   price FROM trades WHERE symbol = 1
SELECT EPOCH_MS(timestamp) AS ts_ms,   price FROM trades WHERE symbol = 1

-- LIKE / NOT LIKE: glob pattern matching
-- '%' = any substring, '_' = any single character
SELECT symbol, price FROM trades WHERE price LIKE '150%'
SELECT symbol, price FROM trades WHERE price NOT LIKE '%9'

-- UNION ALL: concatenate result sets (duplicates kept)
SELECT price FROM trades WHERE symbol = 1
UNION ALL
SELECT price FROM trades WHERE symbol = 2

-- UNION DISTINCT: concatenate + deduplicate
SELECT price FROM trades WHERE symbol = 1
UNION
SELECT price FROM trades WHERE symbol = 2

-- INTERSECT: rows present in both
SELECT price FROM trades WHERE symbol = 1
INTERSECT
SELECT price FROM trades WHERE price > 15050

-- EXCEPT: rows in left not in right
SELECT price FROM trades WHERE symbol = 1
EXCEPT
SELECT price FROM trades WHERE price > 15050
```

**Implementation notes:**
- `ArithExpr::Kind::FUNC`: `func_name` (`date_trunc`/`now`/`epoch_s`/`epoch_ms`), `func_unit`, `func_arg`
- `date_trunc_bucket(unit_str)` maps unit strings to nanosecond bucket sizes
- `NOW()` evaluates via `std::chrono::system_clock::now()` at query time
- `Expr::Kind::LIKE`: DP grid glob match, int64 values converted via `std::to_string()`
- Set operations handled at the top of `exec_select()` before normal dispatch; `UNION DISTINCT` uses `std::set<std::vector<int64_t>>`

### 3-D. SQL Feature Matrix

| Feature | Status | Notes |
|---------|--------|-------|
| SELECT * / col list | ✅ | |
| SELECT arithmetic (`a * b`) | ✅ Phase 2 | Full expression tree |
| CASE WHEN | ✅ Phase 2 | THEN/ELSE are arith exprs |
| DISTINCT | ✅ | |
| FROM / table alias | ✅ | |
| WHERE (compare/BETWEEN/AND/OR/NOT) | ✅ | |
| WHERE IN (v1, v2, ...) | ✅ Phase 1 | |
| WHERE IS NULL / IS NOT NULL | ✅ Phase 1 | INT64_MIN sentinel |
| WHERE LIKE / NOT LIKE | ✅ Phase 3 | DP glob, `%` and `_` |
| INNER / HASH JOIN | ✅ | |
| ASOF JOIN | ✅ | |
| LEFT JOIN | ✅ | NULL = INT64_MIN |
| WINDOW JOIN (wj) | ✅ | O(n log m) |
| GROUP BY (single col) | ✅ | |
| GROUP BY xbar(col, bucket) | ✅ | kdb+ style |
| GROUP BY (multi-column) | ✅ Phase 2 | VectorHash composite key |
| HAVING | ✅ Phase 1 | Post-aggregation filter |
| ORDER BY | ✅ | ASC/DESC, multi-col |
| LIMIT | ✅ | |
| Aggregates (COUNT/SUM/AVG/MIN/MAX) | ✅ | |
| VWAP / FIRST / LAST | ✅ | Financial |
| Window functions (SUM/AVG/.../LAG/LEAD) | ✅ | OVER clause |
| EMA / DELTA / RATIO | ✅ | Financial window |
| ROW_NUMBER / RANK / DENSE_RANK | ✅ | |
| DATE_TRUNC | ✅ Phase 3 | ns/us/ms/s/min/hour/day/week |
| NOW() | ✅ Phase 3 | Nanosecond precision |
| EPOCH_S / EPOCH_MS | ✅ Phase 3 | ns → s / ms |
| UNION ALL / DISTINCT | ✅ Phase 3 | |
| INTERSECT / EXCEPT | ✅ Phase 3 | |
| Subquery / CTE | ❌ Planned | |
| RIGHT JOIN / FULL OUTER | ❌ Planned | |
| EXPLAIN | ❌ Planned | |
| SUBSTR / string functions | ❌ Planned | |
| NULL standardization | ❌ Planned | INT64_MIN → actual NULL |

---

## 4. Design Decisions: Original vs Actual

| Item | Original Design | Actual Implementation | Reason |
|---|---|---|---|
| Python binding | nanobind | **pybind11** | Build stability |
| AST serialization | FlatBuffers | **Direct C++ call** | Complexity reduction |
| DSL → JIT | Python AST → LLVM | **Lazy Eval → C++ API** | Incremental build |
| Client protocol | Custom | **HTTP (ClickHouse-compatible)** | Ecosystem |
| Python ingest | Row-by-row | **Vectorized ingest_batch()** | 100–1000x throughput |
| Null handling | Per-row skip | **fill_null(0) before numpy cast** | Vectorized path safety |

---

## 5. Parallel Query (QueryScheduler DI)

**Current:** `LocalQueryScheduler` — scatter/gather, 3.48x @ 8 threads

```cpp
auto scheduler = std::make_unique<LocalQueryScheduler>(8);
QueryExecutor executor(pipeline, std::move(scheduler));
auto result = executor.execute(ast);
// 0.248ms vs 0.862ms serial
```

**Future:** `DistributedQueryScheduler` (UCX-based) — no API change for multi-node.

---

## 6. Connection Hooks (`.z.po` / `.z.pc` equivalent)

**Status:** ✅ Implemented (2026-03-23)

kdb+ has `.z.po` (port open) and `.z.pc` (port close) callbacks for session lifecycle management. ZeptoDB provides an equivalent through `HttpServer`.

### API

```cpp
HttpServer server(executor, 8123);

// .z.po equivalent — fires on first request from a new remote address
server.set_on_connect([](const zeptodb::server::ConnectionInfo& info) {
    printf("Connect: %s at %lld\n", info.remote_addr.c_str(), info.connected_at_ns);
});

// .z.pc equivalent — fires on Connection:close or eviction
server.set_on_disconnect([](const zeptodb::server::ConnectionInfo& info) {
    printf("Disconnect: %s queries=%llu\n",
           info.remote_addr.c_str(), info.query_count);
});

// Session management
auto sessions = server.list_sessions();                // snapshot
size_t n = server.evict_idle_sessions(30 * 60 * 1000); // evict idle > 30min
```

### `ConnectionInfo` Structure

```cpp
struct ConnectionInfo {
    std::string remote_addr;     // IP:port
    std::string user;            // auth subject or remote_addr
    int64_t     connected_at_ns; // epoch-ns, first request
    int64_t     last_active_ns;  // epoch-ns, most recent request
    uint64_t    query_count;     // requests in this session
};
```

### REST Endpoint

```
GET /admin/sessions    →  [{remote_addr, user, connected_at_ns, last_active_ns, query_count}, ...]
```
Requires admin permission.

### Implementation Notes

- httplib `set_logger` fires after every HTTP response — used to track session state
- `Connection: close` request header triggers `on_disconnect` (HTTP/1.1 semantics)
- Session key: `remote_addr` (IP:port); `evict_idle_sessions` fires `on_disconnect` for each removed session
- Files: `include/zeptodb/server/http_server.h`, `src/server/http_server.cpp`

---

## 7. Interactive Query Timer (`\t <sql>`)

**Status:** ✅ Implemented (2026-03-23)

kdb+ supports `\t expr` to time a single expression without toggling the global timer. zepto-cli now supports the same one-shot syntax.

### Usage

```
zepto> \t SELECT sum(volume) FROM trades WHERE symbol = 1
+-------------+
| sum(volume) |
+-------------+
| 1045        |
+-------------+
1 row in set
Time: 0.42 ms

zepto> \t                ← toggle ON/OFF (existing, unchanged)
Timing ON
```

- `\t <sql>` — runs one query with timing; global toggle state is not changed
- `\t` alone — toggles timing ON/OFF
- File: `tools/zepto-cli.cpp`, `BuiltinCommands::handle()`

---

## 8. Roadmap

- [x] HTTP API + ClickHouse compatibility ✅
- [x] pybind11 zero-copy Python binding ✅
- [x] Migration Toolkit (kdb+/ClickHouse/DuckDB/TimescaleDB) ✅
- [x] **Python Ecosystem** (`zepto_py` full package — vectorized ingest_batch) ✅
- [x] **SQL Phase 1** — IN, IS NULL, NOT, HAVING ✅
- [x] **SQL Phase 2** — SELECT arithmetic, CASE WHEN, multi-column GROUP BY ✅
- [x] **SQL Phase 3** — DATE_TRUNC/NOW/EPOCH_S/EPOCH_MS, LIKE/NOT LIKE, UNION/INTERSECT/EXCEPT ✅
- [x] **SQL Subquery / CTE** — WITH clause, FROM (subquery) ✅
- [x] **Connection hooks** — `.z.po/.z.pc` equivalent, session tracking ✅ (2026-03-23)
- [x] **`\t <sql>` one-shot timer** — interactive query timing ✅ (2026-03-23)
- [x] **`pip install zeptodb` Python wheel** — PEP 517/518 packaging, `twine check` PASSED ✅ (2026-03-23)
- [ ] SQL Window RANGE mode (currently ROWS only)
- [ ] Python DSL → LLVM JIT direct compilation
- [ ] Arrow Flight server (stream results as Arrow over network)
- [ ] PyPI publish (requires PyPI account + token)
- [ ] Cost-based planner — see §9 Impact Assessment (major engine impact)
- [ ] Composite index — see §9 Impact Assessment (major engine impact)
- [ ] MV query rewrite — see §9 Impact Assessment (major engine impact)
- [ ] Prepared statements — see §9 Impact Assessment (moderate engine impact)
- [ ] Query result cache — see §9 Impact Assessment (indirect engine impact)
- [ ] SAMPLE clause — see §9 Impact Assessment (positive engine impact)
- [ ] INTERVAL syntax — see §9 Impact Assessment (cosmetic, no engine impact)

---

## 9. SQL Feature — Engine Performance Impact Assessment

> Evaluation of planned SQL/query features by their direct impact on engine execution performance.

| Feature | Engine Perf Impact? | Explanation |
|---------|---------------------|-------------|
| INTERVAL syntax | No | Pure parser/AST sugar — translates to the same nanosecond literal before reaching the execution engine. Zero runtime cost. |
| Query result cache | Yes (indirect) | Doesn't make the engine itself faster, but avoids hitting the engine entirely on cache hits. Huge win for repeated dashboard queries. |
| MV query rewrite | Yes (major) | Rewrites a full-scan query into a pre-aggregated MV lookup. Eliminates execution work entirely when a matching MV exists. |
| Cost-based planner | Yes (major) | Chooses better join order, index usage, and scan strategies. Every query benefits once the planner makes smarter decisions. Single highest-leverage item for long-term engine throughput. |
| Prepared statements | Yes (moderate) | Skips tokenize → parse → plan on repeated executions. For high-QPS workloads the saved CPU is significant; for ad-hoc queries it's negligible. |
| JOINs/Window on virtual tables | Yes (moderate) | Enables the engine to push joins and window functions into virtual-table scan operators instead of materializing first. Performance depends on how much work can be pushed down. |
| Scalar subqueries in WHERE | Mixed | Enables new query patterns, but a naive implementation (re-execute per row) can hurt performance. Needs decorrelation to be a net positive. |
| Composite index | Yes (major) | Turns multi-column filter scans from O(n) to O(log n). Directly reduces the amount of data the engine touches for the most common query shapes. |
| SAMPLE clause | Yes (positive) | Reads only a fraction of partitions/rows. Directly reduces I/O and compute for exploratory queries. Doesn't affect non-SAMPLE queries. |

### Summary

6 of 9 items directly improve engine performance:
- **Highest leverage:** Cost-based planner + MV query rewrite + Composite index
- **Cosmetic only:** INTERVAL syntax (zero runtime cost)
- **Bypass, not speedup:** Query result cache (avoids engine, doesn't speed it up)
- **Double-edged:** Scalar subqueries require decorrelation to avoid per-row re-execution regression

### Recommended priority for engine speed

1. **Cost-based planner** — highest effort but every query benefits (major)
2. **Composite index** — O(n) → O(log n) for multi-column filters (major)
3. **MV query rewrite** — eliminates execution entirely for matching queries (major)
4. **Prepared statements** — significant for high-QPS workloads (moderate)
5. **JOINs/Window on virtual tables** — pushdown reduces materialization (moderate)
6. **SAMPLE clause** — reduces I/O for exploratory queries (positive)
7. **Query result cache** — bypasses engine on cache hits (indirect)
8. **Scalar subqueries** — implement only with decorrelation (mixed)
9. **INTERVAL syntax** — cosmetic, implement whenever convenient (none)

---

## 10. Streaming Data Source Connectors (Backlog)

- Kafka/Redpanda/Pulsar (librdkafka, C++ client)
- AWS Kinesis, Azure Event Hubs, Google Pub/Sub
- PostgreSQL WAL (CDC), MySQL binlog, MongoDB Change Streams
- Exchange direct: CME FAST, OPRA, CBOE PITCH, Coinbase, Bybit
