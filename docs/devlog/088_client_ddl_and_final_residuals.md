# 088 — Client DDL helpers + final 086 residuals

Date: 2026-04-18
Status: Complete
Related: [082](082_table_scoped_partitioning.md), [083](083_stage_a_hdb_and_durability.md), [084](084_stage_b_ingest_paths.md), [085](085_stage_c_cluster_and_sql_strict.md), [086](086_residual_limits_closed.md)

> Devlog 087 was taken by an unrelated parallel task
> (`087_parallel_test_execution.md`). This devlog uses number 088 to avoid
> collision; no other numbering is affected.

## Scope

Two concurrent workstreams close out here:

1. **Client audit gaps** — the Python HTTP client and CLI had three mismatches
   with what the engine actually supports:
   * `ingest_pandas` hard-coded `INSERT INTO ticks`, ignoring the caller's
     destination table.
   * No convenience DDL helpers (`create_table / drop_table / list_tables`).
   * CLI `show tables` translated to `SELECT name FROM system.tables`, which
     references a virtual table that has never been implemented; the parser's
     native `SHOW TABLES` handler was the right target.

2. **Residual follow-ups from devlog 086** — four known limits in the
   "Residual notes" of 086 are closed here so Stage-D code doesn't leave any
   known-weakness claims dangling.

| # | 086 follow-up | Resolution in 088 |
|---|---|---|
| 1 | `SchemaRegistry::save_to` held `unique_lock` during full JSON write, blocking DDL readers | **F1** — snapshot under `shared_lock`, JSON write with no lock held, brief `unique_lock` only around `std::rename` |
| 2 | Binance feed had no `.cpp`; the auto-stamp test was a `SUCCEED()` stub | **F2** — new `src/feeds/binance_feed.cpp` stamps `tick.table_id = table_id_` before `tick_callback_`; test replaced with live `BinanceParserAutoStampsTableId` modelled on the ITCH test |
| 3 | `Quote` / `Order` structs lacked a documented path for future `table_id` routing | **F3** — explicit paragraph in `docs/design/layer2_ingestion_network.md` describing the P9/P10 migration path (add field, stamp before callback) |
| 4 | ARM64 test image + aarch64 full-suite run | **F4** — new `test` stage in `deploy/docker/Dockerfile`; `tools/run-aarch64-tests.sh` builds/pushes via `buildx` and runs against Graviton on the EKS bench cluster. The local env cannot push to ECR, so the build + push + run is gated on QA having ECR credentials; if they don't, the code changes are arch-neutral and fall back to the documented arch-analysis pattern |

## Part 1 — Python client DDL helpers + CLI SHOW TABLES

### `zepto_py/connection.py`

* `ZeptoConnection.ingest_pandas` gained a `table_name: str = "ticks"` kwarg
  (default preserves backward compat). The generated SQL is now
  `INSERT INTO {table_name} ...`.
* `ZeptoConnection.ingest_polars` threads the same kwarg through to
  `ingest_pandas` (polars → pandas → INSERT is unchanged otherwise).
* Three new convenience methods:
  * `create_table(name, columns, if_not_exists=False)` — columns is a list of
    `(name, type)` pairs; assembles `CREATE TABLE [IF NOT EXISTS] n (...)`.
  * `drop_table(name, if_exists=False)` — `DROP TABLE [IF EXISTS] n`.
  * `list_tables() -> list[str]` — runs `SHOW TABLES`, returns the first
    column of every row.

All three go through `self.execute` / `self.query`, so timing, error
propagation and auth all behave identically to any other client call.

### `tools/zepto-cli.cpp`

One-line change at the `show tables` command handler:
`SELECT name FROM system.tables` → `SHOW TABLES`. The `system.tables` virtual
table has never existed; `SHOW TABLES` is a native parser handler that
returns the same column shape (`name` + `rows`). Manually smoke-tested
against a live `zepto_http_server`:

```
zepto> show tables
+------------+-------+
| name       | rows  |
+------------+-------+
| smoke_test | 10000 |
+------------+-------+
```

## Part 2 — 086 residual follow-ups

### F1. `SchemaRegistry::save_to` DDL hot-path

Old path held `unique_lock` for the entire JSON write (O(tables × columns)
file I/O). On a multi-tenant deployment with hundreds of tables and
concurrent DDL, this serialized every `CREATE TABLE` and blocked
`exists()` / `get()` readers for the duration of the write.

New path:

1. `shared_lock` → copy `tables_` into a local `std::vector<TableSchema>`
   and the scalar `next_table_id_`.
2. Drop the lock.
3. Write the JSON to a per-`(pid, tid)` tmp file with **no lock held**.
4. Take a brief `unique_lock` around `std::rename` only — this keeps two
   concurrent saves on the same final path from stomping each other
   without penalising the O(n) I/O.

