# APEX-DB System Requirements

*Version 2.0 — Last updated: 2026-03-22*

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

### FR-3: Storage
- FR-3.1: Column-oriented in-memory store (typed arrays)
- FR-3.2: HDB disk persistence with LZ4 compression
- FR-3.3: Partition-by-symbol routing (2ns overhead)
- FR-3.4: Arena allocator — no malloc in hot path

### FR-4: APIs
- FR-4.1: HTTP API on port 8123 (ClickHouse wire protocol compatible)
- FR-4.2: Python binding (pybind11, zero-copy numpy/Arrow, < 1μs)
- FR-4.3: C++ API (direct struct access, lowest latency)

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
- FR-6.7: `apex-migrate` CLI (5 modes: query/hdb/clickhouse/duckdb/timescaledb)

### FR-7: Production Operations
- FR-7.1: Prometheus /metrics endpoint (OpenMetrics format)
- FR-7.2: /health and /ready liveness/readiness endpoints
- FR-7.3: Grafana dashboard + 9 alert rules
- FR-7.4: Automated backup (HDB/WAL/Config → S3)
- FR-7.5: systemd service with auto-restart
- FR-7.6: Kubernetes deployment (HPA, PVC, LoadBalancer)

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

### NFR-4: Compatibility
- SQL: ANSI SQL subset (SELECT/WHERE/GROUP BY/JOIN/OVER)
- HTTP: ClickHouse wire protocol (port 8123)
- Python: numpy/Arrow zero-copy
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
| Unit tests (core) | 151+ | Storage, Execution, SQL, JOIN, Window |
| Feed handler tests | 37 | FIX parser, ITCH parser, benchmarks |
| Migration tests | 70 | q→SQL (20), HDB (15), ClickHouse (18), DuckDB (17), TimescaleDB (18) |
| **Total** | **258+** | — |
