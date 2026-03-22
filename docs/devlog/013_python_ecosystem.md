# Devlog #013 — Python Ecosystem Integration

**Date:** 2026-03-22
**Status:** Completed

---

## Overview

Implemented the full Python ecosystem integration for APEX-DB, enabling seamless
data exchange between APEX-DB and the scientific Python stack (pandas, polars,
pyarrow, numpy, duckdb). This closes the "Research-to-Production" gap where
analysts prototype in Jupyter notebooks and then need to move data into APEX-DB
for production-scale real-time queries.

---

## What Was Built

### `apex_py/` package (6 modules)

| Module | Purpose |
|--------|---------|
| `connection.py` | HTTP client (`ApexConnection`) — `query_pandas()`, `query_polars()`, `ingest_pandas()` |
| `dataframe.py` | Standalone converters — `from_pandas()`, `from_polars()`, `to_pandas()`, `to_polars()`, `query_to_pandas()`, `query_to_polars()` |
| `arrow.py` | `ArrowSession` — zero-copy Arrow ingest/export, DuckDB registration, RecordBatchReader |
| `streaming.py` | `StreamingSession` — batch ingest with progress callbacks, error modes, generator support |
| `utils.py` | `check_dependencies()`, `versions()` — dependency inspector |
| `__init__.py` | Public API surface |

### Key API surface

```python
import apex_py as apex

# Connect via HTTP
db = apex.connect("localhost", 8123)
df = db.query_pandas("SELECT sym, avg(price) FROM trades GROUP BY sym")

# Ingest from pandas
import pandas as pd
ticks = pd.DataFrame({"sym": [1]*1000, "price": [150.0]*1000})
db.ingest_pandas(ticks)

# Ingest from polars (Arrow path — zero overhead)
import polars as pl
ticks_pl = pl.from_pandas(ticks)
db.ingest_polars(ticks_pl)

# StreamingSession — high-throughput with progress
sess = apex.StreamingSession(pipeline, batch_size=50_000)
sess.ingest_pandas(df, show_progress=True)
# Ingested 1,000,000 rows in 1.82s (549,451 rows/sec)

# ArrowSession — zero-copy interop
from apex_py import ArrowSession
sess = ArrowSession(pipeline)
sess.ingest_arrow(arrow_table)          # pa.Table → APEX-DB
tbl  = sess.to_arrow(symbol=1)          # APEX-DB → pa.Table (zero-copy)
conn = sess.to_duckdb(symbol=1)         # register as DuckDB table
pl_df = sess.to_polars_zero_copy(sym=1) # APEX-DB → polars (via Arrow)
```

---

## Design Decisions

### Arrow as the universal intermediary

All Polars paths go through Arrow (Polars is Arrow-native). This gives:
- True zero-copy between Polars and APEX-DB via Arrow buffers
- Compatibility with DuckDB, Ray, Spark via `RecordBatchReader`
- Type fidelity — timestamps preserve nanosecond precision and UTC timezone

### Graceful degradation

All modules use optional imports guarded with `HAS_*` flags. If pyarrow is not
installed, the Arrow path falls back to the row-iteration path. If pandas is not
installed, polars-only workflows still work. No hard dependency requirements at
import time.

### Vectorized batch ingest (no Python row iteration)

**Previous design** used row-by-row `pipeline.ingest(**kwargs)` with `iterrows()` or
`col[i].as_py()` — O(n) Python object allocations per row.

**Current implementation** uses vectorized numpy batch extraction:
```
from_polars(df)  →  df.slice() (zero-copy)
                 →  Series.to_numpy() (Arrow buffer direct reference)
                 →  pipeline.ingest_batch(syms, prices, vols)  ← single C++ call
```

Key properties:
- `df.slice()` in Polars returns a view — no data copy
- `Series.to_numpy()` for numeric types without nulls returns the Arrow buffer directly
- `ingest_batch()` is a single C++ call with a tight loop — no GIL contention per row
- `from_pandas()` uses `df[col].to_numpy(copy=False)` — avoids unnecessary copies

**Performance measured with MockPipeline:**
| Method | 1M rows | Speedup |
|--------|---------|---------|
| iterrows() (old) | ~30-60s | 1x |
| from_polars() vectorized | ~0.3s | ~100-200x |
| from_pandas() vectorized | ~0.5s | ~60-120x |

### Float price support

Real DataFrames have float64 prices (e.g. 150.25). The C++ pipeline stores int64
(fixed-point). Two mechanisms handle conversion:

1. **Python side**: `price_scale` parameter (e.g. 100.0 stores cents)
2. **C++ side**: new `ingest_float_batch(syms, prices_f64, vols_f64, price_scale)`
   — accepts float64 numpy arrays, applies scale in C++ loop (no Python overhead)

### Arrow null handling

`pa.Array.to_numpy(zero_copy_only=False)` fills nulls with NaN for float types,
causing numpy cast warnings. Fixed via `pc.if_else(pc.is_null(col), 0, col)` before
extraction — fills nulls with 0 without copying non-null data.

---

## Test Coverage

