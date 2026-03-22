# APEX-DB Benchmark Results — Phase E (End-to-End MVP)
# Run date: 2026-03-21
# Environment: Intel Xeon 6975P-C, 8 vCPU, 30GB RAM, Amazon Linux 2023

---

## BENCH 1: Ingestion Throughput

| Batch size | Throughput | Drops |
|---|---|---|
| batch=1 | **4.97M ticks/sec** | 0 |
| batch=64 | **5.44M ticks/sec** | 0 |
| batch=512 | **5.52M ticks/sec** | 0 |
| batch=4096 | **5.51M ticks/sec** | 0 |
| batch=65535 | **5.52M ticks/sec** | 14 (queue saturation) |

**Peak: 5.52M ticks/sec** (saturates at batch 512–65535)

---

## BENCH 2: Query Latency

### 100K rows
| Query type | p50 | p99 | p999 |
|---|---|---|---|
| VWAP | 52.5μs | 60.6μs | 80.1μs |
| Filter+Sum | 75.5μs | 81.0μs | 110.5μs |
| Count | 0.1μs | 0.1μs | 0.3μs |

### 1M rows
| Query type | p50 | p99 | p999 |
|---|---|---|---|
| VWAP | 637.7μs | 663.5μs | 795.7μs |
| Filter+Sum | 789.7μs | 810.8μs | 829.9μs |
| Count | 0.1μs | 0.1μs | 0.4μs |

### 5M rows
| Query type | p50 | p99 | p999 |
|---|---|---|---|
| VWAP | 3,496μs | 3,577μs | 7,942μs |
| Filter+Sum | 3,945μs | 4,035μs | 5,246μs |
| Count | 0.1μs | 0.1μs | 0.7μs |

**Throughput: 9M rows → 10ms = ~914M rows/sec (VWAP)**

---

## BENCH 3: Multi-Producer Concurrent Performance

| Threads | Throughput |
|---|---|
| 1 | 1.97M ticks/sec |
| 2 | 1.92M ticks/sec |
| 4 | 1.72M ticks/sec |

> Performance degrades with multiple threads — drain thread bottleneck. **Needs improvement** (see analysis below)

---

## BENCH 5: Large 9M rows VWAP
- Load: 5.01M ticks/sec
- Query p50=10.8ms, p99=15ms, p999=16ms
- **Throughput: ~914M rows/sec**

---

## kdb+ Comparison Reference

### kdb+ Published Benchmarks (reference values)
| Metric | kdb+ (reference) | APEX-DB (current) | Status |
|---|---|---|---|
| Ingestion throughput | ~2–5M ticks/sec (single tickerplant) | **5.52M ticks/sec** | ✅ Equivalent to superior |
| VWAP 1M rows | ~500–800μs | **637μs** | ✅ Equivalent |
| Full scan 1M rows | ~200–400μs | 790μs (filter+sum) | ⚠️ Behind (pre-bitmask) |
| Multi-thread ingestion | kdb+ single-thread model | 1.72M (4 threads) | ⚠️ Needs improvement |

> **Note**: kdb+ benchmarks are estimates based on published academic/industry data.
> Direct comparison with actual kdb+ (real-time tick, dedicated hardware) to be conducted later.

---

## Analysis and Improvement Points

### What is working well
1. **Single-thread ingestion 5.52M/sec** — target achieved
2. **Count query O(1)** — index-based, ~0.1μs level
3. **VWAP throughput 900M rows/sec** — approaching memory bandwidth limit

### Needs improvement
1. **Throughput drop with multiple producers** (4 threads → 1.72M/sec)
   - Cause: drain thread with single mutex + single consumer
   - Fix: sharded drain threads (per-symbol separation)

2. **Queue saturation (drops)** — occurs above 65K batch
   - Cause: Ring buffer 64K capacity vs. production rate
   - Fix: add direct-to-storage path for large batches

3. **VWAP query latency** — scalar implementation, no SIMD
   - Phase B: Highway SIMD vectorization expected **10–30x improvement**

4. **HugePages fallback** — occurs in 9M row test
   - Cause: huge_page not configured in VM environment
   - Fix: set `/proc/sys/vm/nr_hugepages`

---

## POC Conclusion (Phase E)

**End-to-End pipeline POC complete**

- Full flow operational: tick receive → columnar storage → vectorized query
- 5.52M ticks/sec ingestion, 914M rows/sec VWAP throughput
- vs. kdb+: ingestion equivalent to superior; query performance pre-SIMD

**Next: Phase B — Highway SIMD + LLVM JIT for 10x query improvement**
