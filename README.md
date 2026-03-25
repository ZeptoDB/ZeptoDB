<div align="center">

# ZeptoDB

### Ultra-Low Latency In-Memory Database

*An ultra-low latency in-memory database unifying real-time and analytics*

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![LLVM 19](https://img.shields.io/badge/LLVM-19-orange)
![Highway SIMD](https://img.shields.io/badge/SIMD-Highway-green)
![Tests](https://img.shields.io/badge/tests-726%2B%20passing-brightgreen)
![kdb+ replacement](https://img.shields.io/badge/kdb%2B%20replacement-95%25-success)

</div>

---

## Overview

ZeptoDB is an ultra-low latency in-memory database designed for HFT, combining
**kdb+ performance**, **ClickHouse versatility**, and the **Polars Python ecosystem**.

Built for quants, ready for everything — from research notebooks to production
trading to factory sensor floors. One database for the full lifecycle.

### kdb+ Replacement Rate (2026-03-22)

| Domain | Rate | Status |
|--------|------|--------|
| **HFT** (tick processing + real-time) | **95%** | ✅ Production-ready |
| **Quant** (backtesting + research) | **90%** | ✅ Production-ready |
| **Risk/Compliance** | **95%** | ✅ Production-ready |

**Core financial functions included:**
- ✅ xbar (time bar aggregation) — 5-min/1-hour OHLCV candlestick charts
- ✅ EMA (Exponential Moving Average) — technical indicators
- ✅ Window JOIN (wj) — time window join (HFT quote analysis)
- ✅ LEFT JOIN, ASOF JOIN, Hash JOIN, RIGHT JOIN, FULL OUTER JOIN
- ✅ UNION JOIN (uj), PLUS JOIN (pj), AJ0 (left-columns-only ASOF)
- ✅ DELTA/RATIO (row-to-row difference/ratio)
- ✅ FIRST/LAST (OHLC)

### Core Performance (Measured)

| Metric | Value |
|--------|-------|
| Ingestion | **5.52M ticks/sec** |
| filter 1M rows | **272μs** (within kdb+ range) |
| VWAP 1M rows | **532μs** |
| **xbar (time bar)** | **11ms** (1M → 3,334 bars) |
| **EMA 1M rows** | **2.2ms** |
| Window SUM 1M | **1.36ms** (O(n) prefix sum) |
| **Parallel GROUP BY 8T** | **248μs** (3.48x vs 1T) |
| SQL parsing | **1.5~4.5μs** |
| Python zero-copy | **522ns** (numpy view) |
| HDB flush | **4.8 GB/s** |
| Partition routing | **2ns** |
| **g# index lookup** | **3.3μs** (274x vs full scan) |
| **p# index lookup** | **3.4μs** (269x vs full scan) |

### vs kdb+ / ClickHouse

| | kdb+ | ClickHouse | **ZeptoDB** |
|---|---|---|---|
| Ingestion | ~5M/sec | Batch-optimized | **5.52M/sec** |
| filter 1M | ~100-300μs | ms range | **272μs** ✅ |
| VWAP 1M | ~200-500μs | ms range | **532μs** ✅ |
| SQL | q language | Standard SQL | **Standard SQL** |
| Python | PyKX (IPC) | clickhouse-connect | **zero-copy** |
| Open-source | ❌ Paid | ✅ | ✅ |

---

## Architecture

```
┌────────────────────────────────────────────────────────┐
│  Security Layer (Cross-cutting)                         │
│  TLS/HTTPS · API Key · JWT/OIDC · RBAC (5 roles)      │
│  Rate Limiting · Admin REST API · Secrets Management   │
│  Query Timeout/Kill · Audit Log (SOC2/EMIR/MiFID II)  │
├────────────────────────────────────────────────────────┤
│  Layer 5: Client Interface                              │
│  HTTP API (port 8123) · Python DSL · C++ API          │
├────────────────────────────────────────────────────────┤
│  Layer 4: SQL + Query Planning                          │
│  Recursive descent parser · AST executor               │
├────────────────────────────────────────────────────────┤
│  Layer 3: Execution Engine                              │
│  Highway SIMD · LLVM JIT · JOIN (ASOF/Hash/LEFT/RIGHT/FULL) │
│  Window Functions (EMA/DELTA/RATIO/SUM/LAG/LEAD)      │
│  Financial functions (xbar/FIRST/LAST/Window JOIN)    │
│  String functions (SUBSTR) · uj/pj/aj0 (kdb+ JOINs)  │
│  QueryScheduler (Local/Distributed DI pattern)        │
│  Parallel Scan (Partition/CHUNKED) · Resource Isolation│
├────────────────────────────────────────────────────────┤
│  Layer 2: Ingestion (Tick Plant)                        │
│  MPMC Ring Buffer · UCX/RDMA · WAL                    │
│  Multi-threaded Drain · Direct-to-Storage Bypass      │
│  Feed Handlers (FIX, NASDAQ ITCH, Binance)            │
├────────────────────────────────────────────────────────┤
│  Layer 1: Storage Engine (DMMT)                         │
│  Arena Allocator · Column Store · HDB (LZ4/Parquet)   │
│  Parquet Reader/Writer · S3 Sink                      │
├────────────────────────────────────────────────────────┤
│  Migration Toolkit                                      │
│  kdb+ · ClickHouse · DuckDB · TimescaleDB             │
├────────────────────────────────────────────────────────┤
│  Layer 0: Distributed Cluster                           │
│  Transport (UCX→CXL) · Consistent Hash · Health       │
│  Replication (RF=2) · Auto Failover · Coordinator HA  │
│  ComputeNode · SnapshotCoordinator · WAL Replicator   │
│  Split-Brain Defense (FencingToken · K8s Lease)        │
└────────────────────────────────────────────────────────┘
```

---

## Quick Start

### SQL via HTTP (ClickHouse compatible)

```bash
# Start server
./zepto_server --port 8123

# Query via curl (string symbols supported)
curl -X POST http://localhost:8123/ \
  -d "SELECT vwap(price, volume), count(*) FROM trades WHERE symbol = 'AAPL'"

# Grafana: connect directly as a ClickHouse data source
```

### Python DSL (zero-copy)

```python
import zeptodb
from zepto_py.dsl import DataFrame

db = zeptodb.Pipeline()
db.start()

# Ingest ticks
db.ingest(symbol=1, price=15000, volume=100)
db.drain()

# zero-copy numpy (no copy)
prices = db.get_column(symbol=1, name="price")  # 522ns

# Lazy DSL (Polars-style)
df = DataFrame(db, symbol=1)
ma20 = df['price'].rolling(20).mean().collect()  # executed in C++
```

### SQL Examples

```sql
-- 5-minute OHLCV candlestick chart (kdb+ xbar style)
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low, last(price) AS close,
       sum(volume) AS volume
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)

-- Exponential Moving Average (EMA) + row-to-row difference/ratio
SELECT symbol, price,
       EMA(price, 20) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema20,
       DELTA(price) OVER (ORDER BY timestamp) AS price_change,
       RATIO(price) OVER (ORDER BY timestamp) AS price_ratio
FROM trades

-- ASOF JOIN (core for time-series)
SELECT t.price, q.bid, q.ask
FROM trades t
ASOF JOIN quotes q
ON t.symbol = q.symbol AND t.timestamp >= q.timestamp

-- Window JOIN (wj) — time window join
SELECT t.price, wj_avg(q.bid) AS avg_bid, wj_count(q.bid) AS quote_count
FROM trades t
WINDOW JOIN quotes q
ON t.symbol = q.symbol
AND q.timestamp BETWEEN t.timestamp - 5000000000 AND t.timestamp + 5000000000

-- LEFT JOIN
SELECT t.price, t.volume, r.risk_score
FROM trades t
LEFT JOIN risk_factors r ON t.symbol = r.symbol

-- FULL OUTER JOIN
SELECT t.price, r.price
FROM trades t
FULL OUTER JOIN quotes r ON t.symbol = r.symbol

-- UNION JOIN (kdb+ uj — merge columns, concatenate rows)
SELECT * FROM trades t UNION JOIN quotes q

-- PLUS JOIN (kdb+ pj — additive join)
SELECT * FROM trades t
PLUS JOIN adjustments a ON t.symbol = a.symbol

-- AJ0 (left-columns-only ASOF JOIN)
SELECT t.price, t.volume, q.bid FROM trades t
AJ0 JOIN quotes q ON t.symbol = q.symbol AND t.timestamp >= q.timestamp

-- SUBSTR (string manipulation on int64 columns)
SELECT SUBSTR(price, 1, 2) AS price_prefix FROM trades WHERE symbol = 1

-- Window functions
SELECT symbol, price,
       AVG(price) OVER (PARTITION BY symbol ROWS 20 PRECEDING) AS ma20,
       LAG(price, 1) OVER (PARTITION BY symbol) AS prev_price,
       RANK() OVER (ORDER BY price DESC) AS rank
FROM trades

-- SELECT arithmetic (Phase 2)
SELECT symbol, price * volume AS notional,
       (price - 15000) / 100 AS premium,
       SUM(price * volume) AS total_notional
FROM trades WHERE symbol = 1

-- CASE WHEN (Phase 2)
SELECT symbol, price,
       CASE WHEN price > 15050 THEN 1 ELSE 0 END AS is_high
FROM trades WHERE symbol = 1

-- Multi-column GROUP BY (Phase 2)
SELECT symbol, price, SUM(volume) AS vol
FROM trades GROUP BY symbol, price

-- Date/time functions (Phase 3)
SELECT DATE_TRUNC('min', timestamp) AS minute, SUM(volume) AS vol
FROM trades WHERE symbol = 1
GROUP BY DATE_TRUNC('min', timestamp)

SELECT EPOCH_S(timestamp) AS ts_sec, price FROM trades WHERE symbol = 1

-- LIKE / NOT LIKE (Phase 3)
SELECT symbol, price FROM trades WHERE price LIKE '150%'

-- UNION / INTERSECT / EXCEPT (Phase 3)
SELECT price FROM trades WHERE symbol = 1
UNION ALL
SELECT price FROM trades WHERE symbol = 2

SELECT price FROM trades WHERE symbol = 1
INTERSECT
SELECT price FROM trades WHERE price > 15050

-- INSERT (single row)
INSERT INTO trades VALUES (1, 15000, 100, 1711234567000000000)

-- INSERT (multi-row)
INSERT INTO trades VALUES (1, 15050, 200, 1711234568000000000),
                          (2, 16000, 300, 1711234569000000000)

-- INSERT (column list — timestamp auto-generated)
INSERT INTO trades (symbol, price, volume) VALUES (1, 15100, 150)

-- INSERT with string symbol (dictionary-encoded)
INSERT INTO trades (symbol, price, volume) VALUES ('AAPL', 150.25, 100)
INSERT INTO trades (symbol, price, volume) VALUES ('AAPL', 151.50, 200),
                                                  ('GOOGL', 2800.00, 50)

-- SELECT with string symbol
SELECT price, volume FROM trades WHERE symbol = 'AAPL'
SELECT VWAP(price, volume) FROM trades WHERE symbol = 'GOOGL'
SELECT symbol, SUM(volume) FROM trades GROUP BY symbol

-- UPDATE
UPDATE trades SET price = 15200 WHERE symbol = 1 AND price > 15100

-- DELETE
DELETE FROM trades WHERE symbol = 1 AND price < 15000

-- Storage tiering policy (Hot → Warm → Cold → Drop)
ALTER TABLE trades SET STORAGE POLICY
  HOT 1 HOURS WARM 24 HOURS COLD 30 DAYS DROP 365 DAYS

-- Materialized View (incremental aggregation on ingest)
CREATE MATERIALIZED VIEW ohlcv_5min AS
  SELECT symbol, xbar(timestamp, 300000000000) AS bar,
         first(price) AS open, max(price) AS high,
         min(price) AS low, last(price) AS close,
         sum(volume) AS vol
  FROM trades
  GROUP BY symbol, xbar(timestamp, 300000000000)

-- Query the materialized view (reads pre-computed results, not raw ticks)
SELECT * FROM ohlcv_5min WHERE symbol = 1

-- Drop materialized view
DROP MATERIALIZED VIEW ohlcv_5min
```

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

## Project Structure

```
zeptodb/
├── include/zeptodb/
│   ├── storage/        # Arena, ColumnStore, PartitionManager, HDB, ParquetReader/Writer
│   ├── ingestion/      # RingBuffer, TickPlant, WAL
│   ├── execution/      # VectorizedEngine, JIT, JOIN, WindowFunctions, QueryScheduler
│   │                   # ParallelScan (Partition/CHUNKED), ResourceIsolation
│   ├── sql/            # Tokenizer, Parser, AST, Executor (+ CancellationToken)
│   ├── server/         # HttpServer (ClickHouse compatible, Admin API, query timeout)
│   ├── auth/           # RBAC, ApiKeyStore, JwtValidator, AuthManager, RateLimiter,
│   │                   # AuditBuffer, CancellationToken, QueryTracker, SecretsProvider
│   ├── feeds/          # FIX, NASDAQ ITCH, Binance feed handlers
│   ├── migration/      # kdb+/ClickHouse/DuckDB/TimescaleDB migrators
│   ├── core/           # ZeptoPipeline (E2E integration, multi-threaded drain)
│   ├── cluster/        # Transport, PartitionRouter, HealthMonitor, QueryCoordinator
│   │                   # WalReplicator, FailoverManager, CoordinatorHA
│   │                   # SnapshotCoordinator, ComputeNode, PartitionMigrator
│   └── transpiler/     # Python binding (pybind11)
├── src/                # Implementation
├── tests/
│   ├── unit/           # Google Test (619+ tests)
│   ├── feeds/          # Feed handler tests (37 tests)
│   ├── migration/      # Migration toolkit tests (70 tests)
│   ├── python/         # Python ecosystem tests (208 tests)
│   └── bench/          # Benchmarks
├── deploy/
│   ├── docker/         # Dockerfile
│   ├── k8s/            # Kubernetes deployment YAML
│   ├── helm/zeptodb/   # Helm chart (production)
│   ├── monitoring/     # Grafana dashboard + Prometheus alerts
│   └── scripts/        # tune_bare_metal, backup, restore, systemd
├── tools/
│   └── zepto-migrate.cpp  # Migration CLI
└── docs/
    ├── design/         # Architecture + Layer design docs
    ├── business/       # Business strategy
    ├── deployment/     # Deployment guides
    ├── operations/     # Operations guide
    ├── feeds/          # Feed handler guides
    ├── requirements/   # PRD/SRS
    ├── bench/          # Benchmark results
    └── devlog/         # Development log (000~022)
```

---

## Target Markets

### Full Lifecycle: Research → Production → Compliance

| Phase | Use Cases | kdb+ Replacement |
|-------|-----------|-----------------|
| **Quant Research** | Backtesting, EMA, Python DSL, Jupyter zero-copy | **90%** |
| **Production Trading** | Tick processing, ASOF JOIN, xbar, 5.52M ticks/sec | **95%** |
| **Risk/Compliance** | Position calc, audit log, Grafana dashboards | **95%** |

### Beyond Finance: Same Engine, New Industries

| Domain | Why ZeptoDB | Key Feature |
|--------|------------|-------------|
| **Crypto/DeFi** | 24/7 multi-exchange, orderbook streaming | Kafka consumer, VWAP |
| **IoT/Smart Factory** | 10KHz sensors, predictive maintenance | 5.52M events/sec, DELTA |
| **Autonomous Vehicles** | Sensor fusion, driving log replay | ASOF JOIN, Parquet HDB |
| **Observability** | High-cardinality metrics, log analytics | SQL + Grafana, TTL + S3 |

---

## Enterprise Security

ZeptoDB ships with a complete enterprise security layer — all contract-blocker requirements
are implemented and tested.

| Feature | Details |
|---------|---------|
| **TLS/HTTPS** | OpenSSL 3.2, cert/key PEM, port 8443 |
| **Authentication** | API Key (Bearer, SHA256-hashed) + JWT/OIDC (HS256/RS256) |
| **Authorization** | RBAC: 5 roles (admin/writer/reader/analyst/metrics) + symbol-level ACL |
| **Rate Limiting** | Token bucket per-identity + per-IP, configurable burst |
| **Admin REST API** | Key CRUD, query list/kill, audit log, version — all behind ADMIN role |
| **Query Timeout** | `set_query_timeout_ms(ms)` — auto-cancel via `CancellationToken` |
| **Query Kill** | `DELETE /admin/queries/:id` — cancels running query at partition boundary |
| **Secrets Management** | Vault KV v2 → K8s file secrets → env var (priority chain) |
| **Audit Log** | spdlog file (7-year retention) + in-memory ring buffer → `GET /admin/audit` |
| **Compliance** | SOC2, EMIR, MiFID II, ISO 27001 — see `docs/design/layer5_security_auth.md` |

```bash
# Secure server startup
AuthManager::Config auth;
auth.api_keys_file   = "keys.txt";
auth.jwt_enabled     = true;
auth.jwt.hs256_secret = secrets->get("JWT_SECRET");
auth.rate_limit.requests_per_minute = 1000;

TlsConfig tls;
tls.enabled   = true;
tls.cert_path = "/etc/zeptodb/cert.pem";
tls.key_path  = "/etc/zeptodb/key.pem";

HttpServer server(executor, 8443, tls, std::make_shared<AuthManager>(auth));
server.set_query_timeout_ms(30000);
server.start_async();

# Admin: create a key
curl -X POST https://zepto:8443/admin/keys \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -d '{"name":"algo-service","role":"writer"}'

# Admin: list running queries
curl https://zepto:8443/admin/queries -H "Authorization: Bearer $ADMIN_KEY"

# Admin: kill a query
curl -X DELETE https://zepto:8443/admin/queries/q_a1b2c3 \
  -H "Authorization: Bearer $ADMIN_KEY"
```

---

## Production Ready

### Deployment Options
- **Bare-metal (recommended)**: HFT latency consistency, `deploy/scripts/tune_bare_metal.sh` auto-tuning
- **Cloud**: Docker + Kubernetes, AWS Graviton4 optimized
- **Helm chart**: `helm install zeptodb ./deploy/helm/zeptodb` — PDB, HPA, rolling upgrades

### Monitoring & Observability
- Prometheus `/metrics` (OpenMetrics format)
- Grafana dashboard + 9 alert rules
- `/health`, `/ready`, `/metrics` endpoints
- Structured JSON access log (every request) with `X-Request-Id` correlation
- Slow query log (>100ms) with query_id tracing
- Audit log (SOC2/EMIR/MiFID II) — 7-year retention
- Design: `docs/design/logging_observability.md`

### Operations Automation
- `deploy/scripts/backup.sh` — HDB/WAL/Config backup + S3
- `deploy/scripts/restore.sh` — Disaster recovery
- `deploy/scripts/install_service.sh` — One-step systemd service installation

**Detailed guides:**
- Deployment: `docs/deployment/PRODUCTION_DEPLOYMENT.md`
- Operations: `docs/operations/PRODUCTION_OPERATIONS.md`
- Kubernetes Ops: `docs/operations/KUBERNETES_OPERATIONS.md`
- K8s Failure Scenarios: `docs/operations/KUBERNETES_FAILURE_SCENARIOS.md`
- Rolling Upgrade: `docs/ops/rolling_upgrade.md`
- Feed Handlers: `docs/feeds/FEED_HANDLER_GUIDE.md`

---

## Development Phases

### Completed
See [`COMPLETED.md`](COMPLETED.md) for the full list of 35+ completed features with client compatibility matrix.

### In Progress
- [ ] Distributed query scheduler (DistributedQueryScheduler + UCX)

### Backlog
See [`BACKLOG.md`](docs/BACKLOG.md) for the full backlog.

## License

Proprietary — All rights reserved.
