# 086 — Residual limits closed (Stage D)

Date: 2026-04-18
Status: Complete
Related: [082](082_table_scoped_partitioning.md), [083](083_stage_a_hdb_and_durability.md), [084](084_stage_b_ingest_paths.md), [085](085_stage_c_cluster_and_sql_strict.md)

## Scope (Stage D — residual sweep)

Stage C (devlog 085) closed the 8-item sweep from 082 but left five known
residual limits in the "Known Limits" section. Stage D closes four of them in
code; the fifth (aarch64 cross-arch verification) is handed to QA.

| # | Residual limit (from 085) | Resolution in 086 |
|---|---|---|
| 1 | `SchemaRegistry::save_to` concurrent race — shared_lock + fixed `.tmp` suffix could race between two concurrent DDL callers | **D1** — `save_to` now takes `unique_lock`; tmp filename is per-`(pid, tid)` (`SYS_gettid` on Linux) |
| 2 | Migration tools don't register `dest_table` in `SchemaRegistry`, so post-migration `SELECT` sees empty | **D2** — new header-only helper `zeptodb::migration::ensure_dest_table(hdb_dir, name)`; `tools/zepto-migrate.cpp` calls it once for dir-output modes (HDB / ClickHouse / DuckDB) after arg parsing |
| 3 | FIX / ITCH / Binance parsers don't auto-stamp `table_id` on emitted ticks | **D3** — `feeds::Tick` gains a `uint16_t table_id` field (default 0); ITCH `extract_tick` stamps from `table_id_`; FIX handler stamps `tick.table_id = parser_.table_id()` right before `tick_callback_`; Binance plumbing is ready but the `.cpp` bridge still doesn't exist |
| 4 | `zepto_http_server` CLI hardcodes `PURE_IN_MEMORY` with no `--hdb-dir` flag | **D4** — new `--hdb-dir <path>` and `--storage-mode <pure|tiered>` flags; either flag switches `PipelineConfig::storage_mode` to `TIERED` with the supplied `hdb_base_path` |
| 5 | aarch64 cross-arch verification deferred | **Deferred to QA** — the `cross-arch-verification` skill describes the EKS bench cluster workflow; Stage D ships the code changes so QA can run the aarch64 suite against them |

## Files changed

| File | Change |
|---|---|
| `include/zeptodb/storage/schema_registry.h` | D1: `save_to` uses `unique_lock` + per-`(pid,tid)` tmp filename; added `<sys/syscall.h>`, `<sys/types.h>`, `<unistd.h>`, `<thread>` includes |
| `include/zeptodb/feeds/tick.h` | D3: added `uint16_t table_id = 0` field to `Tick` (default initialised) |
| `src/feeds/nasdaq_itch.cpp` | D3: `NASDAQITCHParser::extract_tick` stamps `tick.table_id = table_id_` before the per-message-type switch |
| `src/feeds/fix_feed_handler.cpp` | D3: stamps `tick.table_id = parser_.table_id()` immediately before `tick_callback_(tick)` |
| `include/zeptodb/migration/migrate_utils.h` | D2: new header; `ensure_dest_table(hdb_dir, name)` — loads `_schema.json`, `create()` if absent, atomic `save_to`, returns assigned `table_id` |
| `tools/zepto-migrate.cpp` | D2: call `ensure_dest_table` once after arg parsing for HDB / ClickHouse / DuckDB modes when `--dest-table` + output dir are both provided |
| `tools/zepto_http_server.cpp` | D4: parse `--hdb-dir` / `--storage-mode`; switch pipeline config to `TIERED` when either is set; help text updated |
| `tests/unit/test_table_scoped_partitioning.cpp` | D1 test: `SchemaRegistrySaveConcurrentSafe` — 8 threads × 10 creates + saves on the same path; final `load_from` must see all 80 tables and zero save failures |
| `tests/unit/test_feed_table_id.cpp` (new) | D3 tests: `ItchParserAutoStampsTableId` (full ITCH Trade packet), `FixParserSetterRoundTrip`, `BinanceParserSetterRoundTrip` (no .cpp — placeholder), `DefaultTickTableIdIsZero` |
| `tests/migration/test_hdb_loader.cpp` | D2 test: `HDBLoaderTableAware.CliStampsTableIdViaSchemaRegistry` — exercises `ensure_dest_table` directly, verifies round-trip via fresh `SchemaRegistry::load_from` |
| `tests/integration/test_http_hdb.sh` (new, +x) | D4 integration: boots server with `--hdb-dir <tmp>`, runs `CREATE TABLE hdb_test / INSERT`, then asserts `_schema.json` exists and contains `hdb_test` |
| `tests/CMakeLists.txt` | Add `test_feed_table_id.cpp` to `zepto_tests` sources; link `zepto_feeds` so the new test resolves |
| `docs/devlog/085_stage_c_cluster_and_sql_strict.md` | Known Limits annotated — D1–D4 marked RESOLVED in 086 |
| `docs/COMPLETED.md` | New bullet for devlog 086 |
| `KIRO.md` | devlog next-number hint bumped 086 → 087 |
| `docs/api/HTTP_REFERENCE.md` | `--hdb-dir` / `--storage-mode` documented under the `zepto_http_server` CLI |

## Tests added (summary)

