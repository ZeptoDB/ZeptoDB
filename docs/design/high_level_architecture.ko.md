# ZeptoDB High-Level Architecture

*Last updated: 2026-03-22 (Parquet HDB, S3 Sink 반영)*

## Overview

ZeptoDB is an ultra-low latency in-memory time-series database targeting
HFT, quantitative research, and real-time analytics. It replaces kdb+ at
95% feature parity while offering standard SQL, Python zero-copy, and
open-source licensing.

---

## Layer Architecture

```
┌──────────────────────────────────────────────────────────┐
│  Layer 6: Migration Toolkit                              │
│  zepto-migrate CLI                                        │
│  kdb+ (q→SQL, HDB loader) · ClickHouse (DDL, queries)   │
│  DuckDB (Parquet export) · TimescaleDB (hypertable DDL)  │
├──────────────────────────────────────────────────────────┤
│  Layer 5: Client Interface                               │
│  HTTP API (port 8123, ClickHouse-compatible)             │
│  Python DSL (pybind11, zero-copy numpy/Arrow)            │
│  C++ API (direct, lowest latency)                        │
├──────────────────────────────────────────────────────────┤
│  Layer 4: SQL + Query Planning                           │
│  Recursive descent parser (1.5–4.5μs)                   │
│  AST → Physical plan → Executor                          │
│  QueryScheduler (LocalQueryScheduler / DistributedStub)  │
│  ParallelScanExecutor (PARTITION / CHUNKED / SERIAL)     │
├──────────────────────────────────────────────────────────┤
│  Layer 3: Execution Engine                               │
│  Highway SIMD (AVX2/AVX-512/SVE)                        │
│  LLVM JIT (O3, native codegen)                           │
│  JOIN: ASOF O(n) · Hash · LEFT · RIGHT · FULL · Window (wj) · uj/pj/aj0 │
│  Window Functions: SUM/AVG/MIN/MAX/LAG/LEAD/RANK/EMA     │
│  Financial: xbar · VWAP · DELTA · RATIO · FIRST · LAST  │
│  Parallel: 8-thread scatter/gather (3.48x speedup)       │
├──────────────────────────────────────────────────────────┤
│  Layer 2: Ingestion (Tick Plant)                         │
│  MPMC Ring Buffer (lock-free)                            │
│  WAL (write-ahead log)                                   │
│  Feed Handlers:                                          │
│    FIX Protocol (350ns/msg, zero-copy)                   │
│    NASDAQ ITCH 5.0 (250ns/msg, binary)                   │
│    UDP Multicast (sub-1μs)                               │
│    Binance WebSocket (crypto)                            │
├──────────────────────────────────────────────────────────┤
│  Layer 1: Storage Engine (DMMT)                          │
│  Arena Allocator (no malloc in hot path)                 │
│  Column Store (typed arrays, cache-line aligned)         │
│  PartitionManager (symbol-based sharding)                │
│  HDB (disk persistence, LZ4, 4.8 GB/s flush)            │
│  Parquet Writer (SNAPPY/ZSTD/LZ4_RAW, Arrow C++ API)    │
│  S3 Sink (비동기 업로드, MinIO 호환, hive partitioning)  │
│  CXL Backend (future: CXL memory pooling)                │
├──────────────────────────────────────────────────────────┤
│  Layer 0: Distributed Cluster                            │
│  UCX Transport (RDMA/InfiniBand/TCP)                     │
│  Consistent Hash PartitionRouter (2ns)                   │
│  SharedMemBackend (same-host, zero-copy IPC)             │
│  HealthMonitor + ClusterManager                          │
└──────────────────────────────────────────────────────────┘
```

---

## Key Design Decisions

### 1. Column-Oriented Storage
Time-series data is naturally column-oriented. Storing columns contiguously
maximizes SIMD utilization and cache efficiency. Each column is a typed
array (`float64[]`, `int64[]`) in arena-allocated memory.

### 2. No malloc in Hot Path
The Arena Allocator pre-allocates large memory regions. Tick ingest and
query execution never call `malloc`/`free`, eliminating GC pauses and
allocator contention.

