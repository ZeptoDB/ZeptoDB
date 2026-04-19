# Layer 1: Storage Engine & Global Shared Memory Pool (DMMT)

This document is the detailed design specification for the **Disaggregated Memory MergeTree (DMMT)** layer, the most critical component of the in-memory DB.

## 1. Architecture Diagram

```mermaid
flowchart TD
    subgraph ComputeNode["Compute Node"]
        CPU(("Ultra-fast CPU cores (SIMD-equipped)"))
        Allocator["Lock-free Custom Allocator\n(kernel-bypass memory allocation)"]
    end
    subgraph MemoryPool["Memory Pool (CXL 3.0 / RDMA)"]
        RDB["RDB (Real-time DB / Hot Data)\nContiguous Apache Arrow Column Vectors"]
        Index["In-Memory Index (Graph CSR format)"]
    end
    subgraph PersistentStorage["Persistent Storage"]
        HDB["HDB (Historical DB / Warm & Cold)\nAsync background merge (NVMe/S3)"]
    end

    CPU <--> Allocator
    Allocator -- "Zero-Copy (tens of nanoseconds)" <--> RDB
    Allocator <--> Index
    RDB -- "Periodic Async Flush" --> HDB
```

## 2. Tech Stack
- **Language:** Modern C++20 (utilizing `std::pmr` polymorphic memory resources)
- **Memory technology:** Linux HugePages (minimize TLB misses), RAM Disk (tmpfs), NUMA-aware allocation.
- **Distributed/network memory:** CXL 3.0, **UCX (Unified Communication X)** abstraction layer (auto-switching from on-premises RoCE v2/InfiniBand to AWS EFA and Azure InfiniBand without hardware dependency).
- **Data format:** Apache Arrow data specification (C Data Interface compatible).

## 3. Layer Requirements
1. **OS Independence (Kernel Bypass):** Must not call OS `Syscall` (e.g., `read`, `write`, `mmap`) for data storage and retrieval; must directly manage custom memory regions at the application level.
2. **Columnar Continuity Guarantee:** Collected tick data must be contiguously allocated in pure array units perfectly aligned to cache-line size, preventing pointer-chasing latency.
3. **Non-Stop Background Merge (MergeTree):** Continuously monitor memory saturation and, upon reaching the threshold, asynchronously export compressed historical data to NVMe storage (HDB) without locking.

## 4. Detailed Design
- **Memory Arena technique:** The RDB pre-allocates a large memory space (Pool) via CXL/RDMA (Arena). Rather than dynamically allocating memory (`malloc`/`new`) for each incoming tick, the arena's empty pointer is atomically updated (bump pointer), making allocation cost zero.
- **Partitioning:** Data is divided into partition chunks keyed by `(table_id, symbol_id, hour_epoch)`. `table_id` (uint16_t) is assigned by `SchemaRegistry` on `CREATE TABLE` (0 is reserved for the legacy single-table path), so every table has its own partition space and `SELECT * FROM empty_table` never returns rows from other tables. Each chunk becomes a target for HDB flush when it transitions to read-only state. See devlog 082 (initial design) and devlogs 083–085 (full-sweep closure: HDB v2 header, `_schema.json` durability, table-aware ingress/cluster routing, strict SQL fallback) for the implementation.

## 5. Partition Attribute Hints (kdb+ `s#`/`g#`/`p#` equivalent)

ZeptoDB supports per-column attribute hints that allow the query executor to use index structures instead of linear scans.

### 5.1 `s#` — Sorted Attribute

**Status:** ✅ Implemented (2026-03-23)

Marks a column as monotonically non-decreasing (append-only guarantee). Enables O(log n) binary search instead of O(n) linear scan for range predicates.

**API:**
```cpp
Partition* part = ...;
part->set_sorted("price");               // mark as sorted
bool ok = part->is_sorted("price");      // query attribute

auto [begin, end] = part->sorted_range("price", 15000, 16000);
// begin/end are row indices [begin, end) satisfying 15000 <= price <= 16000
```

**Implementation:** `include/zeptodb/storage/partition_manager.h`
- `sorted_columns_` (`unordered_set<string>`) stored per partition
- `sorted_range()` uses `std::lower_bound` / `std::upper_bound` on the column span

**Executor Integration:** `src/sql/executor.cpp` — `extract_sorted_col_range()`
- Scans WHERE clause for BETWEEN / `>=` / `>` / `<=` / `<` / `=` on sorted columns
- Fires in `exec_simple_select` (SELECT without aggregation)
- Falls through to `eval_where_ranged(stmt, *part, r_begin, r_end)` — only scans the pruned range

