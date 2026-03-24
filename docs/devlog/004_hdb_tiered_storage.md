# 004: HDB Tiered Storage — Design, Implementation, Benchmarks

**Date:** 2026-03-22
**Phase:** A — Historical Database (HDB) Tiered Storage
**Status:** Complete

---

## 1. Design Decisions

### 1.1 File Format

Inspired by kdb+'s splayed table approach, we adopted a structure that stores per-column binary files separately for each partition.

```
hdb_data/
  {symbol_id}/
    {hour_epoch}/
      timestamp.bin   <- each column is an independent file
      price.bin
      volume.bin
      msg_type.bin
```

**Per-file structure:**
```
[HDBFileHeader 32 bytes] [data (raw or LZ4 compressed)]
```

**HDBFileHeader (32 bytes):**
| Field | Size | Description |
|-------|------|-------------|
| magic | 5B | `APEXH` |
| version | 1B | current v1 |
| col_type | 1B | ColumnType enum |
| compression | 1B | 0=None, 1=LZ4 |
| row_count | 8B | number of rows |
| data_size | 8B | actual data bytes (compressed size when compressed) |
| uncompressed_size | 8B | original size |

**Design rationale:**
- Per-column separation: only needed columns are mapped during mmap (memory efficiency)
- 32-byte fixed header: half a cache line, minimal parse overhead
- Magic bytes + version: file integrity verification and backward compatibility
- Same "splayed" philosophy as kdb+ but with a custom binary format

### 1.2 LZ4 Compression Strategy

- Uses **LZ4 block compression** (simple block, not frame)
- Automatically stores raw if compressed result is larger than original (prevents inefficient compression)
- Compile-time detection via `__has_include(<lz4.h>)` when `lz4-devel` is unavailable; passthrough mode
- Controlled via `APEX_USE_LZ4` CMake option

**Compression performance (1M rows):**
- Compression ratio: 0.31 (69% savings)
- Compression throughput: ~1,128 MB/sec (uncompressed basis)
- LZ4 is very effective for time-series data (sequential timestamps, similar prices)

### 1.3 mmap Read Strategy

- Uses `mmap(MAP_PRIVATE)` + `madvise(MADV_SEQUENTIAL)`
- Sequential access hint optimizes kernel prefetch
- Uncompressed data: zero-copy direct pointer return
- LZ4 compressed data: decompress to buffer then return pointer (copy required)
- RAII pattern: `MappedColumn` destructor automatically calls `munmap` + `close`

### 1.4 N-4 Storage Modes

Per requirements document N-4, three modes are supported:

| Mode | Description | Query Target |
|------|-------------|--------------|
| `PURE_IN_MEMORY` | For extreme HFT tick processing | RDB only |
| `TIERED` | RDB (today) + HDB (history) async merge | RDB + HDB |
| `PURE_ON_DISK` | For backtesting/deep learning feature generation | HDB only |

### 1.5 FlushManager Async Strategy

- Background thread periodically checks (default 1s) for SEALED partitions
- SEALED -> FLUSHING -> FLUSHED state transitions (single thread, no locking needed)
- After flush, memory reclaimed via `ArenaAllocator::reset()`
- No mutex on hot path (ingestion) — writes only to ACTIVE partition

---

## 2. Benchmark Results

### 2.1 HDB Flush Throughput (NVMe Write)

| Data Size | Throughput | Notes |
|-----------|-----------|-------|
| 100K rows (2.7 MB) | **3,557 MB/sec** | Small I/O |
| 500K rows (13.4 MB) | **4,748 MB/sec** | Medium batch |
| 1M rows (26.7 MB) | **4,785 MB/sec** | Optimal batch |
| 5M rows (118 MB) | **3,804 MB/sec** | Large I/O |
| 1M rows LZ4 (ratio=0.31) | **1,128 MB/sec** | Includes compression overhead |

Uncompressed mode achieves **~4.7 GB/sec** — approaching NVMe SSD theoretical bandwidth (3-7 GB/sec).
LZ4 compression: **69% disk savings**, throughput ~4x lower due to compression CPU cost.

### 2.2 mmap Read Throughput

