# kdb+ / KDB-X & ClickHouse Benchmark Reference

> Collected benchmark data for kdb+ and competing databases for comparison with APEX-DB.
> Last updated: 2026-03-22

---

## 1. kdb+ / KDB-X Official Benchmarks

### 1-A. KDB-X vs TSBS Benchmark (KX Official Blog, 2025)

**Source:** [KX Blog — KDB-X vs QuestDB, ClickHouse, TimescaleDB, InfluxDB](https://kx.com/blog/benchmarking-kdb-x-vs-questdb-clickhouse-timescaledb-and-influxdb-with-tsbs/)

**Environment:** 256 cores, 2.2TB RAM (KDB-X limited to 4 threads, 16GB memory)

**Results (low-frequency 1-year data, slowdown multiplier vs KDB-X):**

| Query | QuestDB | InfluxDB | TimescaleDB | ClickHouse |
|---|---|---|---|---|
| single-groupby-1-1-1 | 16.2x | 48.1x | 119.9x | **9,791.9x** |
| single-groupby-1-1-12 | 25.9x | 39.7x | 528.2x | 5,741.8x |
| cpu-max-all-1 | 14.1x | 23.3x | 127.2x | 425.9x |
| high-cpu-1 | 8.3x | 2.4x | 519.3x | 443.7x |
| double-groupby-1 | 0.7x | 11.9x | 4.9x | 21.0x |
| lastpoint | 0.8x | 7,069.8x | 17.4x | 112.3x |
| **Geometric mean** | **4.2x** | **53.1x** | **25.5x** | **161.3x** |

**Key findings:**
- KDB-X ranked **1st in 58 of 64 scenarios** (using only 4 threads)
- ClickHouse is **10,000x slower** on certain queries
- QuestDB is the most competitive but still 4.2x slower on average
- KDB-X's q language vector operations remain industry-leading

### 1-B. kdb+ Tick Plant Profiling (KX Official Whitepaper)

**Source:** [kdb+tick profiling](https://code.kx.com/q/wp/tick-profiling/)

**Environment:** 64-bit Linux, 8 CPU, kdb+ 3.1

**Tickerplant throughput (microseconds):**

| rows/update | rows/sec | TP log write | TP publish | RDB receive | RDB insert | TP CPU |
|---|---|---|---|---|---|---|
| 1 | 10,000 | 14μs | 3μs | 71μs | 4μs | 31% |
| 10 | 100,000 | 15μs | 4μs | 82μs | 7μs | 32% |
| 100 | 100,000 | 32μs | 6μs | 103μs | 46μs | 6% |
| 100 | 500,000 | 28μs | 6μs | 105μs | 42μs | 32% |

**Key findings:**
- Single-row dispatch: CPU 100% at ~30K rows/sec
- 10-row batch: 100K rows/sec (vector operation efficiency)
- 100-row batch: 500K rows/sec (CPU 32%)
- **kdb+ tickerplant theoretical maximum: ~2–5M rows/sec** (batch size and hardware dependent)

### 1-C. kdb+ Query Optimization Performance (KX Official Whitepaper)

**Source:** [kdb+ query scaling](https://code.kx.com/q/wp/query-scaling/)

**Environment:** kdb+ 3.1, 2M rows in-memory / 10M rows partitioned

| Operation | In-memory (2M rows) | Partitioned (10M rows) |
|---|---|---|
| select by sym | **20ms** | **78ms** |
| select last per sym | 51ms | 345ms |
| select first per sym | 12ms | - |
| max aggregation per sym | 28ms | - |
| filter (3 syms, lambda each) | - | **15ms** |
| filter (3 syms, in operator) | - | 25ms |

**Note:** These figures represent full query latency, not isolated vector operation latency.

---

## 2. kdb+ vs APEX-DB Direct Comparison

### Ingestion Comparison

| Metric | kdb+ tickerplant | APEX-DB | Notes |
|---|---|---|---|
| Single-row throughput | ~30K/sec (CPU 100%) | 4.97M/sec | **165x advantage** (kdb+ has q interpreter overhead) |
| Batch 100 rows | ~500K/sec (CPU 32%) | 5.52M/sec | **11x advantage** |
| Theoretical maximum | ~2–5M/sec | 5.52M/sec | **Equivalent to superior** |

> **Note:** kdb+ tickerplant figures are based on 2014 hardware. Modern hardware may yield higher.
> KDB-X (2025) likely improved significantly with multi-thread support, but no public ingestion benchmarks confirmed.

### Query Comparison (In-Memory OLAP)

| Operation | kdb+ (estimated) | APEX-DB Scalar | APEX-DB SIMD | Gap |
|---|---|---|---|---|
| VWAP 1M rows | ~200–500μs | 649μs | 532μs | ⚠️ 1.1–2.5x behind |
| sum 1M rows | ~50–150μs | 267μs | 264μs | ⚠️ ~2x behind |
| filter 1M rows (bitmask) | ~100–300μs | — | **272μs** | ✅ competitive |

**Root cause analysis:**
1. **kdb+ q is natively column-vector-oriented** — 30+ years of interpreter optimization
2. **APEX-DB filter upgraded to bitmask** — 11x speedup over SelectionVector, now in kdb+ range
3. **APEX-DB sum is memory-bandwidth bound** — auto-vectorized, limited headroom
4. **APEX-DB partition overhead** — index lookup vs. single-partition direct scan

---

## 3. ClickHouse Benchmark Reference

### ClickBench (Official Benchmark Framework)

**Source:** [benchmark.clickhouse.com](https://benchmark.clickhouse.com/)

ClickHouse specializes in **disk-based OLAP**. Direct comparison with in-memory real-time HFT is not appropriate, but for reference:

**ClickHouse characteristics:**
- Vectorized execution engine (similar architecture to APEX-DB Layer 3)
- MergeTree storage (inspiration for APEX-DB DMMT)
- Disk-based → 10–1000x slower query latency vs. in-memory
- TSBS benchmark: **161x slower geometric mean vs. kdb+**

**ClickHouse strengths:**
- Large batch ingestion (hundreds of millions of rows/sec)
- Disk-based compression + scan (cost-efficient)
- SQL compatibility, rich ecosystem

**ClickHouse weaknesses:**
- Real-time μs latency queries (our target)
- Per-tick streaming ingestion
- In-memory performance (kdb+, APEX-DB domain)

### APEX-DB vs ClickHouse Positioning

| Dimension | ClickHouse | APEX-DB |
|---|---|---|
| Target | General OLAP | HFT real-time |
| Storage | Disk-based | In-memory (CXL roadmap) |
| Latency | ms–sec | **μs** |
| Ingestion | Batch-optimized | Streaming-optimized |
| Query language | SQL | SQL + C++ / Python DSL |
| Competitors | Snowflake, BigQuery | kdb+, custom HFT systems |

---

## 4. Conclusions and APEX-DB Target Metrics

### Final Targets (Post Phase B Optimization)

| Metric | kdb+ (latest estimate) | APEX-DB Target | Strategy |
|---|---|---|---|
| Ingestion | ~5M/sec | **10M+/sec** | RDMA direct write, sharded drain |
| VWAP 1M | ~200–500μs | **<200μs** | SIMD fused pipeline |
| sum 1M | ~50–150μs | **<100μs** | Multi-column fusion, prefetch |
| filter 1M | ~100–300μs | **<200μs** | Bitmask SIMD, branch-free ✅ |
| Dynamic query | q interpreter | **JIT SIMD** | LLVM AVX2/512 emit |
| Python integration | PyKX (IPC-based) | **Zero-copy** | pybind11 + Arrow ✅ |

### TSBS Targets
- Single query: **≥ 1:1 parity with kdb+**
- Multi-threaded query: **2–4x advantage over kdb+** (breaking kdb+'s single-thread constraint)
