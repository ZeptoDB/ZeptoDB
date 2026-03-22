# Devlog #001: E2E Integration Pipeline

**Date:** 2026-03-21
**Author:** Gosengyi (AI Assistant)

---

## Overview

Implemented an **end-to-end pipeline** and **benchmark framework** connecting Layer 1 (Storage),
Layer 2 (Ingestion), and Layer 3 (Execution).

## Implementation

### Part 1: ApexPipeline (src/core/pipeline.cpp)

Architecture:
```
External → ingest_tick() → TickPlant (MPMC Queue)
                               ↓
                         [drain_thread]
                               ↓
                         store_tick() → PartitionManager → ColumnVectors
                                                              ↓
                         query_vwap() / query_filter_sum() ---↙
                             → VectorizedEngine (vectorized ops)
```

Key design decisions:
- **Separate drain thread:** Decouples ingestion (TickPlant enqueue) from storage (ColumnStore append)
  to minimize ingestion path latency
- **Partition index:** `unordered_map<SymbolId, vector<Partition*>>` for O(1) partition lookup at query time
- **Synchronous drain mode:** `drain_sync()` for tests/benchmarks — immediate storage without background thread

### Part 2: Benchmark Framework (tests/bench/)

Custom chrono-based benchmarks — lightweight implementation without Google Benchmark.
Measurements:
1. Ingestion throughput (by batch size)
2. Query p50/p99/p999 (VWAP, Filter+Sum, Count)
3. E2E latency (ingest → store → query)
4. Multi-threaded concurrent ingestion (1/2/4 threads)
5. Large-scale VWAP (10M rows)

### Part 3: CMake Integration

- Added `apex_core` library (apex_storage + apex_ingestion + apex_execution combined)
- Added `bench_pipeline` executable
- `-O3 -march=native` optimization flags

## Benchmark Highlights

| Item | Result |
|------|------|
| Ingestion | **5.4M ticks/sec** (sync ingest+store) |
| VWAP 1M rows | **740μs** (p50) |
| VWAP 5M rows | **3.7ms** (p50) |
| E2E p99 | **66μs** (at 100K rows) |
| Scan throughput | **~1.3B rows/sec** |

## Issues Found & Improvements

### 1. Arena Memory Waste
ColumnVector's doubling strategy can't free previous blocks from the arena, causing wasted space.
Loading 10M rows: only ~9M stored in a 1.5GB arena (arena exhaustion).

**Fix:** Implement arena-aware realloc or switch to slab allocator

### 2. MPMC Queue Overflow
Burst drops occur with multi-threaded concurrent ingestion into 64K queue.
Problem when drain thread speed < ingestion speed.

**Fix:** Back-pressure mechanism or increase queue size (256K+)

### 3. HugePages Not Enabled
HugePages mmap fails on EC2 → falls back to regular pages.
Increased TLB misses on large data.

**Fix:** `sudo sysctl vm.nr_hugepages=1024`

## Next Steps

- [ ] Apply Highway SIMD (filter_gt_i64, sum_i64, vwap)
- [ ] Parallel partition queries (std::execution::par)
- [ ] WAL integration (crash recovery)
- [ ] Connect q language parser (Layer 4)
- [ ] kdb+ IPC compatibility layer

---

*"MVP complete. Numbers are in — optimization game starts now."*
