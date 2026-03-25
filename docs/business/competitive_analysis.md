# ZeptoDB Competitive Analysis

> Last updated: 2026-03-24 | Based on measured benchmarks and implemented features

---

## Market Positioning

ZeptoDB occupies a unique position: **ultra-low latency in-memory database with standard SQL**.
It targets the gap between kdb+ (fast but proprietary/expensive) and open-source time-series
databases (accessible but slow for real-time).

```
                    Latency
                    μs ──────────────────────────── sec
                    │                                │
         kdb+  ●───┤                                │
       ZeptoDB ●───┤                                │
               │   │                                │
      QuestDB  │   ●────┤                           │
               │        │                           │
  TimescaleDB  │        │    ●──────┤               │
               │        │          │                │
   ClickHouse  │        │          ●────────┤       │
               │        │                   │       │
    InfluxDB   │        │                   ●───────┤
               │        │                           │
               └────────┴───────────────────────────┘
               Real-time                      Batch
               (tick-by-tick)              (analytics)
```

---

## Use Cases Beyond HFT

| Sector | Use Case | Why Real-Time Matters |
|--------|----------|----------------------|
| **Manufacturing/IoT** | Factory sensor anomaly detection, predictive maintenance | Detect failure in μs, not seconds — prevents production line damage |
| **Autonomous Vehicles** | LiDAR/CAN bus telemetry processing | Safety-critical: ms-level decision latency |
| **Semiconductor** | Fab process monitoring, real-time yield tracking | Wafer defects caught early save millions |
| **Ad Tech** | Real-time bidding (RTB) | 100ms auction deadline — every μs counts |
| **Gaming** | Event stream processing, cheat detection, matchmaking | Player experience degrades with latency |
| **Observability** | APM metrics/traces/logs real-time aggregation | MTTR reduction: faster detection = faster resolution |
| **Network Security** | Traffic analysis, DDoS detection | Attack mitigation must be sub-second |
| **Energy/Utilities** | Power grid real-time balancing, smart meter analytics | Grid stability requires instant response |

---

## Direct Competitors

### Tier 1 — Primary Competition

#### kdb+ (KX Systems)

The incumbent ZeptoDB aims to replace.

| Dimension | kdb+ | ZeptoDB | Winner |
|-----------|------|---------|--------|
| Query latency | ~100-500μs | 272μs (filter 1M) | Tie |
| Ingestion | ~5M/sec | 5.52M/sec | Tie |
| Language | q (proprietary) | Standard SQL | **ZeptoDB** |
| Python integration | PyKX (IPC, copy) | Zero-copy (522ns) | **ZeptoDB** |
| License cost | $100K+/core/year | Open-source | **ZeptoDB** |
| Financial functions | Native (30+ years) | ASOF, xbar, EMA, wj, uj, pj, aj0 | kdb+ (more mature) |
| Production track record | 30 years, every major bank | New project | kdb+ |
| Migration path | — | q→SQL transpiler, HDB loader | **ZeptoDB** |

**Strategy**: Cost + accessibility. Same performance, 1/100th the price, no q learning curve.

#### QuestDB

The closest open-source competitor in positioning.

| Dimension | QuestDB | ZeptoDB | Winner |
|-----------|---------|---------|--------|
| Language | Java (JVM) | C++20 | **ZeptoDB** (no GC) |
| Ingestion | ~4M rows/sec | 5.52M rows/sec | **ZeptoDB** |
| Query latency | ms range | μs range (272μs) | **ZeptoDB** |
| SQL compatibility | PostgreSQL wire | ClickHouse HTTP | Tie |
| GC pauses | Yes (JVM) | None (arena allocator) | **ZeptoDB** |
| Financial functions | Basic (no ASOF JOIN) | Full kdb+ set | **ZeptoDB** |
| Community | Growing (~14K GitHub stars) | New | QuestDB |
| Maturity | 5+ years | New | QuestDB |

**Strategy**: Performance superiority. 10-100x lower latency, deterministic (no GC jitter).

#### TimescaleDB

The enterprise default for time-series on PostgreSQL.

| Dimension | TimescaleDB | ZeptoDB | Winner |
|-----------|-------------|---------|--------|
| SQL completeness | Full PostgreSQL | Core SQL + financial | TimescaleDB |
| Ecosystem | PG extensions, ORMs, BI tools | Python DSL, HTTP API | TimescaleDB |
| Query latency | ms-sec (PG overhead) | μs | **ZeptoDB** |
| Ingestion | ~1M rows/sec | 5.52M rows/sec | **ZeptoDB** |
| Architecture | Disk-first + cache | Memory-first + HDB flush | **ZeptoDB** (hot path) |
| Operational complexity | PostgreSQL DBA required | Single binary | **ZeptoDB** |

**Strategy**: Different layer. TimescaleDB for warm/cold analytics, ZeptoDB for hot real-time path.

