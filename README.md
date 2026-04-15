<div align="center">

# ⚡ ZeptoDB

### In-Memory Time-Series Database for High-Throughput Workloads

*Ingest millions of events per second. Analyze them in microseconds.*

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?logo=cplusplus)](https://en.cppreference.com/w/cpp/20)
[![LLVM 19](https://img.shields.io/badge/LLVM-19-orange?logo=llvm)](https://llvm.org/)
[![Highway SIMD](https://img.shields.io/badge/SIMD-Highway-green)](https://github.com/google/highway)
[![Tests](https://img.shields.io/badge/tests-830%2B%20passing-brightgreen?logo=googletest)](tests/)
[![License](https://img.shields.io/badge/License-BSL_1.1-blue)](LICENSE)
[![Docs](https://img.shields.io/badge/docs-docs.zeptodb.com-blue?logo=readthedocs)](https://docs.zeptodb.com)
[![Discord](https://img.shields.io/discord/1492174712359354590?color=5865F2&logo=discord&logoColor=white&label=Discord)](https://discord.gg/zeptodb)
[![Docker Pulls](https://img.shields.io/docker/pulls/zeptodb/zeptodb?logo=docker)](https://hub.docker.com/r/zeptodb/zeptodb)
[![PyPI](https://img.shields.io/pypi/v/zeptodb?logo=python&logoColor=white)](https://pypi.org/project/zeptodb/)

[Quick Start](#-quick-start) · [Why ZeptoDB](#-why-zeptodb) · [Performance](#-performance) · [SQL Examples](#-sql-examples) · [Docs](https://docs.zeptodb.com) · [Community](#-community)

</div>

---

<!--
<div align="center">
  <img src="docs/assets/demo.gif" alt="ZeptoDB Demo" width="720">
  <br><em>From zero to query results in 30 seconds</em>
</div>
-->

## What is ZeptoDB?

ZeptoDB is an in-memory columnar database purpose-built for time-series analytics at scale.

It handles **high-throughput ingestion** and **real-time analytical queries** simultaneously — without trade-offs between the two.

The engine is hardware-software co-optimized: Highway SIMD vectorization, LLVM JIT compilation, lock-free ring buffers, NUMA-aware allocation, and UCX/RDMA networking — all working together to eliminate unnecessary copies, allocations, and cache misses.

```
┌─────────────────────────────────────────────────────────────┐
│  Clients: HTTP API · Python DSL · C++ API · Arrow Flight    │
├─────────────────────────────────────────────────────────────┤
│  SQL Engine: Parser (1.5μs) · AST Optimizer · Executor      │
├─────────────────────────────────────────────────────────────┤
│  Execution: Highway SIMD · LLVM JIT · Partition-parallel    │
│  ASOF JOIN · Window JOIN · xbar · EMA · VWAP                │
├─────────────────────────────────────────────────────────────┤
│  Ingestion: Lock-free MPMC Ring Buffer · WAL · Feed Handlers│
├─────────────────────────────────────────────────────────────┤
│  Storage: Arena Allocator · Column Store · Tiered (→S3)     │
├─────────────────────────────────────────────────────────────┤
│  Cluster: Consistent Hashing · RF=2 · Auto Failover         │
├─────────────────────────────────────────────────────────────┤
│  Security: TLS · JWT/OIDC · RBAC · Audit (SOC2/MiFID II)   │
└─────────────────────────────────────────────────────────────┘
```

---

## 🔍 Why ZeptoDB?

| | **ZeptoDB** | **kdb+** | **ClickHouse** | **TimescaleDB** | **QuestDB** |
|---|:---:|:---:|:---:|:---:|:---:|
| **Ingestion** | 5.52M evt/s | ~5M evt/s | ~100K evt/s | ~50K evt/s | ~1M evt/s |
| **Filter 1M rows** | 272μs | ~300μs | ~10ms | ~50ms | ~2ms |
| **ASOF JOIN** | ✅ Native | ✅ Native | ❌ UDF | ❌ Manual | ✅ Native |
| **SQL** | ✅ Standard | ❌ q lang | ✅ Dialect | ✅ PostgreSQL | ✅ Dialect |
| **Python zero-copy** | 522ns | ❌ IPC only | ❌ | ❌ | ❌ |
| **License cost** | Free (BSL-1.1) | $100K–500K/yr | Free (Apache 2.0) | Free (Apache 2.0) | Free (Apache 2.0) |

**TL;DR:** kdb+ performance, standard SQL, zero-copy Python, open-source pricing.

---

## 🚀 Quick Start

| Method | Command |
|--------|---------|
| **Binary** | Download from [GitHub Releases](https://github.com/ZeptoDB/ZeptoDB/releases) |
| **Homebrew** | `brew install ZeptoDB/tap/zeptodb` |
| **Docker** | `docker run -p 8123:8123 zeptodb/zeptodb:0.0.1` |
| **PyPI** | `pip install zeptodb` |
| **Source** | [Build instructions below](#build-from-source) |

### Binary

```bash
# amd64
curl -LO https://github.com/ZeptoDB/ZeptoDB/releases/latest/download/zeptodb-linux-amd64-0.0.1.tar.gz
tar xzf zeptodb-linux-amd64-0.0.1.tar.gz
./zeptodb-linux-amd64-0.0.1/zepto_http_server --port 8123

# arm64 (AWS Graviton)
curl -LO https://github.com/ZeptoDB/ZeptoDB/releases/latest/download/zeptodb-linux-arm64-0.0.1.tar.gz
```

> **Note:** Prebuilt binaries require runtime libraries (LLVM 19, Arrow, etc.). See the [Binary Installation Guide](docs/getting-started/BINARY_INSTALL.md) for prerequisites and troubleshooting.

### Docker

```bash
docker run -p 8123:8123 zeptodb/zeptodb:0.0.1

# Insert data
curl -X POST http://localhost:8123/ \
  -d "INSERT INTO trades VALUES (1, 1714000000000000000, 185.50, 100)"

# Query
curl -X POST http://localhost:8123/ \
  -d "SELECT vwap(price, volume), count(*) FROM trades WHERE symbol = 'AAPL'"
```

### Python

```python
import zeptodb

db = zeptodb.Pipeline()
db.start()
db.ingest(symbol=1, price=185.50, volume=100)
db.drain()

# Zero-copy numpy access (522ns)
prices = db.get_column(symbol=1, name="price")
```

### Build from Source

```bash
# Dependencies (Amazon Linux 2023 / Fedora)
sudo dnf install -y clang19 clang19-devel llvm19-devel \
  highway-devel numactl-devel ucx-devel ninja-build lz4-devel

mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19
ninja -j$(nproc)

./zepto_http_server --port 8123
```

📖 Full guide: [Quick Start](https://docs.zeptodb.com/getting-started/QUICK_START/) · [Python Reference](docs/api/PYTHON_REFERENCE.md) · [SQL Reference](docs/api/SQL_REFERENCE.md)

---

## 📊 Performance

Single node. End-to-end latencies including SQL parsing. No cherry-picking.

| Operation | Latency | Notes |
|-----------|---------|-------|
| **Ingestion throughput** | **5.52M events/sec** | Lock-free MPMC ring buffer |
| Filter 1M rows | **272μs** | Highway SIMD vectorized scan |
| VWAP 1M rows | **532μs** | Fused price×volume aggregation |
| GROUP BY (8 threads) | **248μs** | Partition-parallel scatter/gather |
| EMA 1M rows | **2.2ms** | Streaming exponential moving average |
| Window SUM 1M rows | **1.36ms** | Prefix-sum O(n) algorithm |
| xbar (1M → 3,334 bars) | **11ms** | Time-bucketed OHLCV |
| SQL parse | **1.5–4.5μs** | Recursive descent, zero allocation |
| Python column access | **522ns** | Zero-copy shared memory |
| Indexed lookup (g#/p#) | **3.3μs** | 274× faster than full scan |
| HDB flush to disk | **4.8 GB/s** | LZ4 compressed |
| Partition routing | **2ns** | Consistent hash ring |

---

## 💡 SQL Examples

```sql
-- 5-minute OHLCV candlestick bars
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low, last(price) AS close,
       sum(volume) AS volume
FROM trades WHERE symbol = 'AAPL'
GROUP BY xbar(timestamp, 300000000000)

-- ASOF JOIN (point-in-time lookup)
SELECT t.price, q.bid, q.ask
FROM trades t
ASOF JOIN quotes q
ON t.symbol = q.symbol AND t.timestamp >= q.timestamp

-- EMA with delta
SELECT symbol, price,
       EMA(price, 20) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema20,
       DELTA(price) OVER (ORDER BY timestamp) AS price_change
FROM trades

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

-- Storage tiering
ALTER TABLE trades SET STORAGE POLICY
  HOT 1 HOURS WARM 24 HOURS COLD 30 DAYS DROP 365 DAYS
```

Full SQL reference: [SQL_REFERENCE.md](docs/api/SQL_REFERENCE.md) — INSERT, UPDATE, DELETE, CASE WHEN, LIKE, UNION, CTE, subqueries, and more.

---

## 🏗️ Use Cases

| Domain | Why ZeptoDB | Key Features |
|--------|------------|--------------|
| **Finance / HFT** | Sub-ms tick processing, kdb+-class perf | ASOF JOIN, xbar, EMA, VWAP |
| **Quant Research** | Backtest in Python, execute in C++ | Zero-copy numpy, Polars DSL |
| **Crypto / DeFi** | 24/7 multi-exchange streaming | Binance feed handler, real-time agg |
| **IoT / Manufacturing** | High-frequency sensor ingestion | DELTA/RATIO, time-bar agg, LZ4 |
| **Autonomous Vehicles** | Sensor fusion, driving log replay | ASOF JOIN, Parquet HDB, parallel scan |
| **Observability** | High-cardinality metrics | SQL + Grafana, TTL + S3 tiering |

---

## ⚙️ Optimization Stack

<table>
<tr><th>Hardware</th><th>Software</th></tr>
<tr><td>

- **Highway SIMD** — 256/512-bit vectorized scans
- **NUMA-aware** — memory pinned to local node
- **UCX / RDMA** — kernel-bypass networking
- **Arena allocator** — zero GC, zero fragmentation

</td><td>

- **LLVM JIT** — runtime expression compilation
- **Lock-free MPMC** — zero-contention ingestion
- **Columnar storage** — cache-friendly sequential access
- **Partition-parallel** — 3.48× scaling at 8 threads

</td></tr>
</table>

---

## 🔒 Enterprise Security

| Feature | Details |
|---------|---------|
| TLS/HTTPS | OpenSSL 3.2, cert/key PEM |
| Authentication | API Key (SHA256) + JWT/OIDC (HS256/RS256, JWKS auto-fetch) |
| Authorization | RBAC: 5 roles + symbol-level ACL + multi-tenancy |
| Rate Limiting | Token bucket per-identity + per-IP |
| Secrets | Vault KV v2 → K8s secrets → env var (priority chain) |
| Audit Log | 7-year retention, SOC2/EMIR/MiFID II compliant |

---

## 🚢 Deployment

```bash
# Docker
docker run -p 8123:8123 zeptodb/zeptodb

# Helm
helm install zeptodb ./deploy/helm/zeptodb

# Bare-metal (systemd)
./deploy/scripts/install_service.sh
```

Guides: [Production Deployment](docs/deployment/PRODUCTION_DEPLOYMENT.md) · [Kubernetes](docs/operations/KUBERNETES_OPERATIONS.md) · [Bare-metal Tuning](docs/deployment/BARE_METAL_TUNING.md)

---

## 🔄 Migration

Migrate from existing databases with built-in tooling:

```bash
./zepto-migrate --source kdb+ --hdb-path /data/hdb --target localhost:8123
```

Supported: **kdb+** (HDB loader, q→SQL) · **ClickHouse** (DDL/query conversion) · **DuckDB** (Parquet) · **TimescaleDB** (hypertable conversion)

---

## 💬 Community

- [Discord](https://discord.gg/zeptodb) — questions, discussions, real-time help
- [GitHub Discussions](https://github.com/ZeptoDB/ZeptoDB/discussions) — design proposals, Q&A, ideas
- [Twitter/X](https://twitter.com/zeptodb) — release announcements, benchmarks

---

## 🤝 Contributing

We welcome contributions of all sizes — from typo fixes to new features.

- 🏷️ Check out issues labeled [`good-first-issue`](https://github.com/ZeptoDB/ZeptoDB/labels/good-first-issue) for easy starting points
- 📖 Read [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions and guidelines
- 💬 Join [Discord](https://discord.gg/zeptodb) to discuss ideas before starting large changes

---

<div align="center">

If ZeptoDB is useful to you, consider giving it a ⭐ — it helps others discover the project.

</div>

---

## 📄 License

[Business Source License 1.1](LICENSE) — Production use permitted, except offering as a commercial DBaaS. Changes to Apache 2.0 on 2030-04-01.

For commercial licensing: skswlsaks@gmail.com