| Data Size | Throughput | Notes |
|-----------|-----------|-------|
| 100K rows (2.7 MB) | **79 GB/sec** | L3 cache hit |
| 500K rows (13.4 MB) | **382 GB/sec** | Kernel prefetch |
| 1M rows (26.7 MB) | **774 GB/sec** | Memory bandwidth |
| 5M rows (118 MB) | **2,943 GB/sec** | Page cache hit |

mmap reads are extremely fast thanks to OS page cache. Actual NVMe cold reads may drop to a few GB/sec.

### 2.3 Query Latency Comparison

| Mode | Query | Latency | Notes |
|------|-------|---------|-------|
| Pure In-Memory | COUNT 1M rows | **1.11 us** | Optimal path |
| Tiered (HDB mmap) | COUNT 1M rows | **677.60 us** | Reading from disk |
| Pure In-Memory | VWAP 1M rows | **44.84 us** | Includes multiply+sum ops |

In-Memory COUNT is **~1us** — trivially returns `size()`.
Tiered COUNT requires mmap + sequential scan -> **~678us** (~600x slower).
VWAP is compute-intensive so even in-memory takes **~45us**.

---

## 3. kdb+ HDB Comparison

### kdb+ Approach
- **Splayed table:** table as directory, each column as binary file
- **Partitioned table:** `date/sym/col` structure, partitioned by date
- **Memory-mapped:** accessed via mmap on demand
- **Enumeration:** symbol strings interned as integer IDs (sym file)

### ZeptoDB Approach
- **Per-column binary:** same philosophy as kdb+
- **Hour-level partitioning:** finer than kdb+ (day-level) — better suited for HFT data
- **LZ4 compression:** not available by default in kdb+ (requires separate processing)
- **32-byte header:** kdb+ uses 8-byte header (type+attribute+length only)
- **RAII mmap:** kdb+ has no GC (q language uses reference counting)

### Performance Comparison (estimated)

| Item | kdb+ | ZeptoDB | Notes |
|------|------|---------|-------|
| Flush | ~1-2 GB/sec | **~4.7 GB/sec** | Direct write() + custom format |
| mmap read | Similar | Similar | Depends on OS page cache |
| Compression | Separate gzip | **LZ4 built-in** | Real-time compress/decompress |
| Partition unit | Day | **Hour** | Finer granularity for HFT |

---

## 4. Implementation Files

### New Files
| File | Description |
|------|-------------|
| `include/zeptodb/storage/hdb_writer.h` | HDB columnar binary Writer (header + LZ4) |
| `src/storage/hdb_writer.cpp` | HDB Writer implementation |
| `include/zeptodb/storage/hdb_reader.h` | HDB mmap Reader + MappedColumn RAII |
| `src/storage/hdb_reader.cpp` | HDB Reader implementation |
| `include/zeptodb/storage/flush_manager.h` | Background RDB->HDB flush manager |
| `src/storage/flush_manager.cpp` | FlushManager implementation |
| `tests/unit/test_hdb.cpp` | HDB unit tests (10 tests) |
| `tests/bench/bench_hdb.cpp` | HDB benchmarks (flush/read/query) |

### Modified Files
| File | Changes |
|------|---------|
| `include/zeptodb/storage/partition_manager.h` | Added `get_sealed_partitions()`, `reclaim_arena()` |
| `src/storage/partition_manager.cpp` | Added `get_sealed_partitions()` implementation |
| `include/zeptodb/core/pipeline.h` | `StorageMode` enum, HDB component integration |
| `src/core/pipeline.cpp` | Tiered query logic, HDB initialization |
| `CMakeLists.txt` | LZ4 optional dependency, HDB source/bench additions |
| `tests/CMakeLists.txt` | Added test_hdb.cpp + zepto_core link |

---

## 5. TODO (Next Steps)

- [ ] S3 offloading (NVMe -> S3 async transfer)
- [ ] Partition auto-deletion policy (TTL)
- [ ] Distributed HDB — CXL 3.0 inter-node partition sharing
- [ ] LZ4 Frame mode switch (streaming compression)
- [ ] Index file (min/max timestamp per partition -> partition pruning)
