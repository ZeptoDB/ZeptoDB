# ZeptoDB Client API Compatibility Matrix

Last updated: 2026-03-24

---

## Overview

ZeptoDB provides 3 client interfaces:

| Interface | Purpose | Latency |
|-----------|---------|---------|
| **C++ API** | HFT production, feed handler | ~200ns ingest |
| **Python Binding** | Quant research, Jupyter, backtesting | ~522ns zero-copy |
| **SQL (HTTP API)** | BI tools, Grafana, operations, ad-hoc | ~5μs query |

---

## Compatibility Matrix

| Feature | C++ API | Python Binding | SQL (HTTP) |
|---------|---------|---------------|------------|
| **Ingest** | | | |
| Single ingest (~200ns) | ✅ `ingest_tick()` | ✅ `ingest()` | ✅ `INSERT INTO` |
| Batch ingest | ✅ `ingest_batch()` | ✅ `ingest_batch()` | ✅ multi-row INSERT |
| Float batch | ❌ | ✅ `ingest_float_batch()` | ❌ |
| **Query** | | | |
| VWAP/Filter/Count | ✅ Dedicated methods | ✅ Dedicated methods | ✅ SQL |
| General-purpose SQL | ✅ `QueryExecutor` | ✅ `execute()` | ✅ `POST /` |
| Zero-copy column | ✅ raw ptr | ✅ numpy view | ❌ |
| **Financial Functions** | | | |
| xbar (OHLCV) | ✅ via `execute()` | ✅ `xbar()` | ✅ |
| EMA | ✅ via `execute()` | ✅ `ema()` | ✅ |
| DELTA / RATIO | ✅ via `execute()` | ✅ `delta()` / `ratio()` | ✅ |
| Window (SUM/AVG/MIN/MAX) | ✅ via `execute()` | ✅ `window_agg()` | ✅ |
| LAG / LEAD / RANK | ✅ via `execute()` | ✅ via `execute()` | ✅ |
| **DML** | | | |
| INSERT/UPDATE/DELETE | ✅ via `execute()` | ✅ via `execute()` | ✅ |
| **DDL** | | | |
| CREATE/DROP/ALTER | ✅ via `execute()` | ✅ via `execute()` | ✅ |
| Materialized View | ✅ via `execute()` | ✅ via `execute()` | ✅ |
| Storage Policy | ✅ via `execute()` | ✅ via `execute()` | ✅ |
| **Admin** | | | |
| Statistics | ✅ `stats()` | ✅ `stats()` | ✅ `GET /stats` |
| Health check | ❌ | ✅ `is_healthy()`/`is_ready()` | ✅ `/health`, `/ready` |
| API Key management | ❌ | ❌ | ✅ `/admin/keys` |
| Query kill | ❌ | ❌ | ✅ `/admin/queries` |
| Audit log | ❌ | ❌ | ✅ `/admin/audit` |

---

## Python Usage Example

```python
import zeptodb

db = zeptodb.Pipeline()
db.start()

# ── High-performance ingest (native, ~200ns/tick) ──
db.ingest(symbol=1, price=15000, volume=100)
db.ingest_batch(symbols, prices, volumes)       # numpy int64
db.ingest_float_batch(symbols, prices, volumes,  # float → int auto conversion
                       price_scale=10000)
db.drain()

# ── Financial functions (native wrapper) ──
db.vwap(symbol=1)                                # VWAP
db.xbar(symbol=1, bucket_ns=300_000_000_000)     # 5-minute OHLCV bars
db.ema(symbol=1, period=20)                      # EMA(20)
db.delta(symbol=1)                               # Row-to-row difference
db.ratio(symbol=1)                               # Row-to-row ratio
db.window_agg(symbol=1, func='AVG', rows_preceding=20)  # Moving average

# ── General-purpose SQL (full functionality) ──
db.execute("SELECT * FROM trades WHERE symbol = 1 LIMIT 10")
db.execute("INSERT INTO trades VALUES (2, 16000, 300, 1234567890)")
db.execute("CREATE MATERIALIZED VIEW ohlcv AS ...")
db.execute("ALTER TABLE trades SET STORAGE POLICY WARM 24 HOURS")

# ── Zero-copy numpy ──
prices = db.get_column(symbol=1, name="price")   # numpy view, 522ns

# ── Status ──
db.stats()        # dict: ticks_ingested, ticks_stored, ...
db.is_healthy()   # liveness
db.is_ready()     # readiness

db.stop()
```

## C++ Usage Example

```cpp
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"

ZeptoPipeline pipeline;
pipeline.start();

// High-performance ingest
TickMessage msg{.symbol_id=1, .price=15000, .volume=100, .recv_ts=now_ns()};
pipeline.ingest_tick(msg);

// Batch ingest
int64_t prices[] = {15000, 15010, 15020};
int64_t volumes[] = {100, 200, 300};
pipeline.ingest_batch(1, prices, volumes, nullptr, 3);

// SQL execution
QueryExecutor executor(pipeline);
auto r = executor.execute("SELECT vwap(price, volume) FROM trades WHERE symbol = 1");
auto r2 = executor.execute("CREATE MATERIALIZED VIEW ohlcv AS ...");

// Native query
auto vwap = pipeline.query_vwap(1);
auto count = pipeline.query_count(1);
```

## HTTP API Usage Example

```bash
# Query
curl -X POST http://localhost:8123/ -d 'SELECT vwap(price, volume) FROM trades'

# INSERT
curl -X POST http://localhost:8123/ -d 'INSERT INTO trades VALUES (1, 15000, 100, 1234567890)'

# DDL
curl -X POST http://localhost:8123/ -d 'CREATE MATERIALIZED VIEW ohlcv AS ...'

# Admin
curl http://localhost:8123/health
curl http://localhost:8123/metrics
curl -H "Authorization: Bearer $KEY" http://localhost:8123/admin/queries
```
