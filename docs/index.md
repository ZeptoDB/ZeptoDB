---
hide:
  - navigation
  - toc
---

# ZeptoDB

## Ultra-Low Latency In-Memory Database

An ultra-low latency in-memory database unifying real-time and analytics.
Built for quants, ready for everything — from research notebooks to production trading.

<div class="grid cards" markdown>

-   :material-lightning-bolt:{ .lg .middle } **5.52M ticks/sec**

    ---

    Production-grade ingestion matching kdb+ performance

-   :material-timer-sand:{ .lg .middle } **272μs filter (1M rows)**

    ---

    Within kdb+ range, powered by Highway SIMD + LLVM JIT

-   :material-language-python:{ .lg .middle } **522ns Python zero-copy**

    ---

    NumPy view — no serialization, no copy

-   :material-database-search:{ .lg .middle } **Standard SQL**

    ---

    ClickHouse-compatible HTTP API, Grafana-ready

</div>

## Quick Start

```bash
# Start server
./zepto_server --port 8123

# Query
curl -X POST http://localhost:8123/ \
  -d 'SELECT vwap(price, volume), count(*) FROM trades WHERE symbol = 1'
```

```python
import zeptodb

db = zeptodb.Pipeline()
db.start()
db.ingest(symbol=1, price=15000, volume=100)
db.drain()

prices = db.get_column(symbol=1, name="price")  # 522ns zero-copy
```

## kdb+ Replacement Rate

| Domain | Rate | Status |
|--------|------|--------|
| **HFT** (tick processing + real-time) | **95%** | ✅ Production-ready |
| **Quant** (backtesting + research) | **90%** | ✅ Production-ready |
| **Risk/Compliance** | **95%** | ✅ Production-ready |

[Get Started](getting-started/quickstart.md){ .md-button .md-button--primary }
[API Reference](api/SQL_REFERENCE.md){ .md-button }
[Architecture](design/high_level_architecture.md){ .md-button }
