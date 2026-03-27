---
hide:
  - navigation
  - toc
---

<style>
:root {
  --zepto-primary: #4D7CFF;
  --zepto-primary-light: #8EACFF;
  --zepto-primary-dark: #2A45B3;
  --zepto-accent: #00F5D4;
  --zepto-accent-dark: #00BFA5;
  --zepto-success: #00E676;
  --zepto-bg: #0A0C10;
  --zepto-surface: #11161D;
}

.zepto-hero {
  text-align: center;
  padding: 2rem 0 1rem;
}
.zepto-hero h1 {
  font-size: 2.4rem;
  font-weight: 700;
  margin-bottom: 0.5rem;
}
.zepto-hero h1 .brand-zepto { color: var(--zepto-primary); }
.zepto-hero h1 .brand-db { color: #E2E8F0; font-weight: 300; }
.zepto-hero .tagline {
  font-size: 1.3rem;
  color: var(--zepto-accent);
  font-weight: 600;
  margin-bottom: 0.3rem;
}
.zepto-hero .subtitle {
  font-size: 1rem;
  opacity: 0.8;
  max-width: 700px;
  margin: 0 auto;
}

.use-case-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 1rem;
  margin: 1.5rem 0;
}
.use-case-card {
  border: 1px solid rgba(255,255,255,0.08);
  border-radius: 8px;
  padding: 1.2rem;
  transition: border-color 0.2s;
}
.use-case-card:hover {
  border-color: var(--zepto-primary);
}
.use-case-card .icon { font-size: 1.5rem; margin-bottom: 0.5rem; }
.use-case-card h3 { margin: 0 0 0.4rem; font-size: 1rem; }
.use-case-card p { margin: 0; font-size: 0.85rem; opacity: 0.8; }
</style>

<div class="zepto-hero" markdown>

# <span class="brand-zepto">Zepto</span><span class="brand-db">DB</span>

<div class="tagline">Built for Quants. Ready for Everything.</div>
<div class="subtitle">
Ultra-low latency <strong>in-memory</strong> time-series database.<br>
From trading desks to factory floors to autonomous machines — wherever μs matters.
</div>

</div>

<div class="grid cards" markdown>

-   :material-memory:{ .lg .middle } **Pure In-Memory Engine**

    ---

    All data lives in memory. Arena allocator + column store = zero disk I/O on the hot path.

-   :material-lightning-bolt:{ .lg .middle } **5.52M ticks/sec**

    ---

    Production-grade ingestion powered by MPMC ring buffer + Highway SIMD.

-   :material-timer-sand:{ .lg .middle } **272μs query (1M rows)**

    ---

    LLVM JIT compiled execution. ASOF JOIN, Window JOIN, EMA — all at μs latency.

-   :material-language-python:{ .lg .middle } **522ns Python zero-copy**

    ---

    NumPy/Pandas view directly from engine memory. No serialization, no copy.

-   :material-database-search:{ .lg .middle } **Standard SQL**

    ---

    ClickHouse-compatible HTTP API. Grafana, DBeaver, any SQL tool — just connect.

-   :material-shield-lock:{ .lg .middle } **Enterprise Security**

    ---

    TLS 1.3, RBAC (5 roles), JWT/OIDC, audit log. SOC2 & MiFID II ready.

</div>

---

## One Database, Every Industry

Technology proven in HFT, expanded to every domain where time-series data is mission-critical.

<div class="use-case-grid">
  <div class="use-case-card">
    <div class="icon">📈</div>
    <h3>Trading & Finance</h3>
    <p>HFT tick processing, quant backtesting, risk/compliance. kdb+ performance at zero license cost.</p>
  </div>
  <div class="use-case-card">
    <div class="icon">🎮</div>
    <h3>Gaming</h3>
    <p>Real-time player telemetry, in-game economy analytics, matchmaking metrics at μs latency.</p>
  </div>
  <div class="use-case-card">
    <div class="icon">🤖</div>
    <h3>Physical AI & Robotics</h3>
    <p>Sensor fusion, reinforcement learning feature store, zero-copy PyTorch pipeline. 522ns to tensor.</p>
  </div>
  <div class="use-case-card">
    <div class="icon">🏭</div>
    <h3>Smart Factory</h3>
    <p>10KHz sensor ingestion, real-time anomaly detection, predictive maintenance with EMA/DELTA.</p>
  </div>
  <div class="use-case-card">
    <div class="icon">🚗</div>
    <h3>Autonomous Vehicles</h3>
    <p>LiDAR + Camera + IMU time sync via ASOF JOIN. Driving log replay from Parquet HDB.</p>
  </div>
  <div class="use-case-card">
    <div class="icon">🪙</div>
    <h3>Crypto & DeFi</h3>
    <p>24/7 multi-exchange orderbook streaming. Kafka consumer, VWAP, cross-exchange arbitrage.</p>
  </div>
</div>

---

## Why ZeptoDB?

| | **kdb+** | **ClickHouse** | **InfluxDB** | **ZeptoDB** |
|---|---|---|---|---|
| **Latency** | μs | ms | ms | **μs** |
| **Ingestion** | ~5M/sec | 100K/sec | 50K/sec | **5.52M/sec** |
| **SQL** | ✗ (q lang) | ✓ | InfluxQL | **✓ Standard SQL** |
| **Python** | IPC | — | — | **522ns zero-copy** |
| **ASOF JOIN** | ✓ | ✗ | ✗ | **✓** |
| **License** | $100K+/yr | OSS | OSS | **OSS** |
| **In-Memory** | ✓ | Disk-based | Disk-based | **✓ Pure In-Memory** |

---

## Quick Start

```bash
# Start server
./zepto_server --port 8123

# Query via SQL
curl -X POST http://localhost:8123/ \
  -d 'SELECT vwap(price, volume), count(*) FROM trades WHERE symbol = 1'
```

```python
import zeptodb

db = zeptodb.Pipeline()
db.start()
db.ingest(symbol=1, price=15000, volume=100)
db.drain()

prices = db.get_column(symbol=1, name="price")  # 522ns zero-copy numpy
```

---

## kdb+ Replacement Rate

| Domain | Rate | Status |
|--------|------|--------|
| **HFT** (tick processing + real-time) | **95%** | :white_check_mark: Production-ready |
| **Quant** (backtesting + research) | **90%** | :white_check_mark: Production-ready |
| **Risk/Compliance** | **95%** | :white_check_mark: Production-ready |

[Get Started](getting-started/quickstart.md){ .md-button .md-button--primary }
[API Reference](api/SQL_REFERENCE.md){ .md-button }
[Architecture](design/high_level_architecture.md){ .md-button }
