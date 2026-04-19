# 085 — Table-aware full-sweep: Stage C (cluster routing + strict SQL fallback)

Date: 2026-04-18
Status: Complete (Stage C closes the 8-limitations sweep from devlog 082)
Related: `docs/devlog/082_table_scoped_partitioning.md`, `docs/devlog/083_stage_a_hdb_and_durability.md`, `docs/devlog/084_stage_b_ingest_paths.md`

## Scope (Stage C)

Stages A (storage / HDB v2 / SchemaRegistry durability) and B (Python + feeds + migration ingest) made every **storage and ingress** surface table-aware. Stage C finishes the 8-item sweep on the **routing + SQL** side:

1. **`ClusterNode::ingest_tick`** now routes via `route(msg.table_id, msg.symbol_id)`. Different tables with the same symbol id can now land on different owners (previously they collided).
2. **New `ClusterNode::route(table_id, symbol)` overload** wraps `PartitionRouter::route(table_id, symbol)` under the `router_mutex_` shared lock. The existing single-arg `route(symbol)` stays, so every legacy call still compiles (`route(sym) == route(0, sym)`).
3. **Query coordinator wiring** — no RPC format change is needed. Each data node resolves `FROM <table>` via its own `SchemaRegistry` when the scattered SQL lands. Because Stage A made `_schema.json` durable, nodes that load from the same file produce the same `table_id`, so scatter-gather SELECT "just works" across the cluster. A new test (`ClusterRouting.ScatterGatherWithDurableSchemaRegistry`) pins this invariant.
4. **Strict SQL fallback** — the Stage A `QueryExecutor::find_partitions()` already implements the required semantics:
   - `schema_registry.table_count() == 0` → legacy fallback (returns all partitions). Preserves pre-082 behavior for programmatic ingest flows with no `CREATE TABLE`.
   - `table_count() > 0 && !exists(name)` → returns `{}` (standard-SQL strict mode: unknown table yields empty result, not other tables' rows).
   - `exists(name)` → `get_partitions_for_table(tid)`.
   Stage C adds the regression test `StrictFallbackUnknownTableReturnsEmpty` (CREATE TABLE a; INSERT; `SELECT * FROM bogus` → 0 rows) and the legacy-mode counterpart `LegacyFallbackWithNoCreateTable`.
5. **WAL forward-compat note** — documented in `layer2_ingestion_network.md` §WAL: WAL records written before devlog 082 replay with `table_id = 0` (the `TickMessage` padding bytes that now hold `table_id` were zero in the pre-082 on-disk layout), which is the same legacy pool rolling upgrades already expect. No format change required.

## Files Changed (Stage C)

| File | Change |
|---|---|
| `include/zeptodb/cluster/cluster_node.h` | `ingest_tick` now calls `route(msg.table_id, msg.symbol_id)` (was `route(msg.symbol_id)`). Added `NodeId route(uint16_t table_id, SymbolId symbol) const` overload. Migration dual-write path (`migration_target`) is unchanged per the pre-existing single-symbol migration model. |
| `docs/devlog/082_table_scoped_partitioning.md` | Known Limits section annotated: the 8 follow-up surfaces from 082 are marked RESOLVED with pointers to 083/084/085. |
| `docs/devlog/085_stage_c_cluster_and_sql_strict.md` | This file. |
| `docs/design/layer1_storage_memory.md` | HDB layout `{base}/t{tid}/{sym}/{hour}/`, v2 header, `_schema.json` durability already documented after Stage A; cross-referenced from 085. |
| `docs/design/layer2_ingestion_network.md` | Added WAL forward-compat note for pre-082 WAL files replaying as `table_id = 0`. |
| `docs/api/PYTHON_REFERENCE.md` | Stage B already documented `table_name=` kwarg; Stage C cross-links strict fallback behavior. |
| `docs/api/SQL_REFERENCE.md` | Added note: `SELECT FROM <unknown_table>` returns empty when any `CREATE TABLE` exists in the catalog (strict mode); legacy mode (no `CREATE TABLE` ever) falls back to all partitions. |
| `docs/api/CPP_REFERENCE.md` | Documented `ClusterNode::ingest_tick` honoring `msg.table_id` and the new `route(table_id, symbol)` overload. |
| `docs/COMPLETED.md` | Added devlog 085 entry. |
| `.kiro/KIRO.md` | Devlog next-number hint bumped 085 → 086. |
| `tests/unit/test_table_scoped_partitioning.cpp` | **+4 tests**: `StrictFallbackUnknownTableReturnsEmpty`, `LegacyFallbackWithNoCreateTable`, `ClusterRouting.TableAwareRouteOverload`, `ClusterRouting.ScatterGatherWithDurableSchemaRegistry`. |

## Tests

```
$ ./tests/zepto_tests --gtest_filter="TableScopedPartitioning.*:ClusterRouting.*"
[  PASSED  ] 14 tests.   (was 10 post-Stage-A; +4 Stage C)

$ ./tests/zepto_tests
[  PASSED  ] 1205 tests.  (was 1201 post-Stage-B; +4 Stage C)

$ ./tests/test_migration
[  PASSED  ] 130 tests.   (unchanged — no migration changes in Stage C)

$ bash tests/integration/test_multiprocess.sh
=== Results: 5 passed, 0 failed ===

$ python3 -m pytest tests/python/
======================== 212 passed, 1 warning ========================
```

## Full-sweep totals across Stages A + B + C

| Suite | Baseline (pre-082) | Stage A | Stage B | Stage C (final) |
|---|---|---|---|---|
| `zepto_tests` (C++ main) | 1180 | 1196 (+16) | 1201 (+5) | **1205** (+4) |
| `test_migration` | ~115 | 126 (+11) | 130 (+4) | 130 |
| `test_feeds` | 21 | 21 | 23 (+2) | 23 |
| Python `pytest` | ~205 | 208 (+3) | 212 (+4) | 212 |
| Multiprocess integration | 5 | 5 | 5 | 5 |

**Grand total new tests across A + B + C: ~45.** Zero regressions in the full suite.

## What closing the 8 limitations actually looked like

| # | Limitation (from 082) | Resolution | Stage |
|---|---|---|---|
| 1 | HDB Writer path not table-aware | `HDBWriter::partition_dir()` emits `{base}/t{tid}/{sym}/{hour}/` for `tid != 0`; preserves legacy path for `tid == 0` | A |
| 2 | HDB file header missing `table_id` | `HDBFileHeader` v2 (40 B) adds `table_id + reserved`; `HDB_VERSION = 2` | A |
| 3 | HDB Reader not backward-compat | `HDBReader` dispatches on version byte; v1 (32 B) still reads | A |
| 4 | FlushManager not table-aware | `FlushManager::flush_partition_parquet` threads `table_id` through to parquet dir | A |
| 5 | SchemaRegistry not durable | `SchemaRegistry::save_to / load_from` as JSON at `{hdb_base}/_schema.json`; reloads `table_id`, columns, TTL, `has_data` flag; `next_table_id_ > max(loaded)` | A |
| 6 | Python binding missing `table_name=` | `Pipeline.ingest_batch / ingest_float_batch` + DSL `from_pandas / from_polars / from_arrow` / `ArrowSession` kwarg | B |
| 7 | Feed handlers missing `table_name` config | Kafka/MQTT `*Config::table_name`; FIX/ITCH/Binance setters | B |
| 8a | Migration tools missing `--table` flag | `--dest-table <name>` on `zepto-migrate`; `Config::dest_table` on ClickHouse/DuckDB/Timescale/HDBLoader | B |
| 8b | `ClusterNode::ingest_tick` not table-aware | `route(msg.table_id, msg.symbol_id)` + new overload | **C** |
| 8c | Query coordinator not table-aware | Works automatically via durable per-node `_schema.json` (Stage A) + SQL-string `FROM <table>` resolved locally; test pins the invariant | **C** |
| 8d | Strict SQL fallback | `QueryExecutor::find_partitions` returns `{}` for unknown table when `table_count() > 0` | A (confirmed) / **C** (test added) |
| 8e | WAL forward-compat | No format change needed — `TickMessage` was already 64 B pre-082; pre-082 WAL replays as `table_id = 0`; documented | B (noted) / **C** (design-doc note) |

## Known follow-ups (explicitly out of scope)

> **Update 2026-04-18 (devlog 086):** residual limits D1–D4 below are now
> RESOLVED in [devlog 086](086_residual_limits_closed.md). D5 (aarch64
> cross-arch verification) is handed to the QA stage.
>
> - D1 — `SchemaRegistry::save_to` concurrent race → **RESOLVED in 086** (unique_lock + per-`(pid,tid)` tmp filename).
> - D2 — migration tools don't create `dest_table` in `SchemaRegistry` → **RESOLVED in 086** (`migration::ensure_dest_table` helper, invoked once in `zepto-migrate.cpp`).
> - D3 — FIX/ITCH/Binance parsers don't auto-stamp `table_id` → **RESOLVED in 086** (`feeds::Tick::table_id` field; ITCH + FIX handler stamp at emission point; Binance plumbing ready, `.cpp` stub still pending).
> - D4 — `zepto_http_server` hardcodes `PURE_IN_MEMORY` → **RESOLVED in 086** (`--hdb-dir` / `--storage-mode pure|tiered`).
> - D5 — aarch64 cross-arch verification → **deferred to QA** (see `cross-arch-verification` skill; Stage D is arch-neutral so QA can rerun the full suite on aarch64 without code changes).

- **Cross-cluster catalog sync.** Each node loads its own `_schema.json`; there is no automatic propagation of `CREATE TABLE` across the cluster beyond the coordinator's DDL broadcast (which creates the table on every reachable node but does not guarantee `_schema.json` fsync on every node). Rolling upgrades should seed a known-good `_schema.json` on every node before taking traffic.
- **Live-pipeline migration ingest.** Stage B plumbed `dest_table` into every migration tool config, but the actual live-pipeline path (ingesting migrated rows into a running `ZeptoPipeline` with its `SchemaRegistry`) is still CLI-driven — not a library API.
- **Query-coordinator deep cross-table fan-out.** Scatter-gather SELECT on a single table works. Multi-table queries spanning tables owned by different routing subsets still broadcast to all nodes (existing behavior); fine-grained per-table fan-out routing would require the coordinator to resolve `tid` from `_schema.json` and intersect it with `PartitionRouter::owners_for(tid)` — deferred.
- **`PartitionRouter` XOR collision** (documented in 082) — still applies for `SymbolId >= 2^16`; replace with splitmix when we outgrow the current dictionary size.

## Pre-existing (not introduced by this sweep)

- `zepto-migrate` link failure — noted in 084; unchanged.
- `APEX_TRACE / APEX_CRITICAL` naming inconsistency — unchanged, tracked as legacy in KIRO.md.

## Follow-up fix (post-review) — feed consumers aligned with table-aware routing

Reviewer MAJOR #1: `src/feeds/kafka_consumer.cpp` and `src/feeds/mqtt_consumer.cpp`
still called the legacy single-arg `router_->route(msg.symbol_id)` after Stage C
switched `ClusterNode::ingest_tick` to `route(msg.table_id, msg.symbol_id)`. The
inconsistency meant cluster-mode Kafka/MQTT ingest could land on a different
owner than direct `ClusterNode::ingest_tick` for the same `(table_id, symbol)`
tick.

Fix (2 lines):

- `src/feeds/kafka_consumer.cpp:246` — `router_->route(msg.symbol_id)` →
  `router_->route(msg.table_id, msg.symbol_id)`. `msg.table_id` is already
  stamped a few lines earlier in the same function.
- `src/feeds/mqtt_consumer.cpp:87` — same change.

The `PartitionRouter::route(uint16_t, SymbolId)` overload already existed
(added in devlog 082, `include/zeptodb/cluster/partition_router.h:162`) and is
reachable via `zeptodb/feeds/{kafka,mqtt}_consumer.h`, which both include
`zeptodb/cluster/partition_router.h`. No new includes needed.

Verification:

```
$ ninja -j$(nproc) zepto_tests
[5/5] Linking CXX executable tests/zepto_tests     # clean

$ ./tests/zepto_tests --gtest_filter="*Kafka*:*MQTT*:*Cluster*:*Router*"
[  PASSED  ] 69 tests.

$ ./tests/zepto_tests
[  PASSED  ] 1205 tests.                           # unchanged total, zero regressions
```

(One flake observed on `PartitionMigration.ExecutePlan_MultipleSymbols` during
an intermediate run — unrelated to the routing path, passes in isolation and on
re-run; see KIRO.md known flaky tests.)

