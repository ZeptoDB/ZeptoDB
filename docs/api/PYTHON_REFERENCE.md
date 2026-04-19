# ZeptoDB Python API Reference

*Last updated: 2026-04-18*

Two Python interfaces are available:

| Interface | Description | Use case |
|-----------|-------------|----------|
| `zeptodb` | Low-level pybind11 C++ binding | In-process, maximum performance |
| `zepto_py` | High-level Python package | Pandas/Polars/Arrow interop, HTTP client |

---

## Table of Contents

- [zeptodb — pybind11 binding](#apex--pybind11-binding)
  - [zeptodb.Pipeline](#zeptopipeline)
  - [zeptodb.sql.QueryExecutor](#zeptosqlqueryexecutor)
- [zepto_py.connection — HTTP client](#zepto_pyconnection--http-client)
- [zepto_py.dataframe — bulk ingest/export](#zepto_pydataframe--bulk-ingestexport)
- [zepto_py.arrow — Arrow / DuckDB interop](#zepto_pyarrow--arrow--duckdb-interop)
- [zepto_py.streaming — high-throughput ingest](#zepto_pystreaming--high-throughput-ingest)
- [Interoperability Matrix](#interoperability-matrix)

---

## Quick Start

### End-to-end: ingest from polars, query via SQL, export to pandas

```python
import zeptodb
import polars as pl
from zepto_py import from_polars, query_to_pandas

# 1. Start pipeline
pipeline = zeptodb.Pipeline()
pipeline.start()

# 2. Build a polars DataFrame and ingest (zero-copy Arrow path)
df = pl.DataFrame({
    "symbol": [1, 1, 1, 2, 2],
    "price":  [15000, 15010, 15020, 20000, 20010],
    "volume": [100, 150, 200, 80, 90],
})
from_polars(df, pipeline,
            symbol_col="symbol", price_col="price", volume_col="volume")
pipeline.drain()

# 3. Run SQL and get a pandas DataFrame
result = query_to_pandas(
    pipeline,
    "SELECT symbol, vwap(price, volume) AS vwap, sum(volume) AS vol "
    "FROM trades GROUP BY symbol ORDER BY symbol"
)
print(result)
#    symbol        vwap  vol
# 0       1  15012.222  450
# 1       2  20004.706  170

# 4. Zero-copy numpy column access
import numpy as np
prices = pipeline.get_column(symbol=1, name="price")   # np.ndarray[int64], ~522ns
print(prices)  # [15000, 15010, 15020]

pipeline.stop()
```

### HTTP client quick start

```python
import zepto_py as apex

# Connect to running zepto_server
db = zeptodb.connect("localhost", 8123)

# SQL → pandas
df = db.query_pandas(
    "SELECT xbar(timestamp, 300000000000) AS bar, "
    "first(price) AS open, last(price) AS close, sum(volume) AS vol "
    "FROM trades WHERE symbol = 1 "
    "GROUP BY xbar(timestamp, 300000000000) ORDER BY bar"
)
print(df)
```

### High-throughput ingest with StreamingSession

```python
import pandas as pd
from zepto_py import StreamingSession

sess = StreamingSession(pipeline, batch_size=50_000, error_mode="skip")

# Ingest a large DataFrame with progress display
big_df = pd.DataFrame({
    "symbol": [1] * 1_000_000,
    "price":  range(15000, 16000000, 15),
    "volume": [100] * 1_000_000,
})
sess.ingest_pandas(big_df, show_progress=True,
                   symbol_col="symbol", price_col="price", volume_col="volume")
# Ingested 1,000,000 rows in 1.82s (549,451 rows/sec)

stats = sess.stats()
print(f"Ingested {stats.rows_ingested:,} rows at {stats.throughput:,.0f} rows/sec")
```

### Arrow / DuckDB interop

```python
import pyarrow as pa
from zepto_py import ArrowSession

sess = ArrowSession(pipeline)

# Export to Arrow (zero-copy)
table = sess.to_arrow(symbol=1)
print(table.schema)  # timestamp: int64, price: int64, volume: int64

# Query with DuckDB directly on ZeptoDB data
conn = sess.to_duckdb(symbol=1, table_name="trades")
df = conn.execute(
    "SELECT avg(price), stddev(price) FROM trades"
).fetchdf()
print(df)
```

---

## zeptodb — pybind11 binding

The `zeptodb` module is the low-level C++ binding built with pybind11.
Build with `cmake -DAPEX_BUILD_PYTHON=ON`.

### zeptodb.Pipeline

#### Construction

```python
import zeptodb

# Default config (pure in-memory, 32 MB arena per partition)
pipeline = zeptodb.Pipeline()

# Custom config
pipeline = zeptodb.Pipeline(config=zeptodb.PipelineConfig(
    arena_size=32 * 1024 * 1024,
    drain_batch_size=256,
    storage_mode=zeptodb.StorageMode.PURE_IN_MEMORY,
    # storage_mode=zeptodb.StorageMode.TIERED,
    # storage_mode=zeptodb.StorageMode.PURE_ON_DISK,
))
```

#### Lifecycle

```python
pipeline.start()   # start background drain thread
pipeline.stop()    # flush queue + stop drain thread
pipeline.drain()   # synchronous drain — useful in tests without background thread
```

#### Ingest

```python
# Single tick
pipeline.ingest(symbol=1, price=15000, volume=100)
pipeline.ingest(symbol=1, price=15010, volume=50, timestamp=1711000000000000000)

# Batch ingest — vectorized, single C++ call, no Python loop
import numpy as np
syms   = np.array([1, 1, 1], dtype=np.int64)
prices = np.array([15000, 15010, 15020], dtype=np.int64)
vols   = np.array([100, 50, 75], dtype=np.int64)
pipeline.ingest_batch(syms, prices, vols)

# Float batch ingest — auto scale float64 → int64 in C++
prices_f = np.array([150.00, 150.10, 150.20], dtype=np.float64)
vols_f   = np.array([100.0, 50.0, 75.0], dtype=np.float64)
pipeline.ingest_float_batch(syms, prices_f, vols_f, price_scale=100.0)

# Table-aware ingest (Stage B, devlog 084) — lands rows in a specific
# CREATE TABLE table so that SELECT FROM other_table sees 0 rows.
pipeline.execute("CREATE TABLE trades (symbol INT64, price INT64, volume INT64, timestamp TIMESTAMP_NS)")
pipeline.ingest_batch(syms, prices, vols, table_name="trades")
# Unknown table raises ValueError:
#   pipeline.ingest_batch(syms, prices, vols, table_name="nope")  # ValueError
```

#### Direct queries (C++ execution)

```python
result = pipeline.vwap(symbol=1)                   # → float
result = pipeline.vwap(symbol=1, from_ns=t0, to_ns=t1)
result = pipeline.count(symbol=1)                  # → int
result = pipeline.sum(symbol=1, col="volume")      # → int
```

#### Zero-copy column export

Returns a numpy view into ZeptoDB's internal column buffer — no copy, ~522ns.

```python
prices     = pipeline.get_column(symbol=1, name="price")      # np.ndarray[int64]
volumes    = pipeline.get_column(symbol=1, name="volume")     # np.ndarray[int64]
timestamps = pipeline.get_column(symbol=1, name="timestamp")  # np.ndarray[int64]

assert prices.base is not None        # it's a view
assert not prices.flags['OWNDATA']    # zero-copy confirmed
```

#### Statistics

```python
stats = pipeline.stats()
stats.ticks_ingested     # int — total ticks received
stats.ticks_stored       # int — written to column store
stats.ticks_dropped      # int — dropped (queue overflow)
stats.queries_executed   # int
stats.total_rows_scanned # int
stats.partitions_created # int
stats.last_ingest_latency_ns  # int
```

---

### zeptodb.sql.QueryExecutor

```python
from zeptodb.sql import QueryExecutor

executor = QueryExecutor(pipeline)

# Parallel execution
executor.enable_parallel()                          # auto thread count
executor.enable_parallel(num_threads=8, row_threshold=100_000)
executor.disable_parallel()

# Execute SQL
result = executor.execute("SELECT vwap(price, volume) FROM trades WHERE symbol = 1")

# Result fields
result.column_names       # list[str]
result.rows               # list[list[int]]  — all values as int64
result.execution_time_us  # float
result.rows_scanned       # int
result.error              # str — empty if ok
result.ok()               # bool

# Iterate results
for row in result.rows:
    sym_id = row[result.column_names.index("symbol")]
    price  = row[result.column_names.index("price")]
```

---

## zepto_py.connection — HTTP client

Connects to a running `zepto_server` on port 8123 (ClickHouse-compatible HTTP API).

```python
import zepto_py as apex
from zepto_py import ApexConnection

# Connect
db = zeptodb.connect("localhost", 8123)
# or equivalently:
db = ApexConnection(host="localhost", port=8123)
db = ApexConnection(host="localhost", port=8123,
                    api_key="zepto_<64-hex>")  # with auth

# Health check
db.ping()   # → True if server is up

# SQL → pandas DataFrame
df = db.query_pandas("SELECT symbol, avg(price) FROM trades GROUP BY symbol")
# Returns: pd.DataFrame

# SQL → polars DataFrame
df = db.query_polars("SELECT symbol, avg(price) FROM trades GROUP BY symbol")
# Returns: pl.DataFrame

# SQL → numpy dict
arrays = db.query_numpy("SELECT price, volume FROM trades WHERE symbol = 1")
# Returns: dict[str, np.ndarray]

# Ingest pandas → HTTP
db.ingest_pandas(df,
                 symbol_col="symbol",
                 price_col="price",
                 volume_col="volume")

# Ingest pandas into a specific table (devlog 088)
db.ingest_pandas(df, table_name="my_trades")   # INSERT INTO my_trades ...

# Ingest polars → HTTP
db.ingest_polars(df_polars)
db.ingest_polars(df_polars, table_name="my_trades")

# DDL convenience helpers (devlog 088) — wrappers over execute() / query()
db.create_table(
    "my_trades",
    [("symbol", "INT64"), ("price", "INT64"),
     ("volume", "INT64"), ("timestamp", "INT64")],
    if_not_exists=True,
)
db.list_tables()                          # → ['my_trades', ...]
db.drop_table("my_trades", if_exists=True)
```

### ZeptoConnection DDL helpers (devlog 088)

| Method | Signature | Purpose |
|---|---|---|
| `create_table` | `create_table(name, columns, if_not_exists=False)` | `columns` is a `list[tuple[str, str]]` of `(col_name, sql_type)`; issues `CREATE TABLE [IF NOT EXISTS] name (...)` |
| `drop_table`   | `drop_table(name, if_exists=False)` | Issues `DROP TABLE [IF EXISTS] name` |
| `list_tables`  | `list_tables() -> list[str]`         | Runs `SHOW TABLES` and returns the first column of every row |
| `ingest_pandas`| `ingest_pandas(df, ..., table_name="ticks")` | `table_name` kwarg (default `"ticks"` for backward compat) selects the destination table for the generated `INSERT` statements |
| `ingest_polars`| `ingest_polars(df, ..., table_name="ticks")` | Passes `table_name` through to `ingest_pandas` |

**Identifier validation (devlog 089).** All caller-supplied identifiers
are validated before interpolation into SQL — invalid names raise
`ValueError` and never hit the wire.

| Arg | Regex | Methods that validate it |
|---|---|---|
| table / column `name` | `^[A-Za-z_][A-Za-z0-9_]*$` | `create_table`, `drop_table`, `ingest_pandas`, `ingest_polars` |
| column `type` string  | `^[A-Za-z0-9_]+$`          | `create_table` |

```python
db.create_table("ticks; DROP TABLE x; --", [("a", "INT64")])
# ValueError: Invalid SQL identifier: "ticks; DROP TABLE x; --"

db.create_table("safe", [("col; DROP", "INT64")])
# ValueError: Invalid SQL identifier: "col; DROP"
```

`ingest_pandas` also SQL-escapes single quotes inside string values
(standard `''` doubling), so `DataFrame` rows containing `it's` are
inserted correctly.

---

## zepto_py.dataframe — bulk ingest/export

Standalone converters. Requires a live `zeptodb.Pipeline` (C++ binding) object — no HTTP.

```python
from zepto_py import (
    from_pandas, from_polars, from_arrow,
    to_pandas, to_polars,
    query_to_pandas, query_to_polars,
)
```

### Ingest

#### from_pandas

Vectorized — uses `df[col].to_numpy(copy=False)` then single `ingest_batch()` call. No Python row loop.

```python
from_pandas(
    df,                          # pd.DataFrame
    pipeline,                    # zeptodb.Pipeline
    symbol_col="symbol",         # column name for symbol id
    price_col="price",           # column name for price
    volume_col="volume",         # column name for volume
    timestamp_col="timestamp",   # optional; omit to use now_ns()
    price_scale=1.0,             # float → int64: value * price_scale
)
```

#### from_polars

Zero-copy via Arrow buffer — `Series.to_numpy()` returns Arrow buffer directly for numeric columns without nulls.

```python
from_polars(
    df,                          # pl.DataFrame
    pipeline,
    symbol_col="symbol",
    price_col="price",
    volume_col="volume",
    price_scale=1.0,
)
```

#### from_arrow

```python
from_arrow(
    table,                       # pa.Table
    pipeline,
    symbol_col="symbol",
    price_col="price",
    volume_col="volume",
)
```

### Export

```python
# All columns for a symbol → pandas
df = to_pandas(pipeline, symbol=1)
# Returns: pd.DataFrame with columns: timestamp, price, volume, ...

# All columns for a symbol → polars
df = to_polars(pipeline, symbol=1)

# SQL → pandas
df = query_to_pandas(pipeline,
    "SELECT avg(price), sum(volume) FROM trades WHERE symbol = 1")

# SQL → polars
df = query_to_polars(pipeline,
    "SELECT symbol, sum(volume) FROM trades GROUP BY symbol")
```

### Performance (1M rows)

| Method | Throughput |
|--------|-----------|
| `from_polars()` | ~3.3M rows/sec |
| `from_pandas()` | ~2M rows/sec |
| `from_arrow()` | ~3M rows/sec |

---

## zepto_py.arrow — Arrow / DuckDB interop

Zero-copy Arrow table exchange and DuckDB registration.

```python
from zepto_py import ArrowSession
import pyarrow as pa

sess = ArrowSession(pipeline)
```

### Ingest

```python
# Arrow Table → ZeptoDB
sess.ingest_arrow(
    table,                       # pa.Table
    symbol_col="symbol",
    price_col="price",
    volume_col="volume",
)

# Arrow RecordBatch → ZeptoDB
sess.ingest_record_batch(batch)  # pa.RecordBatch

# Per-column Arrow arrays → ZeptoDB (raw columnar)
sess.ingest_arrow_columnar(
    symbols=sym_array,           # pa.Array[int64]
    prices=px_array,             # pa.Array[int64]
    volumes=vol_array,           # pa.Array[int64]
)
```

### Export

```python
# ZeptoDB → Arrow Table (zero-copy)
table = sess.to_arrow(symbol=1)
# Returns: pa.Table with schema: timestamp: int64, price: int64, volume: int64

# ZeptoDB → Arrow RecordBatchReader (streaming/lazy)
reader = sess.to_record_batch_reader(symbol=1)
for batch in reader:
    print(batch.num_rows)

# ZeptoDB → polars (via Arrow buffer — true zero-copy)
df = sess.to_polars_zero_copy(symbol=1)
# Returns: pl.DataFrame sharing Arrow buffer — no copy

# ZeptoDB → DuckDB in-memory table
conn = sess.to_duckdb(symbol=1, table_name="trades")
result = conn.execute("SELECT avg(price) FROM trades").fetchdf()
```

### Utilities

```python
# Schema
arrow_schema = sess.get_schema()   # → pa.Schema

from zepto_py.arrow import zeptodb_schema_to_arrow
schema = zepto_schema_to_arrow(["timestamp", "price", "volume"])
```

---

## zepto_py.streaming — high-throughput ingest

Batch ingest from pandas/polars/generators with progress and error handling.

```python
from zepto_py import StreamingSession

sess = StreamingSession(
    pipeline,
    batch_size=50_000,           # rows per C++ ingest_batch() call
    error_mode="skip",           # see error modes below
)
```

### ingest_pandas / ingest_polars

```python
sess.ingest_pandas(
    df,
    show_progress=True,          # print throughput stats at end
    symbol_col="symbol",
    price_col="price",
    volume_col="volume",
)
# → Ingested 1,000,000 rows in 1.82s (549,451 rows/sec)

sess.ingest_polars(
    df_polars,
    show_progress=True,
)
```

### ingest_iter (generator / iterator)

```python
def tick_generator():
    for row in external_feed:
        yield {"symbol": row.sym, "price": row.px, "volume": row.vol}

sess.ingest_iter(
    tick_generator(),
    show_progress=True,
    total=1_000_000,             # optional: total for progress display
)
```

### Statistics

```python
stats = sess.stats()
stats.rows_ingested    # int
stats.rows_skipped     # int
stats.batches          # int
stats.elapsed_sec      # float
stats.throughput       # float  (rows/sec)
```

### Error modes

| Mode | Behavior |
|------|----------|
| `"skip"` | Skip bad rows, continue ingesting |
| `"raise"` | Raise exception on first error |
| `"warn"` | Print warning to stderr, continue |

---

## Interoperability Matrix

| From \ To | pandas | polars | numpy | Arrow | DuckDB | HTTP |
|-----------|--------|--------|-------|-------|--------|------|
| **ZeptoDB (in-proc)** | `to_pandas()` | `to_polars()` | `get_column()` | `to_arrow()` | `to_duckdb()` | — |
| **ZeptoDB (HTTP)** | `query_pandas()` | `query_polars()` | `query_numpy()` | — | — | `POST /` |
| **pandas → APEX** | `from_pandas()` | — | — | — | — | `ingest_pandas()` |
| **polars → APEX** | — | `from_polars()` | — | — | — | `ingest_polars()` |
| **Arrow → APEX** | — | — | — | `ingest_arrow()` | — | — |

---

*See also: [SQL Reference](SQL_REFERENCE.md) · [C++ Reference](CPP_REFERENCE.md) · [HTTP Reference](HTTP_REFERENCE.md)*
