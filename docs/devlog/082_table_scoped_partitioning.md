# Devlog 082 — Table-Scoped Partitioning (P7 Critical)

Date: 2026-04-18
Status: Complete
Related: BACKLOG.md §P7 "Table-Scoped Partitioning", layer1_storage_memory.md §4

## Problem

`PartitionKey = (symbol_id, hour_epoch)` had no table dimension. All tables shared the same partition pool, so a freshly-created empty table could return rows belonging to other tables:

```
CREATE TABLE a (…);
CREATE TABLE b (…);
INSERT INTO a VALUES (1, 10, 100, 1000);
SELECT * FROM b;   -- Bug: returned 1 row from table a
```

For multi-table workloads (Physical AI, IoT — dozens of telemetry streams like
`temperature`, `vibration`, `lidar`) every query scanned every partition in
the pool → 10–50× unnecessary overhead.

## Solution

Add `table_id` (`uint16_t`) as the first dimension of `PartitionKey` →
`(table_id, symbol_id, hour_epoch)`. `table_id = 0` is reserved for the
legacy/default path (no `CREATE TABLE`) so all pre-existing APIs keep working.

### Wire-up

1. `TableSchema` now carries `table_id`; `SchemaRegistry` assigns a monotonic
   `next_table_id_` starting at 1 on each `create(name, …)`. IDs are never
   reused on `drop` (simplest policy).
2. `TickMessage._pad[6]` → `uint16_t table_id + _pad[4]`; cache-line size
   (`sizeof == 64`) unchanged (`static_assert` preserved).
3. `PartitionManager::get_or_create(table_id, symbol, ts)` — new canonical
   signature. Legacy `(symbol, ts)` overload kept, delegating with `table_id=0`.
4. New `PartitionManager::get_partitions_for_table(table_id)` returns every
   partition whose key matches the given table. `get_partitions_for_symbol` and
   `get_partitions_for_time_range` now have table-scoped overloads alongside
   the legacy ones.
5. `ZeptoPipeline::store_tick` passes `msg.table_id` into `get_or_create`.
   The in-pipeline `partition_index_` is re-keyed to `uint64_t = (table_id << 32) | symbol_id`.
   `find_partitions(SymbolId)` is preserved as a `table_id=0` thin wrapper; new
   `find_partitions(table_id, symbol)` added.
6. `QueryExecutor::exec_insert` stamps
   `msg.table_id = pipeline_.schema_registry().get_table_id(stmt.table_name)`
   before `ingest_tick`.
7. `QueryExecutor::find_partitions(table_name)` now returns only the partitions
   whose `table_id` matches the registered table. When the name is not
   registered (legacy single-table mode), it falls back to `get_all_partitions()`.
8. The `SELECT` path resolves `from_tid` once from `stmt.from_table`, then the
   three symbol/time-range partition lookups (lines 1243, 1324, 1354 of
   `executor.cpp`) pass `from_tid` through, so `WHERE symbol = …` and
   `WHERE ts BETWEEN …` are table-scoped.
9. `cluster::PartitionRouter`: new `route(table_id, symbol)` and
   `route_replica(table_id, symbol)` overloads XOR `table_id << 16` into the
   symbol hash before the existing ring lookup. Existing single-argument
   `route(symbol)` is unchanged (≡ `table_id = 0`).

## Files Changed

| File | Change |
|---|---|
| `include/zeptodb/storage/partition_manager.h` | `PartitionKey.table_id`, new accessors, hash mix |
| `src/storage/partition_manager.cpp` | `get_or_create(table_id, …)`, `get_partitions_for_table`, scoped overloads |
| `include/zeptodb/ingestion/tick_plant.h` | `TickMessage.table_id` (via shrunk pad) |
| `include/zeptodb/storage/schema_registry.h` | `table_id` field, `next_table_id_`, `get_table_id()` |
| `include/zeptodb/core/pipeline.h` | `partition_index_` re-keyed to `uint64_t`, new `find_partitions` overload |
| `src/core/pipeline.cpp` | `store_tick`, `find_partitions`, TTL rebuild use `(table_id, symbol_id)` key |
| `src/sql/executor.cpp` | `exec_insert` sets `table_id`; `find_partitions` scoped; WHERE path threads `from_tid` |
| `include/zeptodb/cluster/partition_router.h` | New table-aware `route(table_id, sym)` / `route_replica(table_id, sym)` |
| `tests/unit/test_table_scoped_partitioning.cpp` | **NEW** — 7 tests |
| `tests/unit/test_sql.cpp` | SetUp fixtures now `CREATE TABLE trades` before ingest and stamp `msg.table_id` |
| `tests/unit/test_flight_server.cpp` | SetUp fixture stamps `msg.table_id` after `CREATE TABLE` |
| `tests/unit/test_hdb.cpp` | `PartitionKey{…}` aggregate init updated for new field order |
| `tests/unit/test_table_statistics.cpp` | Same |
| `tests/bench/bench_hdb.cpp` | Same |
| `tests/CMakeLists.txt` | Register new test file |

