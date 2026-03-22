# Layer 4: Client & Transpilation Layer

*Last updated: 2026-03-22 (Python ecosystem — apex_py package completed)*

This document covers the client interface layer of APEX-DB: HTTP API, Python
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
import apex

db = apex.Pipeline()
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
from apex_py.dsl import DataFrame
df = DataFrame(db, symbol=1)
result = df[df['price'] > 15000]['volume'].sum().collect()
```

**Polars vs APEX-DB comparison (100K rows):**
| | APEX | Polars | Ratio |
|---|---|---|---|
| VWAP | 56.9μs | 228.7μs | **4x** |
| COUNT | 716ns | 26.3μs | **37x** |
| get_column | 522ns | 760ns | **1.5x** |

### 1-C. C++ Direct API

```cpp
ApexPipeline pipeline;
pipeline.start();

// C++ direct — lowest latency
auto result = pipeline.query_vwap(symbol=1);  // 51μs
auto col = pipeline.partition_manager()
    .get_or_create(1, ts)
    .get_column("price");  // Direct pointer, 0 overhead
```

---

## 2. Python Ecosystem — apex_py Package ✅ (Completed 2026-03-22)

The `apex_py` package provides seamless data exchange between APEX-DB and the
scientific Python stack. It closes the Research-to-Production gap: analysts
prototype in Jupyter notebooks and ingest data into APEX-DB for production-scale
real-time queries without any serialization overhead.

### Package structure

```
apex_py/
├── __init__.py       — Public API surface
├── connection.py     — HTTP client (ApexConnection, QueryResult)
├── dataframe.py      — Vectorized ingest/export converters
├── arrow.py          — ArrowSession: zero-copy Arrow / DuckDB
├── streaming.py      — StreamingSession: high-throughput batch ingest
└── utils.py          — Dependency checker (check_dependencies, versions)
```

### Ingest paths (fastest → most flexible)

```python
import apex_py as apex

# 1. from_arrow() — Arrow buffer direct (vectorized ingest_batch)
import pyarrow as pa
tbl = pa.table({"sym": [1,2], "price": [150.0, 200.0], "volume": [100, 200]})
apex.from_arrow(tbl, pipeline)

# 2. from_polars_arrow() — Polars Arrow buffer → ingest_batch (zero-copy)
import polars as pl
df_pl = pl.DataFrame({"sym": [1], "price": [150.0], "volume": [100]})
apex.from_polars_arrow(df_pl, pipeline)

# 3. from_polars() — .to_numpy() zero-copy → ingest_batch
apex.from_polars(df_pl, pipeline, batch_size=100_000)

# 4. from_pandas() — numpy vectorized extraction → ingest_batch
import pandas as pd
df_pd = pd.DataFrame({"sym": [1], "price": [150.0], "volume": [100]})
apex.from_pandas(df_pd, pipeline, price_scale=100)  # store cents

# 5. StreamingSession — batch ingest with progress + error handling
sess = apex.StreamingSession(pipeline, batch_size=50_000, on_error="skip")
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
from apex_py import ArrowSession

sess = ArrowSession(pipeline)

tbl    = sess.to_arrow(symbol=1)                          # pa.Table
reader = sess.to_record_batch_reader(symbol=1)            # streaming
conn   = sess.to_duckdb(symbol=1, table_name="trades")    # DuckDB zero-copy
df_pl  = sess.to_polars_zero_copy(symbol=1)               # Polars via Arrow
```

### HTTP client (query → DataFrame)

```python
db = apex.connect("localhost", 8123)
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

| APEX-DB → | pandas | polars | numpy | Arrow | DuckDB |
|-----------|--------|--------|-------|-------|--------|
| HTTP query | `query_pandas()` | `query_polars()` | `query_numpy()` | — | — |
| Pipeline export | `to_pandas()` | `to_polars()` | `get_column()` | `to_arrow()` | `to_duckdb()` |
| Zero-copy | numpy view | via Arrow | direct | yes | Arrow register |

| → APEX-DB | pandas | polars | Arrow | generator |
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

### Type mapping: APEX-DB ↔ Arrow

| APEX-DB Type | Arrow Type |
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

---

## 3. SQL Support (current implementation)

```sql
-- Basic aggregation
SELECT count(*), sum(volume), avg(price), vwap(price, volume)
FROM trades WHERE symbol = 1

-- GROUP BY
SELECT symbol, sum(volume) FROM trades GROUP BY symbol

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

## 6. Roadmap

- [x] HTTP API + ClickHouse compatibility ✅
- [x] pybind11 zero-copy Python binding ✅
- [x] Migration Toolkit (kdb+/ClickHouse/DuckDB/TimescaleDB) ✅
- [x] **Python Ecosystem** (`apex_py` full package — vectorized ingest_batch) ✅
- [ ] SQL Window RANGE mode (currently ROWS only)
- [ ] Python DSL → LLVM JIT direct compilation
- [ ] Arrow Flight server (stream results as Arrow over network)
- [ ] `pip install apex-db` PyPI package

---

## 7. Streaming Data Source Connectors (Backlog)

- Kafka/Redpanda/Pulsar (librdkafka, C++ client)
- AWS Kinesis, Azure Event Hubs, Google Pub/Sub
- PostgreSQL WAL (CDC), MySQL binlog, MongoDB Change Streams
- Exchange direct: CME FAST, OPRA, CBOE PITCH, Coinbase, Bybit
