<div align="center">

# ZeptoDB

### In-Memory Time-Series Database for High-Throughput Workloads

*Ingest millions of events per second. Analyze them in microseconds.*

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![LLVM 19](https://img.shields.io/badge/LLVM-19-orange)
![License](https://img.shields.io/badge/License-BSL_1.1-blue)
![Highway SIMD](https://img.shields.io/badge/SIMD-Highway-green)
![Tests](https://img.shields.io/badge/tests-726%2B%20passing-brightgreen)

</div>

---

## What is ZeptoDB?

ZeptoDB is an in-memory columnar database purpose-built for time-series analytics at scale.

It is designed from the ground up to handle two things simultaneously: **high-throughput ingestion** and **real-time analytical queries** — without trade-offs between the two.

The engine is hardware-software co-optimized. At the hardware level, it leverages SIMD vectorization (Highway), NUMA-aware memory allocation, and RDMA networking (UCX). At the software level, it uses LLVM JIT compilation, lock-free ring buffers, arena-based memory management, and a pure columnar storage layout — all working together to eliminate unnecessary copies, allocations, and cache misses.

The result is a general-purpose time-series database that serves finance, IoT, observability, autonomous systems, and any domain where data arrives fast and answers are needed faster.

---

## Key Design Principles

| Principle | How |
|-----------|-----|
| **In-memory first** | All hot data lives in memory. Column store with arena allocator — zero GC, zero fragmentation. |
| **Time-series native** | ASOF JOIN, Window JOIN, xbar, EMA, DELTA/RATIO — temporal operations are first-class, not bolted on. |
| **Hardware-optimized** | Highway SIMD for vectorized scans, NUMA-aware allocation, UCX/RDMA transport, CXL-ready. |
| **Software-optimized** | LLVM JIT for expression evaluation, lock-free MPMC ring buffer, partition-parallel execution. |
| **Ingest + Analyze** | 5.52M events/sec ingestion and sub-millisecond queries run concurrently on the same engine. |
| **Standard SQL** | No proprietary query language. Standard SQL over HTTP (ClickHouse wire-compatible). |
| **Zero-copy Python** | numpy/Polars integration via shared memory — 522ns column access, no serialization. |

---

## Performance

All numbers measured on a single node. No cherry-picking — these are end-to-end latencies including SQL parsing.

| Operation | Latency |
|-----------|---------|
| Ingestion throughput | **5.52M events/sec** |
| Filter 1M rows | **272μs** |
| VWAP 1M rows | **532μs** |
| EMA 1M rows | **2.2ms** |
| xbar (1M → 3,334 bars) | **11ms** |
| Window SUM 1M rows | **1.36ms** |
| Parallel GROUP BY (8 threads) | **248μs** |
| SQL parse | **1.5–4.5μs** |
| Python zero-copy column access | **522ns** |
| HDB flush to disk | **4.8 GB/s** |
| Partition routing | **2ns** |
| Indexed lookup (g#/p#) | **3.3μs** (274× vs full scan) |

---

## Architecture

```
┌────────────────────────────────────────────────────────┐
│  Security Layer (Cross-cutting)                         │
│  TLS/HTTPS · API Key · JWT/OIDC · RBAC (5 roles)      │
│  Rate Limiting · Audit Log (SOC2/EMIR/MiFID II)       │
├────────────────────────────────────────────────────────┤
│  Client Interface                                       │
│  HTTP API (port 8123) · Python DSL · C++ API           │
├────────────────────────────────────────────────────────┤
│  SQL Engine                                             │
│  Recursive descent parser · AST optimizer · Executor   │
├────────────────────────────────────────────────────────┤
│  Execution Engine                                       │
│  Highway SIMD · LLVM JIT · Partition-parallel scan     │
│  JOIN: ASOF / Window / Hash / LEFT / RIGHT / FULL      │
│  Window: EMA / DELTA / RATIO / LAG / LEAD / RANK       │
│  Financial: xbar / FIRST / LAST / VWAP                 │
├────────────────────────────────────────────────────────┤
│  Ingestion (Tick Plant)                                 │
│  Lock-free MPMC Ring Buffer · Multi-threaded drain     │
│  UCX/RDMA transport · WAL · Feed handlers              │
├────────────────────────────────────────────────────────┤
│  Storage Engine                                         │
│  Arena allocator · Column store · Tiered storage       │
│  HDB (LZ4/Parquet) · S3 sink                          │
├────────────────────────────────────────────────────────┤
│  Distributed Cluster                                    │
│  Consistent hashing · Replication (RF=2)               │
│  Auto failover · Split-brain defense · WAL replication │
└────────────────────────────────────────────────────────┘
```

---

## Quick Start

### SQL via HTTP

```bash
./zepto_server --port 8123

curl -X POST http://localhost:8123/ \
  -d "SELECT vwap(price, volume), count(*) FROM trades WHERE symbol = 'AAPL'"
```

Grafana connects directly as a ClickHouse data source — no adapter needed.

### Python

```python
import zeptodb
from zepto_py.dsl import DataFrame

db = zeptodb.Pipeline()
db.start()

db.ingest(symbol=1, price=15000, volume=100)
db.drain()

# Zero-copy numpy access (522ns)
prices = db.get_column(symbol=1, name="price")

# Polars-style lazy DSL — executed in C++
df = DataFrame(db, symbol=1)
result = df['price'].rolling(20).mean().collect()
```

### SQL Examples

```sql
-- 5-minute OHLCV candlestick bars
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low, last(price) AS close,
       sum(volume) AS volume
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)

-- EMA with row-to-row delta
SELECT symbol, price,
       EMA(price, 20) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema20,
       DELTA(price) OVER (ORDER BY timestamp) AS price_change
FROM trades

-- ASOF JOIN (point-in-time lookup)
SELECT t.price, q.bid, q.ask
FROM trades t
ASOF JOIN quotes q
ON t.symbol = q.symbol AND t.timestamp >= q.timestamp

-- Window JOIN (time-range aggregation)
SELECT t.price, wj_avg(q.bid) AS avg_bid
FROM trades t
WINDOW JOIN quotes q ON t.symbol = q.symbol
AND q.timestamp BETWEEN t.timestamp - 5000000000 AND t.timestamp + 5000000000

-- Materialized view (incremental, updated on ingest)
CREATE MATERIALIZED VIEW ohlcv_5min AS
  SELECT symbol, xbar(timestamp, 300000000000) AS bar,
         first(price) AS open, max(price) AS high,
         min(price) AS low, last(price) AS close,
         sum(volume) AS vol
  FROM trades
  GROUP BY symbol, xbar(timestamp, 300000000000)

-- Storage tiering (Hot → Warm → Cold → Drop)
ALTER TABLE trades SET STORAGE POLICY
  HOT 1 HOURS WARM 24 HOURS COLD 30 DAYS DROP 365 DAYS
```

Full SQL reference: INSERT, UPDATE, DELETE, CASE WHEN, LIKE, UNION/INTERSECT/EXCEPT, DATE_TRUNC, EPOCH_S, SUBSTR, and more. See [API Reference](docs/API_REFERENCE.md).

---

## Use Cases

ZeptoDB is not limited to a single domain. The same engine that processes trading ticks handles IoT sensor streams, observability metrics, and autonomous vehicle telemetry.

| Domain | Why ZeptoDB | Key Capabilities |
|--------|------------|-----------------|
| **Finance / HFT** | Sub-millisecond tick processing, kdb+-class performance | ASOF JOIN, xbar, EMA, Window JOIN, 5.52M ticks/sec |
| **Quant Research** | Backtest in Python, execute in C++ | Zero-copy numpy, Polars-style DSL, Jupyter integration |
| **Crypto / DeFi** | 24/7 multi-exchange, orderbook streaming | Binance feed handler, VWAP, real-time aggregation |
| **IoT / Manufacturing** | High-frequency sensor ingestion, anomaly detection | DELTA/RATIO, time-bar aggregation, LZ4 compression |
| **Autonomous Vehicles** | Sensor fusion, driving log replay at scale | ASOF JOIN, Parquet HDB, partition-parallel scan |
| **Observability** | High-cardinality metrics, log analytics | SQL + Grafana, TTL + S3 tiering, HTTP API |

---

## Optimization Stack

### Hardware Level

| Technique | Purpose |
|-----------|---------|
| **Highway SIMD** | Vectorized filter/scan — processes 256/512-bit lanes per cycle |
| **NUMA-aware allocation** | Memory pinned to local NUMA node — eliminates cross-socket latency |
| **UCX / RDMA** | Kernel-bypass networking for cluster communication |
| **CXL-ready** | Architecture prepared for CXL memory pooling |
| **Arena allocator** | Bulk allocation, no per-object malloc/free, zero fragmentation |

### Software Level

| Technique | Purpose |
|-----------|---------|
| **LLVM JIT** | Runtime code generation for expression evaluation — no interpreter overhead |
| **Lock-free MPMC ring buffer** | Ingestion pipeline with zero contention between producers and consumers |
| **Columnar storage** | Cache-friendly sequential access for analytical scans |
| **Partition-parallel execution** | Scatter/gather across partitions — 3.48× scaling at 8 threads |
| **Prefix-sum window functions** | O(n) window aggregation instead of O(n×w) |
| **Dictionary encoding** | String symbols stored as integers — constant-time equality checks |

---

## Enterprise Security

| Feature | Details |
|---------|---------|
| TLS/HTTPS | OpenSSL 3.2, cert/key PEM |
| Authentication | API Key (SHA256-hashed) + JWT/OIDC (HS256/RS256) |
| Authorization | RBAC: 5 roles + symbol-level ACL |
| Rate Limiting | Token bucket per-identity + per-IP |
| Query Management | Timeout, kill, tracking via Admin REST API |
| Secrets | Vault KV v2 → K8s secrets → env var (priority chain) |
| Audit Log | 7-year retention, SOC2/EMIR/MiFID II compliant |

---

## Deployment

| Option | Details |
|--------|---------|
| **Bare-metal** | Recommended for latency-sensitive workloads. Auto-tuning via `tune_bare_metal.sh`. |
| **Docker + Kubernetes** | Helm chart with PDB, HPA, rolling upgrades. AWS Graviton4 optimized. |
| **Monitoring** | Prometheus `/metrics`, Grafana dashboard, 9 alert rules, structured JSON access log. |
| **Operations** | Backup/restore scripts, systemd service, disaster recovery. |

```bash
# Helm
helm install zeptodb ./deploy/helm/zeptodb

# Bare-metal
./deploy/scripts/install_service.sh
```

Guides: [Production Deployment](docs/deployment/PRODUCTION_DEPLOYMENT.md) · [Operations](docs/operations/PRODUCTION_OPERATIONS.md) · [Kubernetes](docs/operations/KUBERNETES_OPERATIONS.md)

---

## Build

```bash
# Dependencies (Amazon Linux 2023 / Fedora)
sudo dnf install -y clang19 clang19-devel llvm19-devel \
  highway-devel numactl-devel ucx-devel ninja-build lz4-devel

mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19
ninja -j$(nproc)

# Tests
./tests/zepto_tests
python3 -m pytest ../tests/test_python.py -v
```

---

## Migration

Migrate from existing databases with built-in tooling:

| Source | What it does |
|--------|-------------|
| **kdb+** | HDB loader, q→SQL transpiler |
| **ClickHouse** | DDL/query conversion, data import |
| **DuckDB** | Parquet-based migration |
| **TimescaleDB** | Hypertable conversion |

```bash
./zepto-migrate --source kdb+ --hdb-path /data/hdb --target localhost:8123
```

---

## Project Structure

```
zeptodb/
├── include/zeptodb/     # Headers
│   ├── storage/         # Arena, ColumnStore, HDB, Parquet
│   ├── ingestion/       # RingBuffer, TickPlant, WAL
│   ├── execution/       # SIMD engine, JIT, JOINs, Window functions
│   ├── sql/             # Tokenizer, Parser, AST, Executor
│   ├── server/          # HTTP server (ClickHouse compatible)
│   ├── auth/            # RBAC, JWT, API keys, audit
│   ├── feeds/           # FIX, NASDAQ ITCH, Binance
│   ├── cluster/         # Distributed coordination
│   └── transpiler/      # Python binding (pybind11)
├── src/                 # Implementation
├── tests/               # 726+ tests (unit, feed, migration, Python)
├── deploy/              # Docker, K8s, Helm, monitoring, scripts
├── tools/               # CLI tools (migrate, server, data node)
└── docs/                # Design docs, guides, benchmarks
```

---

## License

ZeptoDB is licensed under the [Business Source License 1.1](LICENSE).

- **Additional Use Grant**: Production use is permitted, except offering ZeptoDB as a commercial DBaaS, managed service, or cloud service.
- **Change Date**: 2030-04-01
- **Change License**: Apache License, Version 2.0

For commercial licensing inquiries, contact skswlsaks@gmail.com.
