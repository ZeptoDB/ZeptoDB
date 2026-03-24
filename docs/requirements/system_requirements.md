# ZeptoDB System Requirements

*Version 2.5 — Last updated: 2026-03-24 (Storage Tiering, Materialized View, Multi-Tenancy)*

---

## Functional Requirements

### FR-1: Time-Series Ingestion
- FR-1.1: Ingest > 1M ticks/sec sustained (**achieved: 5.52M/sec**)
- FR-1.2: Lock-free MPMC ring buffer (no blocking in hot path)
- FR-1.3: WAL for durability across restarts
- FR-1.4: Feed handler support: FIX, NASDAQ ITCH, UDP multicast, WebSocket

### FR-2: Query Engine
- FR-2.1: Standard SQL (SELECT / WHERE / GROUP BY / ORDER BY / LIMIT)
- FR-2.2: ASOF JOIN (time-series join, O(n) two-pointer)
- FR-2.3: Hash JOIN (equi-join)
- FR-2.4: LEFT JOIN with NULL sentinel
- FR-2.5: Window JOIN (wj) — time-window range join
- FR-2.6: Window functions: SUM/AVG/MIN/MAX/ROW_NUMBER/RANK/DENSE_RANK/LAG/LEAD
- FR-2.7: Financial functions: xbar, EMA, VWAP, DELTA, RATIO, FIRST, LAST
- FR-2.8: Parallel query execution (scatter/gather, N threads)
- FR-2.9: SQL DML — INSERT INTO (single/multi-row, column list), UPDATE SET WHERE, DELETE FROM WHERE
- FR-2.10: SQL DDL — CREATE TABLE, DROP TABLE, ALTER TABLE (ADD/DROP COLUMN, SET TTL, SET ATTRIBUTE, SET STORAGE POLICY)
- FR-2.11: SQL subqueries / CTE — WITH clause, FROM subquery, chained CTEs
- FR-2.12: Materialized View — CREATE/DROP MATERIALIZED VIEW, incremental aggregation on ingest (SUM/COUNT/MIN/MAX/FIRST/LAST + xbar)

### FR-3: Storage
- FR-3.1: Column-oriented in-memory store (typed arrays)
- FR-3.2: HDB disk persistence with LZ4 compression (4.8 GB/s flush)
- FR-3.3: Partition-by-symbol routing (2ns overhead)
- FR-3.4: Arena allocator — no malloc in hot path
- FR-3.5: Parquet output (SNAPPY/ZSTD/LZ4_RAW) — Arrow C++ API, directly queryable from DuckDB/Polars/Spark
- FR-3.6: S3 Sink — Parquet/Binary → S3 async upload, MinIO compatible, hive partitioning (`{symbol}/{hour}.parquet`)
- FR-3.7: Storage tiering — Hot (memory) → Warm (SSD/LZ4) → Cold (S3/Parquet) → Drop, ALTER TABLE SET STORAGE POLICY

### FR-4: APIs
- FR-4.1: HTTP API on port 8123 (ClickHouse wire protocol compatible)
  - `POST /` — SQL query execution (JSON response) — SELECT, DDL, INSERT
  - `GET /ping` — ClickHouse-compatible health check
  - `GET /health` — Kubernetes liveness probe
  - `GET /ready` — Kubernetes readiness probe
  - `GET /stats` — Pipeline statistics
  - `GET /metrics` — Prometheus OpenMetrics format
- FR-4.2: Python binding (pybind11, zero-copy numpy/Arrow, < 1μs)
- FR-4.3: C++ API (direct struct access, lowest latency)
- FR-4.4: Python Ecosystem — `zepto_py` package ✅
  - `from_pandas(df, pipeline)` — vectorized numpy batch ingest
  - `from_polars(df, pipeline)` — zero-copy Arrow buffer → ingest_batch
  - `from_polars_arrow(df, pipeline)` — polars.to_arrow() → ingest_batch
  - `from_arrow(table, pipeline)` — Arrow Table → ingest_batch
  - `ArrowSession` — zero-copy ingest/export, DuckDB registration, RecordBatchReader
  - `StreamingSession` — batch ingest with progress callbacks, error modes, generator support
  - `ApexConnection` — HTTP client returning pandas/polars/numpy DataFrames
  - `query_to_pandas()` / `query_to_polars()` — SQL JSON result → DataFrame
  - Supports: numpy, pandas, polars, pyarrow, duckdb interoperability
  - Null-safe Arrow extraction via `_arrow_col_to_numpy()` (fills null → 0)

### FR-5: Distributed
- FR-5.1: UCX transport (RDMA/InfiniBand/TCP)
- FR-5.2: Shared memory transport (same-host, zero-copy IPC)
- FR-5.3: Consistent hash partition router
- FR-5.4: Health monitoring + cluster management

### FR-6: Migration Toolkit
- FR-6.1: kdb+ q language → APEX SQL transpiler (lexer/parser/transformer)
- FR-6.2: kdb+ HDB splayed table loader (mmap, zero-copy)
- FR-6.3: ClickHouse schema generator (MergeTree, codecs, LowCardinality)
- FR-6.4: ClickHouse query translator (xbar, ASOF JOIN, argMin/argMax)
- FR-6.5: DuckDB/Parquet exporter (SNAPPY/ZSTD, hive partitioning)
- FR-6.6: TimescaleDB hypertable DDL + continuous aggregates generator
- FR-6.7: `zepto-migrate` CLI (5 modes: query/hdb/clickhouse/duckdb/timescaledb)