**SQL example:**
```sql
-- With s# on price column: O(log n) binary search, not O(n) full scan
SELECT price, volume FROM trades
WHERE price BETWEEN 15000 AND 16000;
```

**Benchmark:** For 1M rows, a 1% selectivity range filter reduces rows_scanned from 1,000,000 → ~10,000.

### 5.2 `g#` — Grouped (Hash) Attribute

**Status:** Planned

Hash index for low-cardinality columns (e.g., exchange_id, side). O(1) equality lookup.

### 5.3 `p#` — Parted Attribute

**Status:** Planned

Similar to `g#` but with contiguous memory layout per group — enables range skip on a partitioned column.

## 6. Time Range Index

**Status:** ✅ Implemented

Timestamps within each partition are monotonically increasing (append-only). The executor uses `timestamp_range(lo, hi)` (O(log n) binary search) and `overlaps_time_range(lo, hi)` (O(1) partition skip) automatically for `WHERE timestamp BETWEEN ...` clauses.

Related code: `Partition::timestamp_range()`, `Partition::overlaps_time_range()`, `QueryExecutor::extract_time_range()`

## 7. Data Durability — Intra-day Snapshot & Recovery

**Status:** ✅ Implemented (2026-03-23)

### Problem

ZeptoDB is an in-memory database. Without explicit persistence:
- A process crash or node restart loses all RDB (ACTIVE partition) data since the last EOD flush.
- SEALED partitions are only written to HDB by `FlushManager` on memory pressure — not on a time-based schedule.

### Design

Two orthogonal mechanisms together close the data loss window to at most `snapshot_interval_ms` (default 60 s):

#### 7.1 Intra-day Auto-Snapshot

`FlushManager` (background thread) writes a **binary snapshot** of every partition — including ACTIVE partitions that have not yet been sealed — to a configurable `snapshot_path` directory.

```
{snapshot_path}/{symbol_id}/{hour_epoch}/{col}.bin
```

The snapshot uses the same LZ4-compressed binary format as HDB flush (`HDBWriter::snapshot_partition()`), but skips the sealed-state check. Files are overwritten on every snapshot cycle, making recovery deterministic.

**Config:**
```cpp
FlushConfig cfg;
cfg.enable_auto_snapshot  = true;
cfg.snapshot_interval_ms  = 60'000;   // 60s default
cfg.snapshot_path         = "/var/zeptodb/snap";
```

**Manual trigger:**
```cpp
flush_manager->snapshot_now();  // synchronous
```

**Implementation:** `src/storage/flush_manager.cpp` — `do_snapshot()`, `snapshot_now()`

Timer check is inside `flush_loop()`:
```
every check_interval_ms:
  → do_flush_sealed()          // existing SEALED flush
  → if now - last_snapshot ≥ snapshot_interval_ms → do_snapshot()
```

#### 7.2 Recovery on Restart

When `PipelineConfig::enable_recovery = true`, `ZeptoPipeline::start()` reloads the snapshot directory **before** starting drain threads. Recovery is safe at all `StorageMode` levels (PURE_IN_MEMORY, TIERED, PURE_ON_DISK).

```cpp
PipelineConfig cfg;
cfg.enable_recovery          = true;
cfg.recovery_snapshot_path   = "/var/zeptodb/snap";

ZeptoPipeline pipeline(cfg);
pipeline.start();  // reads snapshot → store_tick() each row → starts drain
```

Recovery path:
1. `std::filesystem::directory_iterator` enumerates `{snap}/{symbol}/{hour}` directories.
2. `HDBReader::read_column()` mmap-reads `timestamp`, `price`, `volume`, `msg_type`.
3. Each row reconstructed as `TickMessage` and replayed via `store_tick()`.
4. Drain threads start after all rows are loaded — no concurrency hazard.

**Implementation:** `src/core/pipeline.cpp` — `ZeptoPipeline::start()` recovery block

### Guarantees

| Property | Value |
|----------|-------|
| Max data loss window | ≤ `snapshot_interval_ms` (default 60 s) |
| Snapshot overhead | Negligible — sequential write, same thread as flush_loop |
| Recovery time | Proportional to snapshot size (mmap → store_tick) |
| Thread safety | Recovery runs single-threaded before drain threads start |
| Storage format | Same binary `.bin` as HDB — no additional format |