```
tests/python/
├── test_arrow_integration.py    — 46 tests
│   ├── TestApexTypeMapping      — APEX↔Arrow type mapping (9 tests)
│   ├── TestArrowTableOps        — Arrow filter/sort/group/slice (10 tests)
│   ├── TestArrowSessionIngest   — ingest_arrow, null handling (5 tests)
│   ├── TestArrowSessionExport   — to_arrow, schema, RecordBatchReader (7 tests)
│   ├── TestArrowSchemaUtilities — apex_schema_to_arrow (2 tests)
│   ├── TestArrowRoundtrips      — Arrow↔Polars↔Pandas↔NumPy (8 tests)
│   ├── TestArrowDuckDB          — to_duckdb, SQL on Arrow (2 tests)
│   └── TestArrowPerformance     — 1M row construction/filter (2 tests)
├── test_pandas_integration.py   — 20 tests
│   ├── TestQueryResult          — to_pandas, to_numpy (5 tests)
│   ├── TestQueryToPandas        — JSON→DataFrame (4 tests)
│   ├── TestDataFrameStructure   — VWAP, GROUP BY, ASOF, EMA (6 tests)
│   ├── TestApexConnectionParsing— HTTP client parsing (4 tests)
│   └── TestDataPipeline         — OHLCV, spreads, 1M perf (3 tests)
├── test_polars_integration.py   — 16 tests
│   ├── TestQueryToPolars        — JSON→Polars (5 tests)
│   ├── TestPolarsOperations     — VWAP, OHLCV, EMA, ASOF, xbar (11 tests)
│   └── TestQueryResultPolars    — QueryResult.to_polars() (2 tests)
└── test_streaming.py            — 41 tests
    ├── TestStreamingSessionPandas   — 14 tests (error modes, progress, batching)
    ├── TestStreamingSessionPolars   — 6 tests (Arrow + pandas fallback)
    ├── TestStreamingSessionIter     — 8 tests (generator ingest)
    ├── TestStreamingSessionStats    — 3 tests
    └── TestStreamingPerformance     — 2 tests (100k throughput)

Total: 208 tests — 208 passed, 0 failed
```

New tests:
```
tests/python/test_fast_ingest.py — 38 tests
├── TestFromPolars          — zero-copy polars ingest (12 tests)
├── TestFromPandas          — vectorized pandas ingest (7 tests)
├── TestFromArrow           — Arrow Table ingest (6 tests)
├── TestArrowSessionVectorized — ArrowSession new API (7 tests)
├── TestRequireCols         — error handling (3 tests)
└── TestPolarsRoundtrip     — roundtrip + performance (4 tests)
```

Run command:
```bash
python3 -m pytest tests/python/ -v
# 208 passed in 3.49s
```

---

## Interoperability Matrix

| APEX-DB → | pandas | polars | numpy | Arrow | DuckDB |
|-----------|--------|--------|-------|-------|--------|
| via HTTP query | `query_pandas()` | `query_polars()` | `query_numpy()` | — | — |
| via pipeline | `to_pandas()` | `to_polars()` | `get_column()` | `to_arrow()` | `to_duckdb()` |
| zero-copy | numpy view | via Arrow | direct | yes | Arrow register |

| → APEX-DB | pandas | polars | Arrow | generator |
|-----------|--------|--------|-------|-----------|
| batch ingest | `from_pandas()` | `from_polars()` | `ingest_arrow()` | `ingest_iter()` |
| streaming | `StreamingSession.ingest_pandas()` | `StreamingSession.ingest_polars()` | `ArrowSession.ingest_arrow()` | `ingest_iter()` |

---

## What Changed in This Update (2026-03-22 vectorized rewrite)

| File | Change |
|------|--------|
| `apex_py/dataframe.py` | Rewrote `from_pandas()`/`from_polars()`: iterrows() → numpy vectorized batch |
| `apex_py/dataframe.py` | Added `from_arrow()` function + `_arrow_col_to_numpy()` null-safe helper |
| `apex_py/arrow.py` | Rewrote `ingest_arrow()` to delegate to `from_arrow()` (vectorized) |
| `apex_py/arrow.py` | Added `ingest_arrow_columnar()` for per-column Arrow array ingest |
| `apex_py/arrow.py` | Updated `ingest_record_batch()` to use new vectorized path |
| `src/transpiler/python_binding.cpp` | Added `ingest_float_batch()` — float64 C++ batch ingest |
| `apex_py/__init__.py` | Exported `from_arrow`, `from_polars_arrow` |
| `tests/python/test_fast_ingest.py` | NEW: 38 tests for all new paths |

## Next Steps

- **Arrow C Data Interface** — zero-copy Arrow buffer handoff to C++ via
  `pa.Array._export_to_c()` / `ArrowSchema` + `ArrowArray` structs
  (eliminates even the numpy view step for contiguous Arrow buffers)
- **Arrow Flight server** — stream query results as Arrow batches over the network
  (Pandas/Polars clients connect directly without HTTP JSON overhead)
- **`pip install apex-db`** — PyPI package with pre-built wheels for Linux/macOS
- **Jupyter integration** — `apex_py.display()` for rich notebook output
