# Devlog 083 — Stage A: HDB Table-Aware Format v2 + SchemaRegistry Durability

Date: 2026-04-18
Status: Complete (Stage A of the 8-limitations closure from devlog 082)
Related: BACKLOG.md, `docs/devlog/082_table_scoped_partitioning.md`, `docs/design/layer1_storage_memory.md` §8–§9

## Scope (Stage A only)

Devlog 082 introduced table-scoped partitioning but left the on-disk HDB path, the file header, and the schema catalog **not** table-aware. Stage A closes those three:

1. `HDBWriter::partition_dir` is now `(table_id, symbol, hour)`. `table_id != 0` writes under `{base}/t{table_id}/{symbol}/{hour}/`; `table_id == 0` keeps the pre-v2 `{base}/{symbol}/{hour}/` layout.
2. `HDBFileHeader` grows from 32 bytes (v1) to 40 bytes (v2) by appending `uint16_t table_id + uint8_t reserved[6]`. `HDB_VERSION = 2`.
3. `HDBReader` dispatches on the `version` byte at offset 5 and accepts both v1 (32 B, `table_id = 0`) and v2 (40 B, `table_id` from header).
4. `FlushManager::flush_partition_parquet` threads the partition's `table_id` into the parquet directory.
5. `SchemaRegistry::save_to / load_from` persist the catalog to `{hdb_base}/_schema.json` so `table_id` survives restart.
6. `ZeptoPipeline` constructor auto-loads the catalog; `QueryExecutor::exec_create_table / exec_drop_table` call `pipeline_.save_schema_catalog()` on success.
7. The snapshot-recovery directory walker in `ZeptoPipeline::start()` now discovers both legacy `{snap}/{sym}/...` and v2 `{snap}/t{tid}/{sym}/...` subtrees.

Stages B–D (Python bindings, feed handlers, migration tools, ClusterNode ingest routing, coordinator wiring, strict SQL fallback, WAL compat note) are tracked separately and are **not** part of this devlog.

## Files Changed

| File | Change |
|---|---|
| `include/zeptodb/storage/hdb_writer.h` | v2 `HDBFileHeader` (40 B, `table_id` + `reserved[6]`); `HDB_VERSION=2`; `HDB_HEADER_V1_SIZE`/`V2_SIZE`; new `partition_dir(table_id, sym, hour)` + legacy overload; `write_column_file` takes `table_id` |
| `src/storage/hdb_writer.cpp` | `flush_partition` / `snapshot_partition` pass `key.table_id`; `partition_dir` emits `t{tid}/` subpath when `table_id != 0`; column header stamps `table_id` |
| `include/zeptodb/storage/hdb_reader.h` | `read_column`, `list_partitions`, `list_partitions_in_range`, `column_file_path` gain a `table_id` parameter; legacy single-arg overloads delegate with `table_id = 0` |
| `src/storage/hdb_reader.cpp` | Version-aware header parse (dispatch on offset-5 `version` byte); reject `table_id` mismatch; `{base}/t{tid}/...` path builder |
| `src/storage/flush_manager.cpp` | `flush_partition_parquet` builds `t{tid}/{sym}/{hour}/` when `key.table_id != 0` |
| `include/zeptodb/storage/schema_registry.h` | `save_to(path)` / `load_from(path)` (hand-rolled JSON, no new deps); private JSON helpers; `next_table_id_` restored as `max(persisted_next, max_loaded_id + 1)` |
| `include/zeptodb/core/pipeline.h` | `hdb_base_path()` accessor; `save_schema_catalog()` wrapper |
| `src/core/pipeline.cpp` | Ctor calls `schema_registry_.load_from("{hdb_base}/_schema.json")`; `start()` recovery walker handles both `t{tid}/...` and legacy `{sym}/...` subtrees and stamps `msg.table_id` |
| `src/sql/executor.cpp` | `exec_create_table` / `exec_drop_table` call `pipeline_.save_schema_catalog()` on success |
| `docs/design/layer1_storage_memory.md` | New §8 (File header v1/v2 + directory layout) and §9 (`_schema.json` durability) |
| `tests/unit/test_table_scoped_partitioning.cpp` | **+3 tests**: `HDBFlushIsolatedPerTable`, `SchemaRegistryPersistsAcrossRestart`, `HDBFileHeader.V1BackwardCompatibleRead` |

## Tests

```
$ ./tests/zepto_tests --gtest_filter="*HDB*:*TableScoped*:*Schema*"
[  PASSED  ] 25 tests.

$ ./tests/zepto_tests
[  PASSED  ] 1196 tests.   (was 1193; +3 new Stage A tests)
```

| New test | What it proves |
|---|---|
| `TableScopedPartitioning.HDBFlushIsolatedPerTable` | Two partitions with same `(symbol, hour)` but different `table_id` produce two distinct `{base}/t{tid}/...` directories and round-trip through `HDBReader` with each table seeing only its own rows. |
| `TableScopedPartitioning.SchemaRegistryPersistsAcrossRestart` | `save_to → load_from` round-trip preserves `table_id`, `ttl_ns`, `has_data`, and `columns`. `next_table_id_` is restored so a post-load `create()` never collides with persisted ids. |
| `HDBFileHeader.V1BackwardCompatibleRead` | A hand-crafted 32-byte v1 header + INT64 payload is read back with `num_rows == 3`, correct values, `table_id = 0` (via legacy overload). |

Build targets: `zepto_tests`, `zepto_http_server`, `zepto_data_node`, `zepto-cli` — all link cleanly.

## Backward Compatibility

- **Pre-devlog 083 HDB roots keep working.** Files with a `version == 1` header are read through the v2 `HDBReader` unchanged; `table_id` is forced to `0` and the files remain under `{base}/{sym}/{hour}/`.
- **Legacy `partition_dir(sym, hour)` / `read_column(sym, hour, col)` / `list_partitions(sym)` overloads preserved** — all delegate to the new `table_id = 0` variant. External code that has not yet adopted table ids is unaffected.
- **PURE_IN_MEMORY mode:** `hdb_base_path` is empty by default, so `save_schema_catalog()` and `load_from()` are no-ops and preserve the pre-existing ephemeral semantics.
- **Readers reject cross-table reads.** A caller asking for `table_id = T` will refuse to return a file whose header declares a different `table_id` (the `0` caller still matches anything — "don't care / legacy").

## Performance Impact

- HDB column file: +8 bytes per file (header grew 32 → 40). Negligible vs. multi-MB payloads.
- `_schema.json` save on CREATE/DROP TABLE: one `open + write + rename`, ~hundreds of µs on SSD, off the hot path.
- No change to query / ingest hot paths.

## Known Follow-ups (for Stages B–D)

- Python `Connection.ingest_batch(table_name=...)` binding.
- Feed handler (Kafka / MQTT / FIX / ITCH / Binance) `table_name` config.
- Migration tool `--table` flag (kdb+ / ClickHouse / DuckDB / Timescale / `hdb_loader`).
- `ClusterNode::ingest_tick` table-aware routing + query coordinator wiring.
- Strict SQL fallback: unknown table returns empty when any CREATE TABLE exists.
- WAL forward-compatibility note (WAL is still `(symbol, ts)` keyed; safe as long as WAL replay goes through the same pipeline that stamps `msg.table_id` from the SQL path).