### Tier 2 — Adjacent Competition

| Product | Overlap | Key Difference |
|---------|---------|----------------|
| **ClickHouse** | Analytics SQL | Batch-optimized, not real-time. INSERT throttling (1/sec recommended). ms-sec latency. |
| **InfluxDB** | IoT time-series | Flux language confusion, performance ceiling. Good ecosystem (Telegraf/Grafana). |
| **TDengine** | IoT time-series | Super-table concept, strong in China. Weak global ecosystem. |
| **Redis + TimeSeries** | Real-time cache | No analytical capability. Memory inefficient for time-series. |
| **DragonflyDB** | Low-latency store | Redis-compatible, no time-series or analytical functions. |

### Tier 3 — Streaming (Different Category)

| Product | Overlap | Key Difference |
|---------|---------|----------------|
| **Materialize** | Real-time SQL | Incremental view maintenance, not a storage engine. Kafka-dependent. |
| **RisingWave** | Streaming SQL | PostgreSQL-compatible streaming. Early stage, performance unproven. |
| **Apache Flink** | Stream processing | Framework, not a database. Requires separate storage. |

---

## ZeptoDB Competitive Advantages (Measured)

### 1. Latency — 10-100x vs Open-Source Alternatives

| Operation | ZeptoDB | QuestDB | TimescaleDB | ClickHouse |
|-----------|---------|---------|-------------|------------|
| Filter 1M rows | **272μs** | ~5-10ms | ~50-100ms | ~10-50ms |
| VWAP 1M rows | **532μs** | N/A | ~100ms+ | ~10-50ms |
| xbar 1M rows | **11ms** | N/A | N/A | ~50-100ms |
| SQL parse | **1.5-4.5μs** | ~100μs+ | ~1ms+ | ~10-100μs |

### 2. Ingestion — Continuous, Not Batched

- **5.52M ticks/sec** sustained ingestion
- Tick-level query availability (no batch delay)
- MPMC ring buffer → zero allocation on hot path
- ClickHouse officially recommends max 1 INSERT/sec; QuestDB buffers

### 3. Python Zero-Copy — 522ns

- NumPy view directly into arena memory — no serialization
- Polars-style lazy DSL executed in C++ engine
- vs. ClickHouse `clickhouse-connect`: serialize → transfer → deserialize → copy
- vs. QuestDB: JDBC/HTTP → parse → allocate → copy

### 4. Deterministic Performance

- Arena allocator: no malloc/free on hot path
- No GC (vs. QuestDB JVM)
- No WAL fsync on hot path (WAL is async)
- Highway SIMD vectorized scans
- Cache-line aligned TickMessage (64 bytes)

### 5. Financial Functions — Native SQL

```sql
-- These are 1-line SQL in ZeptoDB, complex workarounds elsewhere
SELECT xbar(timestamp, 300000000000) AS bar, first(price), last(price) ...
SELECT EMA(price, 20) OVER (PARTITION BY symbol ORDER BY timestamp)
SELECT * FROM trades t ASOF JOIN quotes q ON t.symbol = q.symbol ...
SELECT * FROM trades t WINDOW JOIN quotes q ON t.symbol = q.symbol ...
```

---

## Competitive Gaps (Honest Assessment)

| Gap | Impact | Mitigation |
|-----|--------|------------|
| **No variable-length string** | Free-text columns (logs, comments) not supported | Dictionary-encoded STRING covers symbol/exchange/side (95% of HFT use cases) |
| **No JDBC/ODBC** | No Tableau/Excel/BI tools | ClickHouse HTTP protocol provides partial Grafana compat |
| **Small community** | Trust/adoption barrier | Open-source + documentation + benchmarks |
| **Limited SQL** | Complex subqueries may fail | CTE + basic subquery supported; expanding |
| **No disk-first mode** | Large historical datasets | HDB flush to Parquet/S3; hybrid RDB+HDB query |

---

## Win Scenarios

| Scenario | Why ZeptoDB Wins |
|----------|-----------------|
| **kdb+ cost reduction** | Same performance, open-source, standard SQL |
| **Real-time + analytics unified** | One DB for tick plant AND research notebooks |
| **Python-native quant workflow** | Zero-copy NumPy, Polars DSL, no IPC overhead |
| **IoT edge → cloud** | Single binary embeddable, async cloud sync planned |
| **Latency-sensitive monitoring** | μs query for real-time dashboards/alerts |

## Lose Scenarios

| Scenario | Why ZeptoDB Loses |
|----------|-------------------|
| **PB-scale historical analytics** | ClickHouse/Snowflake better suited |
| **Full PostgreSQL compatibility needed** | TimescaleDB wins |
| **Existing ClickHouse/Grafana stack** | Migration cost not justified |
| **String-heavy workloads (logs)** | Variable-length strings not yet supported (dictionary-encoded only) |
| **Enterprise procurement requires vendor** | No commercial entity yet |