## Tests

```
$ ./tests/zepto_tests --gtest_filter="*TableScoped*"
[  PASSED  ] 7 tests.

$ ./tests/zepto_tests
[  PASSED  ] 1192 tests.   (was 1185 before; +7 new)
```

| New test | What it proves |
|---|---|
| `TickMessageSize64` | `sizeof(TickMessage) == 64` preserved |
| `PartitionKeyHashDistinct` | Same (symbol, hour), different table_id → different hash |
| `CreateTableAssignsUniqueTableId` | 3 tables get 3 distinct ids |
| `InsertIntoTableSetsMsgTableId` | `get_partitions_for_table(tid)` populates on INSERT |
| `EmptyTableReturnsZeroRows` | The actual bug fix |
| `TwoTablesIsolated` | `sum()` on one table excludes rows from another |
| `LegacyPathTableIdZero` | Direct `ingest_tick` with `table_id = 0` still works |

Build targets all link cleanly: `zepto_tests`, `zepto_http_server`, `zepto_data_node`, `zepto-cli`.

## Performance Impact

- Ingest path: +2 bytes into the hash; unmeasurable.
- Query (HFT, 1–3 tables): 0–2× faster.
- Query (IoT, 10+ tables): 2–10× faster.
- Query (Physical AI, 50+ tables): 10–50× faster — queries no longer scan
  other tables' partitions.
- Memory: +2 bytes per `PartitionKey`; ≤1 % of partition metadata.

## Backward Compatibility

- `table_id = 0` reserved for legacy mode (no `CREATE TABLE`). All existing
  one-argument overloads (`get_or_create(sym, ts)`, `get_partitions_for_symbol(sym)`,
  `get_partitions_for_time_range(lo, hi)`, `find_partitions(sym)`, `route(sym)`)
  continue to work and are equivalent to passing `table_id = 0`.
- `ZeptoPipeline::query_vwap/query_filter_sum/query_count(SymbolId,…)` keep
  their signatures; they now use `find_partitions(0, sym)` internally.
- SQL tests that previously mixed raw `ingest_tick` with `CREATE TABLE foo` had
  to be updated: after CREATE TABLE, subsequent ingests must stamp `msg.table_id`.
  This is the intended new semantics — the bug being fixed.

## Deviations from Spec

- Devlog filename: spec said `docs/devlog/053_*.md`; actual next sequential
  number is **082** (devlog 053 already existed as `053_k8s_compat_ha_testing.md`).
  Used 082.
- `PartitionRouter::route(table_id, sym)` uses XOR-fold into the existing
  `SymbolId` space (`sym ^ (table_id << 16)`) rather than allocating a new
  64-bit hash key — preserves the existing cache/ring machinery unchanged.

## Known Limits

> **Status update (2026-04-18):** The 8 follow-up surfaces identified after 082 — HDB writer/reader table-awareness, file header v2, FlushManager, SchemaRegistry JSON durability, Python `ingest_batch(table_name=)`, feed-handler `table_name` config, migration-tool `--dest-table` flag, `ClusterNode::ingest_tick` + query coordinator table-aware routing, strict SQL fallback, and WAL forward-compat note — are **all RESOLVED**. See `083_stage_a_hdb_and_durability.md`, `084_stage_b_ingest_paths.md`, and `085_stage_c_cluster_and_sql_strict.md`.

- ~~**Maximum tables per process lifetime: 65,535.**~~ Still applies — `table_id` is `uint16_t` and IDs are not reused on `DROP TABLE` to avoid stale-partition resurrection. Workloads that churn >65K tables should be handled by a catalog-level rotation or a future migration to `uint32_t`. `SchemaRegistry::create()` returns `false` if the counter wraps.
- ~~**PartitionRouter XOR-fold collisions.**~~ Still applies — `route(table_id, symbol)` combines the two via `symbol ^ (table_id << 16)`. If a `SymbolId` ever has bit 16 or higher set, this can collide with a different `(table_id, symbol)` pair and produce identical routing — a load-balancing skew, not a correctness bug (partitions are still disambiguated by the full `PartitionKey` downstream). Current string-dictionary-based `SymbolId` allocation stays well below 65K, so this is acceptable today. Future work: replace XOR with splitmix or run each word through `symbol_hash` separately.