`TableScopedPartitioning.SchemaRegistrySaveConcurrentSafe` (8 threads × 10
CREATE+save on the same path, `load_from` must see all 80 tables) still
passes. No new test needed — the invariant was already covered.

### F2. Binance `.cpp` + auto-stamp test

`include/zeptodb/feeds/binance_feed.h` has had `set_table_id` since Stage B
(devlog 084), but was header-only — so the stub test
`BinanceParserSetterRoundTrip` could only verify the getter round-trip, not
the emission-time stamping.

New file `src/feeds/binance_feed.cpp`:

* Constructor + all `IFeedHandler` vtable methods out-of-line (transport
  is still stubbed; this file's purpose is linkability, not WS support).
* `parse_trade_message(json)` — minimal hand-rolled extractor for the
  `@trade` payload (fields `s / p / q / T / m`); stamps
  `tick.table_id = table_id_` **before** calling `tick_callback_`.
* `parse_depth_message(json)` is a placeholder — depth → `Quote`
  conversion isn't part of the 088 contract.

`parse_trade_message / parse_depth_message` were moved from `private:` to
`public:` in the header so the unit test can feed a raw payload without a
friend-shim. This is a deliberate (minor) API-surface cost: once the full
WebSocket transport lands they should be callable only by the internal
read loop, but keeping them public now is cleaner than the alternative
`using BinanceFeedHandler::parse_trade_message;` sub-class trick.

`tests/unit/test_feed_table_id.cpp`:

* Include `zeptodb/feeds/binance_feed.h`.
* Replace `BinanceParserSetterRoundTrip` with
  `BinanceParserAutoStampsTableId` (mirrors
  `ItchParserAutoStampsTableId`): constructs a handler with
  `table_id = 9`, installs an `on_tick` capture, feeds a minimal `@trade`
  payload, asserts `captured.table_id == 9` and `symbol_id == 1`.

### F3. `Quote` / `Order` future-path doc note

`docs/design/layer2_ingestion_network.md` gained a short section before the
"Last updated" footer titled **"`Quote` / `Order` structs — future
`table_id` path"**. It states that only `Tick` carries a `table_id` field
today (the ingress pipeline consumes Ticks only); when order-book /
quote-distribution use cases become routable (`BACKLOG.md` items P9 / P10),
the same 3-step pattern applies: (1) add `uint16_t table_id = 0` to the
struct, (2) stamp from `parser_.table_id()` immediately before
`quote_callback_` / `order_callback_`, (3) extend the handler setters to
cascade the resolved id. No wire format or WAL impact because neither
struct is on either surface.

### F4. ARM64 test image + helper script

`deploy/docker/Dockerfile` gained a `test` stage that re-runs `cmake` with
`-DBUILD_TESTS=ON` and builds `zepto_tests test_migration test_feeds`.
`CMD` defaults to running `zepto_tests` so `docker run <image>`
transparently executes the full unit suite.

`tools/run-aarch64-tests.sh` (new, +x):

```
docker buildx build --platform linux/arm64 --target test \
    -f deploy/docker/Dockerfile -t $REPO:$TAG --push .
./tools/eks-bench.sh wake
kubectl run zepto-arm64-tests --rm -i --restart=Never \
    --image=$REPO:$TAG \
    --overrides='{"spec":{"nodeSelector":{"kubernetes.io/arch":"arm64"}}}' \
    -- /build/build/tests/zepto_tests
```

The script `trap`-sleeps the cluster on exit so it can't be left running.
`SKIP_PUSH=1` builds locally without ECR (useful for dev-host smoke).

**Deferred to QA** — this environment cannot build multi-arch images
(no buildx emulation set up) nor push to ECR (no credentials). The code
in F1 / F2 / F3 is fully arch-neutral (no new SIMD, no platform-specific
atomics); F4 is end-to-end arch-neutral (only POSIX shell + docker CLI).
QA can run the helper against the EKS bench cluster and attach the
aarch64 pass counts below when available.

## Files changed

| File | Change |
|---|---|
| `zepto_py/connection.py` | P1 + P2: `ingest_pandas(table_name=...)`, `ingest_polars(table_name=...)`, `create_table / drop_table / list_tables` |
| `tools/zepto-cli.cpp` | P3: one-line `SHOW TABLES` passthrough |
| `tests/python/test_client_ddl.py` (new) | P4: HTTP-client DDL round-trip + `ingest_pandas(table_name=...)` |
| `include/zeptodb/storage/schema_registry.h` | F1: `save_to` snapshot-then-write-then-rename pattern |
| `src/feeds/binance_feed.cpp` (new) | F2: ctor + vtable out-of-line + `parse_trade_message` that stamps `tick.table_id` |
| `include/zeptodb/feeds/binance_feed.h` | F2: `parse_trade_message / parse_depth_message` promoted to public for test reachability |
| `CMakeLists.txt` | F2: add `src/feeds/binance_feed.cpp` to `zepto_feeds` |
| `tests/unit/test_feed_table_id.cpp` | F2: replace `BinanceParserSetterRoundTrip` with `BinanceParserAutoStampsTableId` |
| `docs/design/layer2_ingestion_network.md` | F3: `Quote` / `Order` future-path paragraph |
| `deploy/docker/Dockerfile` | F4: new `test` stage |
| `tools/run-aarch64-tests.sh` (new, +x) | F4: buildx + EKS-bench + kubectl runner |
| `docs/devlog/086_residual_limits_closed.md` | follow-ups section — items 1–4 marked RESOLVED → 088 |
| `docs/api/PYTHON_REFERENCE.md` | document `create_table / drop_table / list_tables / ingest_pandas(table_name=...)` |
| `docs/COMPLETED.md` | new bullet for devlog 088 |
| `.kiro/KIRO.md` | next-devlog hint 087 → 089 (087 taken by unrelated parallel task) |

## Tests added

| Test | Purpose |
|---|---|
| `FeedTableId.BinanceParserAutoStampsTableId` (C++) | F2: replaces stub — proves `table_id` stamped on emitted Tick via live parse |
| `tests/python/test_client_ddl.py::test_create_table_and_list_tables` | P4: create + `SHOW TABLES` round-trip |
| `tests/python/test_client_ddl.py::test_ingest_pandas_with_table_name` | P1/P4: rows land in the named table, not `ticks` |

## Lines of code

Roughly: `+230` source / `+150` tests / `+170` docs. No deletions beyond the
stub test body replacement.

## Verification (local baseline)

```
$ ./tests/zepto_tests --gtest_filter="*TableScoped*:*Schema*:*Feed*:*Binance*"
[  PASSED  ] 22 tests.     # includes the new BinanceParserAutoStampsTableId

$ ./tests/zepto_tests 2>&1 | tail -3
[==========] 1210 tests from 152 test suites ran.
[  PASSED  ] 1210 tests.  # unchanged — stub test replaced, not added

$ ./tests/test_migration 2>&1 | tail -3
[  PASSED  ] 131 tests.   # unchanged

$ ./tests/test_feeds 2>&1 | tail -3
[  PASSED  ] 23 tests.    # unchanged

$ cd tests/python && python3 -m pytest
========= 214 passed, 1 warning in 7.72s =========
                          # was 212 post-Stage-B, +2 (test_client_ddl.py)

$ echo "show tables" | ./zepto-cli --host 127.0.0.1 --port 18457
+------------+-------+
| name       | rows  |
+------------+-------+
| smoke_test | 10000 |
+------------+-------+
```

x86_64 baseline on the dev host. aarch64 full-suite run is QA's step
(requires ECR push). All code in 088 is arch-neutral.

## Review residuals — RESOLVED in [089](089_client_hardening.md)

The 088 review cycle flagged six small surface issues; all are closed by
devlog 089:

1. **Python DDL identifier validation** — `create_table` / `drop_table` /
   `ingest_pandas` / `ingest_polars` now reject caller-supplied names
   that don't match `^[A-Za-z_][A-Za-z0-9_]*$` (and type strings that
   don't match `^[A-Za-z0-9_]+$`) before interpolating into SQL.
   **RESOLVED → 089 (F1)**
2. **`ingest_pandas` single-quote escape** — string values are now SQL-escaped
   (`'` → `''`); the server tokenizer was updated to decode `''` back to
   `'`. **RESOLVED → 089 (F2)**
3. **CLI trailing-semicolon strip** — the REPL and `run_script` now strip
   trailing `;` (and whitespace) before dispatch, so `CREATE TABLE ...;`
   typed at the prompt no longer hits the server's tokenizer. **RESOLVED → 089 (F3)**
4. **Binance `parse_trade_message` / `parse_depth_message` public marker** —
   an explicit `TODO(devlog 088)` now flags the eventual demotion once
   WebSocket transport lands. **RESOLVED → 089 (F4)**
5. **Binance qty precision** — inline `NOTE` documents the truncation
   semantics and the scale-factor workaround next to the `static_cast`.
   **RESOLVED → 089 (F5)**
6. **`run-aarch64-tests.sh` `REPLACE_ME` guard** — early `exit 1` if the
   operator forgot to override `REPO` (bypassable with `SKIP_PUSH=1`).
   **RESOLVED → 089 (F6)**

## 086 follow-up closure

All four residual items from devlog 086's "Known limits after Stage D"
are now RESOLVED, see the table at the top of this file. The only item
left for QA is the actual aarch64 execution of `tools/run-aarch64-tests.sh`
— the infrastructure lives in this devlog.