### Integration Test Coverage

| Test | What is verified |
|------|-----------------|
| `HDBTest.AutoSnapshot_CreatesFiles` | `snapshot_now()` creates `price.bin` in snapshot dir for ACTIVE partition |
| `HDBTest.Recovery_ReloadsData` | New pipeline with `enable_recovery=true` restores all 50 rows from snapshot |

---

## 8. HDB On-Disk Format — File Header (v1 / v2)

Each HDB column file starts with a fixed-size header.

| Version | Header size | Emitted by | Contains `table_id` | Read-compatible by |
|---------|-------------|------------|---------------------|--------------------|
| v1 | 32 bytes | Pre-devlog 083 writers | No (implicit `0`) | v1 & v2 readers |
| v2 | 40 bytes | Current writer (`HDB_VERSION = 2`) | Yes, `uint16_t` at offset 32 | v2 readers only |

v2 layout (`HDBFileHeader`, `sizeof == 40`):

```
offset  size  field
  0     5     magic[5] = "APEXH"
  5     1     version  = 2
  6     1     col_type (ColumnType)
  7     1     compression (0=None, 1=LZ4)
  8     8     row_count
 16     8     data_size
 24     8     uncompressed_size
 32     2     table_id            <-- v2 only
 34     6     reserved[6] (zero)
```

Readers dispatch on the `version` byte at offset 5: v1 files are read with a 32-byte header and `table_id` is forced to `0`. v2 files read the full 40 bytes. A caller passing `table_id != 0` to `HDBReader::read_column` will be rejected if the on-disk header carries a different `table_id` (cross-table leak guard).

### 8.1 Directory Layout

| table_id | Directory pattern |
|----------|-------------------|
| `0` (legacy / single-table) | `{hdb_base}/{symbol_id}/{hour_epoch}/{col}.bin` |
| `> 0` (CREATE TABLE assigned) | `{hdb_base}/t{table_id}/{symbol_id}/{hour_epoch}/{col}.bin` |

`HDBWriter::partition_dir(table_id, symbol, hour)` is the canonical builder; a legacy single-argument overload delegates with `table_id = 0` and returns the pre-v2 layout so existing kdb+-style HDB roots remain readable.

## 9. SchemaRegistry Durability (`_schema.json`)

`SchemaRegistry` persists the catalog to `{hdb_base_path}/_schema.json` whenever a CREATE/DROP TABLE succeeds, so `table_id` assignments survive process restarts:

```json
{"next_table_id":4,
 "tables":[
   {"name":"trades","table_id":1,"ttl_ns":0,"has_data":1,
    "columns":[{"name":"price","type":0}, ...]},
   ...
 ]}
```

- **Save:** `ZeptoPipeline::save_schema_catalog()` is called from `QueryExecutor::exec_create_table` and `exec_drop_table` when `hdb_base_path` is non-empty. Write is `open + rename(2)` for crash-atomicity.
- **Load:** `ZeptoPipeline` constructor calls `SchemaRegistry::load_from()` after `HDBWriter` is built. `next_table_id_` is restored as `max(persisted next, max(loaded table_id) + 1)` so a new CREATE after restart never reuses a dropped id.
- **No dep:** Hand-rolled JSON (no `nlohmann/json`); all fields are scalars.
- **PURE_IN_MEMORY mode:** `hdb_base_path` is empty by default, so save/load are no-ops — matching the pre-existing ephemeral behavior.

See devlog 083 for the full Stage A changelog.

## 10. WAL forward-compatibility with `table_id` (Stage B — devlog 084)

`WALWriter::write` persists the raw 64-byte `TickMessage` (see
`include/zeptodb/ingestion/tick_plant.h`). Since devlog 082, `TickMessage`
includes `uint16_t table_id` within that 64-byte layout — **no WAL format
change is required for Stage B**:

- WAL files written *after* devlog 082 already carry the correct `table_id`
  on every record and replay losslessly.
- WAL files written *before* devlog 082 replay with the old byte layout; on
  modern readers, the bytes at the `table_id` offset are either zero (from
  pre-field padding) or ignored, giving the legacy path (`table_id = 0`).
- No forward-compat break: the 64-byte `static_assert` is unchanged, so
  older binaries reading newer WAL files still see exactly 64 bytes per
  record and simply ignore the `table_id` field they don't know about.

See devlog 084 for the full Stage B changelog.

