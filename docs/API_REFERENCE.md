# APEX-DB API Reference

*Last updated: 2026-03-22*

This is the top-level index. Each reference is in a separate file under `docs/api/`.

---

## Reference Documents

| Document | Contents |
|----------|----------|
| [docs/api/SQL_REFERENCE.md](api/SQL_REFERENCE.md) | Full SQL syntax — SELECT, WHERE, aggregates, window functions, financial functions, date/time functions, JOINs, set operations, CASE WHEN |
| [docs/api/PYTHON_REFERENCE.md](api/PYTHON_REFERENCE.md) | `apex` pybind11 binding, `apex_py` package — connection, dataframe, arrow, streaming |
| [docs/api/CPP_REFERENCE.md](api/CPP_REFERENCE.md) | `ApexPipeline`, `QueryExecutor`, `PartitionManager`, `TickMessage`, `CancellationToken` |
| [docs/api/HTTP_REFERENCE.md](api/HTTP_REFERENCE.md) | HTTP endpoints, JSON response format, authentication, Prometheus metrics, roles |

Korean translations:

| Document | Contents |
|----------|----------|
| [docs/api/SQL_REFERENCE_ko.md](api/SQL_REFERENCE_ko.md) | SQL 레퍼런스 (한국어) |
| [docs/api/PYTHON_REFERENCE_ko.md](api/PYTHON_REFERENCE_ko.md) | Python API 레퍼런스 (한국어) |
| [docs/api/CPP_REFERENCE_ko.md](api/CPP_REFERENCE_ko.md) | C++ API 레퍼런스 (한국어) |
| [docs/api/HTTP_REFERENCE_ko.md](api/HTTP_REFERENCE_ko.md) | HTTP API 레퍼런스 (한국어) |

---

## Quick Reference

### SQL — most common patterns

```sql
-- VWAP + count
SELECT vwap(price, volume), count(*) FROM trades WHERE symbol = 1

-- 5-minute OHLCV bar (kdb+ xbar style)
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low,   last(price) AS close,
       sum(volume) AS vol
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)

-- Moving average
SELECT price, AVG(price) OVER (PARTITION BY symbol ROWS 20 PRECEDING) AS ma20
FROM trades

-- Date truncation
SELECT DATE_TRUNC('min', timestamp) AS minute, sum(volume)
FROM trades GROUP BY DATE_TRUNC('min', timestamp)
```

### Python — most common patterns

```python
import apex
from apex_py import from_polars, to_pandas, ArrowSession

# Setup
pipeline = apex.Pipeline()
pipeline.start()

# Ingest from polars (zero-copy Arrow path)
from_polars(df, pipeline, symbol_col="sym", price_col="px", volume_col="vol")

# Query → pandas
result_df = to_pandas(pipeline, symbol=1)

# Zero-copy numpy
prices = pipeline.get_column(symbol=1, name="price")

# HTTP client
db = apex.connect("localhost", 8123)
df = db.query_pandas("SELECT avg(price) FROM trades GROUP BY symbol")
```

### C++ — most common patterns

```cpp
// Setup
ApexPipeline pipeline;
pipeline.start();

// Ingest
TickMessage msg{.symbol_id=1, .price=15000, .volume=100, .recv_ts=now_ns()};
pipeline.ingest_tick(msg);

// SQL
QueryExecutor exec{pipeline};
exec.enable_parallel();
auto result = exec.execute("SELECT vwap(price, volume) FROM trades WHERE symbol = 1");

// Direct query
auto r = pipeline.query_vwap(1);
```

### HTTP — most common patterns

```bash
# Query
curl -X POST http://localhost:8123/ \
  -H "Authorization: Bearer apex_<key>" \
  -d 'SELECT vwap(price, volume) FROM trades WHERE symbol = 1'

# Stats
curl http://localhost:8123/stats -H "Authorization: Bearer apex_<key>"

# Health (no auth)
curl http://localhost:8123/ping
```

---

## Interoperability Matrix

| From \ To | pandas | polars | numpy | Arrow | DuckDB | HTTP |
|-----------|--------|--------|-------|-------|--------|------|
| APEX-DB (in-proc) | `to_pandas()` | `to_polars()` | `get_column()` | `to_arrow()` | `to_duckdb()` | — |
| APEX-DB (HTTP) | `query_pandas()` | `query_polars()` | `query_numpy()` | — | — | `POST /` |
| pandas → APEX | `from_pandas()` | — | — | — | — | `ingest_pandas()` |
| polars → APEX | — | `from_polars()` | — | — | — | `ingest_polars()` |
| Arrow → APEX | — | — | — | `ingest_arrow()` | — | — |

---

## See Also

- `docs/design/layer4_transpiler_client.md` — SQL feature matrix and design decisions
- `docs/devlog/014_sql_phase2_phase3.md` — Phase 2/3 SQL implementation notes
- `docs/devlog/013_python_ecosystem.md` — Python ecosystem implementation
- `docs/design/layer5_security_auth.md` — TLS/JWT/API key setup
- `docs/deployment/PRODUCTION_DEPLOYMENT.md` — Production deployment guide
