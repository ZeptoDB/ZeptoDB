# ZeptoDB — Product Positioning & Use Cases

Last updated: 2026-03-24

---

## Core Thesis

**"Built for quants. Ready for everything."**

ZeptoDB was born for quant research and HFT, but the technologies created in the process — μs latency, millions of ticks/sec ingestion, ASOF JOIN, zero-copy Python — are equally needed in every industry where large-scale time-series data exists.

```
Quant Research (origin)
  → Backtest Pipeline
    → Production Trading
      → Risk/Compliance
        → Crypto/DeFi
          → IoT/Smart Factory
            → Autonomous Vehicles/Robotics
              → Anywhere time-series data exists
```

---

## Positioning: One Database, Full Lifecycle

### Phase 1: Research (Quant Research)

The stage where quants develop strategies in Jupyter.

| Need | ZeptoDB Solution |
|------|-----------------|
| Load historical data quickly | Parquet HDB → 4.8 GB/s read |
| Analyze directly in Python | 522ns zero-copy numpy/pandas |
| Calculate technical indicators | EMA, DELTA, RATIO, xbar, VWAP — in SQL |
| Backtest loop | Reproduce historical points in time with ASOF JOIN |
| Explore large datasets | 1M rows filter 272μs, GROUP BY 248μs |

**Killer message:** "kdb+ performance in Python. Without the license cost."

### Phase 2: Production (Production Trading)

The stage where validated strategies run in real time.

| Need | ZeptoDB Solution |
|------|-----------------|
| Real-time tick ingestion | 5.52M ticks/sec, FIX/ITCH/Binance feed handler |
| Ultra-low latency queries | 272μs filter, g# index 3.3μs |
| Distributed processing | 3-node scatter-gather, auto failover |
| 24/7 zero downtime | Helm chart, rolling upgrade, PDB |
| Monitoring | Prometheus /metrics, Grafana dashboard |

**Killer message:** "The same DB you used in research, straight into production."

### Phase 3: Compliance (Risk/Regulation)

The stage where trading data is audited and reported.

| Need | ZeptoDB Solution |
|------|-----------------|
| Standard SQL | ClickHouse-compatible HTTP API |
| Audit log | WAL + audit log (SOC2/MiFID II) |
| Dashboards | Instant Grafana integration |
| Data retention | TTL policy, Parquet → S3 archive |
| Access control | RBAC 5 roles, API Key, JWT/OIDC |

**Killer message:** "Same data, same DB. From trading to audit."

---

## Beyond Finance: Industry Use Cases

The technology built for quants solves the same problems in other industries.

### IoT / Smart Factory

| Finance Feature | Industry Application |
|-----------|----------|
| 5.52M ticks/sec ingestion | Semiconductor fab 10KHz sensors × thousands of points |
| ASOF JOIN | Multi-sensor time alignment (temperature + vibration + current) |
| EMA / DELTA | Predictive maintenance — anomaly detection, trend analysis |
| xbar (time bucket) | Minute/hour aggregation dashboards |
| g#/p# index | O(1) lookup by equipment ID |

**Killer message:** "Factory sensor data powered by an ingestion engine proven in HFT."

### Autonomous Vehicles / Robotics

| Finance Feature | Industry Application |
|-----------|----------|
| zero-copy numpy | Sensor → PyTorch training pipeline |
| Window JOIN | LiDAR + Camera + IMU time synchronization |
| Parquet HDB | Long-term driving log storage + replay |
| Distributed cluster | Fleet data central aggregation |

### Crypto / DeFi

| Finance Feature | Industry Application |
|-----------|----------|
| 24/7 ingestion | Multi-exchange order book streaming |
| VWAP / xbar | Real-time price aggregation, candlestick charts |
| Kafka consumer | Binance/Coinbase WebSocket → ZeptoDB |
| ASOF JOIN | Cross-exchange arbitrage analysis |

### Observability / APM

| Finance Feature | Industry Application |
|-----------|----------|
| 5.52M events/sec | Large-scale log/metric ingestion |
| SQL + Grafana | Instant integration with existing monitoring stacks |
| TTL + S3 | Hot data in-memory, cold data on S3 |
| Distributed cluster | Multi-region metric aggregation |

---

## Competitive Positioning

```
                    ┌─────────────────────────────────────┐
                    │         ZeptoDB                       │
                    │  "Quant-grade for everyone"           │
                    │                                       │
                    │  ┌─────────┐  ┌──────────┐           │
                    │  │ kdb+    │  │ClickHouse│           │
                    │  │ Perf    │  │ SQL ease  │           │
                    │  └────┬────┘  └────┬─────┘           │
                    │       └─────┬──────┘                  │
                    │             │                          │
                    │    + Python zero-copy                  │
                    │    + Open source                       │
                    │    + Multi-industry                    │
                    └─────────────────────────────────────┘

vs kdb+:       Same performance, SQL + Python, free
vs ClickHouse: 1000x faster latency, ASOF JOIN, real-time
vs InfluxDB:   100x faster ingestion, financial functions, SIMD
vs TimescaleDB: In-memory μs latency, zero-copy Python
```

---

## Website Messaging Guide

### Homepage Hero
- Primary: "Built for Quants. Ready for Everything."
- Sub: "From research notebooks to production trading to factory floors — one database for all your time-series data."

### Target Audience Tabs (homepage)
1. **Quant / Researcher** — "Your Jupyter notebook, turbocharged"
2. **Trading Desk** — "Research to production, zero migration"
3. **Platform Engineer** — "Helm install, Grafana connect, done"
4. **IoT / Industry** — "HFT-grade ingestion for your sensors"

### Key Differentiators (repeat everywhere)
1. **μs latency** — not ms, not seconds
2. **Research → Production** — same DB, no migration
3. **Standard SQL + Python** — no q language, no vendor lock-in
4. **Open source** — no $100K/year license
5. **Multi-industry** — finance-proven, industry-ready