### FR-7: Production Operations
- FR-7.1: Prometheus /metrics endpoint (OpenMetrics format)
- FR-7.2: /health and /ready liveness/readiness endpoints
- FR-7.3: Grafana dashboard + 9 alert rules
- FR-7.4: Automated backup (HDB/WAL/Config → S3)
- FR-7.5: systemd service with auto-restart
- FR-7.6: Kubernetes deployment (HPA, PVC, LoadBalancer)

### FR-8: Security & Access Control ✅ (2026-03-22)
- FR-8.1: **TLS/HTTPS** — OpenSSL 3.2, cert/key PEM, port 8443; falls back to HTTP if not compiled in
- FR-8.2: **API Key Authentication** — `zepto_<256-bit>` format, SHA256-hashed store, CRUD lifecycle, file-persisted
- FR-8.3: **JWT/SSO Authentication** — HS256 (shared secret) + RS256 (PEM public key); OIDC-compatible
  - Claims: `sub`, `exp`, `iss`, `aud`, `zepto_role`, `zepto_symbols`
  - Supported IdPs: Okta, Azure AD, Google Workspace, Auth0 (any OIDC RS256 provider)
- FR-8.4: **RBAC** — 5 roles: `admin`, `writer`, `reader`, `analyst`, `metrics`
  - Permission bitmask: READ / WRITE / ADMIN / METRICS
  - Symbol-level ACL per identity (allowlist; empty = unrestricted)
- FR-8.5: **Auth Middleware** — `set_pre_routing_handler` intercepts all requests before route dispatch
  - JWT priority over API key when both presented
  - Public paths exempt: `/ping`, `/health`, `/ready`
  - Returns `401 Unauthorized` (no/invalid credentials) or `403 Forbidden` (insufficient role)
- FR-8.6: **Audit Logging** — per-request identity + action + IP, spdlog to configurable file
  - Format: `[timestamp] [AUDIT] subject=<id> role=<role> action="<method> <path>" detail=<auth-method> from=<ip>`
  - Meets EMIR / MiFID II / SOC2 / ISO 27001 audit trail requirements
- FR-8.7: **Enterprise Governance** — see `docs/design/layer5_security_auth.md` for:
  - Key rotation policy (90-day), access review cadence (monthly/quarterly)
  - Separation of duties (create/revoke keys: admin only)
  - Incident response procedures (key compromise, JWT secret compromise)
  - Multi-tenant symbol isolation model
  - Security hardening checklist (network, key management, operational)
- FR-8.8: **Multi-Tenancy** — TenantManager per-tenant resource quotas (concurrent queries, memory), table namespace isolation, usage tracking, AuthContext tenant_id binding

---

## Non-Functional Requirements

### NFR-1: Latency
| Operation | Requirement | Achieved |
|---|---|---|
| Tick ingest (p99) | < 1μs | < 200ns |
| Filter 1M rows | < 500μs | 272μs |
| VWAP 1M rows | < 1ms | 532μs |
| SQL parse | < 10μs | 1.5–4.5μs |
| Python zero-copy | < 1μs | 522ns |
| Partition routing | < 10ns | 2ns |

### NFR-2: Throughput
- Ingest: > 1M ticks/sec sustained (**5.52M/sec**)
- HDB flush: > 1 GB/s (**4.8 GB/s**)
- Query parallelism: > 3x speedup @ 8 threads (**3.48x**)

### NFR-3: Reliability
- No data loss on graceful shutdown (WAL)
- Health endpoint for load balancer integration
- Automated daily backup with S3 upload

### NFR-6: Security
- All data in transit encrypted (TLS 1.2+ mandatory in production)
- No plaintext API keys stored at rest (SHA256 hash only)
- Authentication latency: API key < 2μs, JWT HS256 < 5μs, JWT RS256 < 100μs
- Audit log retention: 7 years (EMIR/MiFID II compliance)
- Zero auth overhead for public health endpoints

### NFR-4: Compatibility
- SQL: ANSI SQL subset (SELECT/WHERE/GROUP BY/JOIN/OVER)
- HTTP: ClickHouse wire protocol (port 8123)
- Python: numpy/Arrow zero-copy; pandas/polars/duckdb interop via zepto_py
- Monitoring: Prometheus + Grafana

### NFR-5: Build & Platform
- C++20, clang-19, LLVM 19
- Linux (Amazon Linux 2023 / Ubuntu 22.04+)
- x86_64 (AVX2/AVX-512) + ARM64 (SVE, roadmap)
- CMake + Ninja build system

---

## Test Requirements

| Suite | Count | Coverage |
|---|---|---|
| Unit tests (core) | 619 | Storage, Execution, SQL, JOIN, Window, Parquet, S3, Security/Auth, Cluster, Scheduler, Kafka, DDL, INSERT |
| Feed handler tests | 21 | FIX parser, ITCH parser, benchmarks |
| Migration tests | 126 | q→SQL, HDB, ClickHouse, DuckDB, TimescaleDB |
| Python ecosystem tests | 208 | from_pandas/polars/arrow, ArrowSession, StreamingSession, ingest_batch |
| **Total** | **974+** | — |

> Python ecosystem suite includes ingest_batch tests (test_ingest_batch.py) which
> overlap with test_arrow_integration.py for ArrowSession paths.