- `TableScopedPartitioning.SchemaRegistrySaveConcurrentSafe` — D1
- `HDBLoaderTableAware.CliStampsTableIdViaSchemaRegistry` — D2
- `FeedTableId.ItchParserAutoStampsTableId` — D3 (primary)
- `FeedTableId.FixParserSetterRoundTrip` — D3 (wiring pin)
- `FeedTableId.BinanceParserSetterRoundTrip` — D3 (no-op placeholder)
- `FeedTableId.DefaultTickTableIdIsZero` — D3 (struct invariant)
- `tests/integration/test_http_hdb.sh` — D4 (integration, bash)

## Operator notes

### D4: using `--hdb-dir` / `--storage-mode`

```bash
# Persist RDB + HDB to a local directory (tiered mode).
./zepto_http_server --port 8123 --hdb-dir /var/lib/zeptodb/hdb

# Explicit: same thing.
./zepto_http_server --port 8123 --hdb-dir /var/lib/zeptodb/hdb --storage-mode tiered

# Force in-memory (default).
./zepto_http_server --port 8123 --storage-mode pure
```

- `--hdb-dir <path>` implies `--storage-mode tiered` — supplying the directory
  is enough.
- If `--storage-mode tiered` is passed without `--hdb-dir`, the pipeline uses
  the default `/tmp/zepto_hdb`.
- `_schema.json` is written under `<hdb-dir>/_schema.json` on every DDL and
  reloaded on server start, so `CREATE TABLE` survives restarts.

### D2: migration tools + SchemaRegistry

```bash
./zepto-migrate hdb \
    --hdb-dir /data/kdb_hdb \
    --output   /data/zepto_hdb \
    --dest-table trades_migrated \
    -v
# → "dest_table 'trades_migrated' registered with table_id=3 in /data/zepto_hdb/_schema.json"
```

After migration, launching the server with `--hdb-dir /data/zepto_hdb` makes
`SELECT * FROM trades_migrated` resolve via the persisted catalog. The dest
table is created with an empty column list — callers that want typed columns
can add a `CREATE TABLE` step (strict fallback is satisfied either way because
`exists(name)` is true).

### D3: feed parsers

When forwarding parsed `Tick` records into a `ZeptoPipeline`, the parser's
`set_table_id(tid)` now actually propagates onto the emitted struct. Callers
that translate `feeds::Tick → ingestion::TickMessage` should copy
`tick.table_id` onto `msg.table_id` before `pipeline.ingest_tick(msg)`. This
preserves the SchemaRegistry-scoped routing established in Stages A/B/C.

## D5: aarch64 deferred to QA

Per the `cross-arch-verification` skill, Stage D does not run the aarch64
suite — that's the QA stage's job against the EKS bench cluster
(`./tools/eks-bench.sh wake` → run → `sleep`). All code in D1–D4 is
arch-neutral (no new SIMD, no platform-specific atomics, `SYS_gettid` is
Linux-only but guarded behind `__linux__`).

## Verification (Stage D local baseline)

```
$ ./tests/zepto_tests --gtest_filter="*TableScoped*:*HDB*:*Feed*:*Schema*"
[  PASSED  ] 50 tests.

$ ./tests/zepto_tests 2>&1 | tail -3
[==========] 1210 tests from 152 test suites ran.
[  PASSED  ] 1210 tests.                    # was 1205 post-Stage-C, +5 (D1 + 4×D3)

$ ./tests/test_migration 2>&1 | tail -3
[==========] 131 tests from 21 test suites ran.
[  PASSED  ] 131 tests.                     # was 130 post-Stage-B, +1 (D2)

$ ./tests/test_feeds 2>&1 | tail -3
[  PASSED  ] 23 tests.                      # unchanged — Tick `table_id` field
                                            # default-initialises, no behavioural regression

$ bash tests/integration/test_http_hdb.sh
PASS: test_http_hdb

$ bash tests/integration/test_multiprocess.sh
=== Results: 5 passed, 0 failed ===
```

x86_64 baseline taken on the dev host. aarch64 run is QA's responsibility per
D5 — code is arch-neutral so QA can rerun the same set on the EKS bench
cluster without code changes.

## Follow-ups (closed in devlog 088)

The four residual items left open after Stage D are all closed in
[devlog 088](088_client_ddl_and_final_residuals.md):

| # | 086 residual | 088 resolution |
|---|---|---|
| 1 | `SchemaRegistry::save_to` held `unique_lock` for the whole JSON write — concurrent DDL serialized on slow file I/O | **RESOLVED** — snapshot under `shared_lock`, lockless JSON write, brief `unique_lock` only around `std::rename` |
| 2 | Binance feed had no `.cpp`; auto-stamp test was a `SUCCEED()` placeholder | **RESOLVED** — new `src/feeds/binance_feed.cpp` stamps `tick.table_id = table_id_` before `tick_callback_`; live `BinanceParserAutoStampsTableId` test replaces the stub |
| 3 | `Quote` / `Order` structs had no documented future `table_id` path | **RESOLVED** — explicit paragraph in `docs/design/layer2_ingestion_network.md` describing the P9/P10 migration recipe |
| 4 | aarch64 cross-arch verification deferred | **RESOLVED (code)** — `test` stage in `deploy/docker/Dockerfile` + `tools/run-aarch64-tests.sh` buildx runner; actual EKS-bench execution remains QA's step |