### 3. SIMD-First Execution
All filter/aggregation operations use Highway SIMD with runtime dispatch.
Targets: SSE4.2 → AVX2 → AVX-512 on x86, SVE on ARM Graviton.

### 4. JIT for Complex Queries
LLVM JIT compiles predicate expressions at query time (O3). Reused across
identical query patterns. 2.6x speedup over interpreted execution.

### 5. ASOF JOIN as First-Class Operation
Time-series joins (quotes → trades) require ASOF semantics. Implemented as
O(n) two-pointer scan, matching kdb+ `aj[]` performance.

### 6. Query Scheduler DI Pattern (QueryScheduler DI 패턴)
`QueryScheduler` is an abstract interface with two implementations:
- `LocalQueryScheduler`: thread pool scatter/gather (deployed today)
- `DistributedQueryScheduler`: UCX-based multi-node (roadmap)

Same query planner, swappable transport layer.

---

## Performance Targets (Achieved)

| Operation | Target | Achieved |
|---|---|---|
| Tick ingest | > 1M/sec | **5.52M/sec** |
| Filter 1M rows | < 500μs | **272μs** |
| VWAP 1M rows | < 1ms | **532μs** |
| xbar 1M rows | < 100ms | **24ms** |
| EMA 1M rows | < 10ms | **2.2ms** |
| Parallel GROUP BY (8T) | > 3x | **3.48x** |
| SQL parse | < 10μs | **1.5–4.5μs** |
| Python zero-copy | < 1μs | **522ns** |
| Partition routing | < 10ns | **2ns** |

---

## Migration Toolkit

Customers can migrate from existing systems using `zepto-migrate`:

```
zepto-migrate query       # kdb+ q → APEX SQL transpiler
zepto-migrate hdb         # kdb+ HDB splayed tables → APEX columnar
zepto-migrate clickhouse  # Generate ClickHouse DDL + query translations
zepto-migrate duckdb      # Export APEX data to Parquet (DuckDB compatible)
zepto-migrate timescaledb # Generate TimescaleDB hypertable + continuous aggregates
```

**Conversion coverage:**

| Source | Feature | Translation |
|---|---|---|
| kdb+ | `size wavg price` | `SUM(size*price)/SUM(size)` (VWAP) |
| kdb+ | `xbar[300;time]` | `time_bucket('300 seconds', time)` |
| kdb+ | `aj[\`time\`sym;t1;t2]` | `ASOF JOIN` |
| ClickHouse | `toStartOfMinute()` | Preserved |
| ClickHouse | `argMin(price, ts)` | `FIRST(price)` |
| TimescaleDB | `time_bucket()` | Preserved |
| TimescaleDB | `candlestick_agg()` | OHLCV aggregate |

---

## Roadmap

| Phase | Status | Description |
|---|---|---|
| E — E2E Pipeline | ✅ | 5.52M ticks/sec |
| B — SIMD + JIT | ✅ | BitMask 11x, filter < 300μs |
| A — HDB Storage | ✅ | LZ4, 4.8 GB/s flush |
| D — Python Bridge | ✅ | zero-copy 522ns |
| C — Cluster Transport | ✅ | UCX, 2ns routing |
| SQL/HTTP | ✅ | Parser, ClickHouse API |
| Financial Functions | ✅ | xbar, EMA, wj, ASOF, Window |
| Parallel Query | ✅ | 3.48x @ 8 threads |
| Feed Handlers | ✅ | FIX 350ns, ITCH 250ns |
| Migration Toolkit | ✅ | kdb+/ClickHouse/DuckDB/TimescaleDB |
| Parquet HDB | ✅ | SNAPPY/ZSTD/LZ4_RAW, DuckDB/Polars 직접 쿼리 |
| S3 HDB Flush | ✅ | 비동기 업로드, MinIO 호환, 클라우드 데이터 레이크 |
| Production Ops | ✅ | Monitoring, backup, k8s |
| Distributed Query | 🚧 | DistributedQueryScheduler + UCX |
| Python Ecosystem | 🚧 | Polars/Pandas zero-copy |
| Snowflake/Delta Lake | 📋 | Backlog |
