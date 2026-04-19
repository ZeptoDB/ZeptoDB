# ZeptoDB — Completed Features

Last updated: 2026-04-18

---

## Core Engine
- [x] **Phase E** — E2E Pipeline MVP (5.52M ticks/sec)
- [x] **Phase B** — SIMD + JIT (BitMask 11x, filter within kdb+ range)
- [x] **Phase A** — HDB Tiered Storage (LZ4, 4.8GB/s flush)
- [x] **Phase D** — Python Bridge (zero-copy, 4x vs Polars)
- [x] **Phase C** — Distributed Cluster (UCX transport, 2ns routing)

## SQL Engine
- [x] **SQL + HTTP** — Parser (1.5~4.5μs) + ClickHouse API (port 8123)
- [x] **SQL Phase 1** — IN operator, IS NULL/NOT NULL, NOT, HAVING clause
- [x] **SQL Phase 2** — SELECT arithmetic (`price * volume AS notional`), CASE WHEN, multi-column GROUP BY
- [x] **SQL Phase 3** — Date/time functions (DATE_TRUNC/NOW/EPOCH_S/EPOCH_MS), LIKE/NOT LIKE, UNION ALL/DISTINCT/INTERSECT/EXCEPT
- [x] **SQL subqueries / CTE** — WITH clause, FROM subquery, chained CTEs, distributed CTE — 12 tests
- [x] **SQL INSERT** — INSERT INTO table VALUES, multi-row, column list, HTTP API (ClickHouse Compatible)
- [x] **SQL UPDATE / DELETE** — UPDATE SET WHERE, DELETE FROM WHERE, in-place compaction

## JOIN & Window Functions
- [x] **JOIN** — ASOF, Hash, LEFT, RIGHT, FULL OUTER, Window JOIN
- [x] **FlatHashMap for joins** — CRC32 intrinsic open-addressing hash map, replaces `std::unordered_map` in all join operators (ASOF, Hash, Window) — 9 unit tests
- [x] **Window functions** — EMA, DELTA, RATIO, SUM, AVG, MIN, MAX, LAG, LEAD, ROW_NUMBER, RANK, DENSE_RANK
- [x] **Financial functions** — xbar, FIRST, LAST, Window JOIN, UNION JOIN (uj), PLUS JOIN (pj), AJ0
- [x] **SIMD WindowJoin aggregate** — Contiguous fast-path + Highway SIMD sum_i64() for SUM/AVG, gather+SIMD for large non-contiguous windows, scalar fallback for small windows — 10 tests (devlog 080)

## Query Execution
- [x] **Parallel query** — LocalQueryScheduler (scatter/gather, 3.48x@8T), CHUNKED mode
- [x] **Time range index** — O(log n) binary search within partitions, O(1) partition skip
- [x] **Sorted column index** — `p#`/`g#` style sorted attribute, O(log n) binary search range scan, 269x vs full scan — 13 tests
- [x] **Materialized View** — CREATE/DROP MATERIALIZED VIEW, incremental aggregation on ingest, OHLCV/SUM/COUNT/MIN/MAX/FIRST/LAST, xbar time bucket
- [x] **MV query rewrite** — Automatic rewrite of SELECT GROUP BY into direct MV lookup when matching MV exists. O(n) → O(1) for aggregation queries — 6 tests (devlog 064)
- [x] **Cost-based planner (Phase 1+2)** — TableStatistics (HyperLogLog distinct, incremental min/max/count), CostModel (selectivity estimation, scan/join/sort/aggregate cost), observation-only infrastructure — 27 tests (devlog 066)
- [x] **Cost-based planner (Phase 3-6)** — LogicalPlan (AST→operator tree, predicate/projection pushdown), PhysicalPlan (cost-based scan/join/sort selection), 2-tier adaptive routing (simple→fast path, complex→cost-based), EXPLAIN v2 with cost estimates — 20 tests (devlog 067)
- [x] **Cost-based planner (Phase 7)** — Wired PhysicalPlan HASH_JOIN build side decision to exec_hash_join, TOPN_SORT already wired via apply_order_by, INDEX_SCAN already wired via collect_and_intersect. Planning overhead ~1μs. (devlog 075)
- [x] **DuckDB embedding** — Embedded DuckDB engine for Parquet offload, Arrow bridge (columnar data conversion), `duckdb('path')` table function, configurable memory budget (256MB default), lazy init, conditional compilation (`ZEPTO_ENABLE_DUCKDB`), SQL injection protection, path traversal validation (devlog 076, 077)

## Storage
- [x] **Parquet HDB** — SNAPPY/ZSTD/LZ4_RAW, DuckDB/Polars/Spark direct query (Arrow C++ API)
- [x] **S3 HDB Flush** — async upload, MinIO compatible, cloud data lake
- [x] **Storage tiering** — Hot (memory) → Warm (SSD) → Cold (S3) → Drop, ALTER TABLE SET STORAGE POLICY, FlushManager auto-tiering
- [x] **DDL / Schema Management** — CREATE TABLE, DROP TABLE (IF EXISTS), ALTER TABLE (ADD/DROP COLUMN, SET TTL), TTL auto-eviction — 8 tests
- [x] **Table-scoped partitioning** (devlog 082) — `PartitionKey = (table_id, symbol_id, hour_epoch)`; fixes `SELECT * FROM empty_table` returning data from other tables; 2–50× query speedup for multi-table workloads (IoT / Physical AI); fully backward compatible with `table_id = 0` legacy mode; `DROP TABLE` now releases the table's partitions; `SchemaRegistry::create` guards `uint16_t` overflow at 65,535 tables — 8 tests
- [x] **HDB format v2 + schema durability (Stage A of 8-limitations closure)** (devlog 083) — `HDBFileHeader` bumped to v2 (40 B, appends `uint16_t table_id + reserved[6]`); `HDBWriter::partition_dir(table_id, sym, hour)` writes under `{base}/t{tid}/{sym}/{hour}/` when `table_id > 0` (legacy `{base}/{sym}/{hour}/` kept for `table_id == 0`); `HDBReader` is version-aware (accepts v1 32-byte header with implicit `table_id = 0`); `FlushManager` parquet path table-scoped; `SchemaRegistry::save_to / load_from` persist catalog to `{hdb_base}/_schema.json` so `table_id` survives process restart; `ZeptoPipeline` auto-loads on ctor, `QueryExecutor` saves on CREATE/DROP TABLE — 3 new tests (1196 total, +3 vs 1193)
- [x] **Table-aware ingest paths & migration tools (Stage B of 8-limitations closure)** (devlog 084) — Python `Pipeline.ingest_batch(..., table_name=)` and `ingest_float_batch(..., table_name=)` kwargs resolve via `SchemaRegistry` (unknown name → `ValueError`); `zepto_py.from_pandas / from_polars / from_polars_arrow / from_arrow` and `ArrowSession.ingest_arrow[_columnar]` thread `table_name` through; `KafkaConfig::table_name` / `MqttConfig::table_name` resolve the table_id once in `set_pipeline()` and stamp it on every decoded tick; FIX / NASDAQ ITCH / Binance handler classes gain `set_table_name/set_table_id` setters; migration-tool configs (`ClickHouseMigrator / DuckDBIntegrator / TimescaleDBMigrator / HDBLoader`) all expose a `dest_table` field wired into `zepto-migrate --dest-table <name>` — +5 C++ suite tests, +4 feed tests, +4 migration tests, +4 Python tests (1201 total)
- [x] **Cluster routing + strict SQL fallback (Stage C of 8-limitations closure)** (devlog 085) — `ClusterNode::ingest_tick` now routes via `route(msg.table_id, msg.symbol_id)` so two tables with the same symbol id can land on different owners; new `ClusterNode::route(table_id, symbol)` overload with shared-lock semantics and backward-compat `route(sym) == route(0, sym)`; scatter-gather SELECT is automatically table-aware because each data node resolves `FROM <table>` via its own durable `_schema.json` (Stage A) — no RPC format change needed; strict SQL fallback confirmed (`QueryExecutor::find_partitions` returns `{}` for unknown table when `table_count() > 0`, preserves legacy all-partitions fallback when no CREATE TABLE has ever run); WAL forward-compat documented (pre-082 WAL files replay as `table_id = 0` into the legacy pool, safe for rolling upgrades) — +4 C++ suite tests (1205 total); closes all 8 follow-up surfaces from devlog 082
- [x] **Residual limits closed (Stage D of 8-limitations closure)** (devlog 086) — four residual known limits from 085 resolved: (D1) `SchemaRegistry::save_to` now uses `unique_lock` + per-`(pid,tid)` tmp filename so concurrent DDL callers can't race on the tmp path; (D2) `migration::ensure_dest_table(hdb_dir, name)` helper wired into `zepto-migrate.cpp` so `--dest-table` registers into `{output}/_schema.json` for HDB / ClickHouse / DuckDB dir-output modes; (D3) `feeds::Tick::table_id` field added (default 0), ITCH `extract_tick` stamps from `table_id_`, FIX handler stamps `tick.table_id = parser_.table_id()` before `tick_callback_(tick)` (Binance plumbing ready, `.cpp` stub still pending); (D4) `zepto_http_server` now accepts `--hdb-dir <path>` and `--storage-mode <pure|tiered>` so operators can persist the catalog across restarts instead of defaulting to `PURE_IN_MEMORY`; D5 (aarch64 cross-arch verification) handed to QA stage — all Stage D code is arch-neutral (only `__linux__`-guarded `SYS_gettid` is platform-specific)
- [x] **Client DDL helpers + final 086 residuals** (devlog 088) — (Part 1, client audit gaps) `zepto_py.ZeptoConnection.ingest_pandas` now takes `table_name` kwarg (was hard-coded `INSERT INTO ticks`) and `ingest_polars` threads it through; added `create_table / drop_table / list_tables` convenience helpers; CLI `show tables` passthrough changed from `SELECT name FROM system.tables` (virtual table never implemented) to native `SHOW TABLES` parser handler. (Part 2, 086 residual closure) F1 — `SchemaRegistry::save_to` now snapshots under `shared_lock`, writes JSON with no lock held, takes `unique_lock` only around `std::rename`; F2 — new `src/feeds/binance_feed.cpp` stamps `tick.table_id = table_id_` before `tick_callback_`, live `BinanceParserAutoStampsTableId` test replaces the old `SUCCEED()` stub; F3 — `docs/design/layer2_ingestion_network.md` documents the `Quote` / `Order` future-path pattern (P9/P10 backlog); F4 — new `test` stage in `deploy/docker/Dockerfile` + `tools/run-aarch64-tests.sh` buildx runner (aarch64 execution is QA's step, all 088 code arch-neutral). C++ suite 1210 (1 stub replaced, not added); Python suite 214 (+2); `test_migration` 131, `test_feeds` 23 both unchanged.
- [x] **Client hardening — 088 review residuals closed** (devlog 089) — six small surface fixes from the 088 review cycle: (F1) `zepto_py.ZeptoConnection._validate_identifier` / `_validate_type` helpers reject SQL injection via `create_table / drop_table / ingest_pandas / ingest_polars` caller-supplied names (type strings still validated against `^[A-Za-z0-9_]+$`); (F2) `ingest_pandas` SQL-escapes single quotes in string values (`'` → `''`) and the server tokenizer now decodes `''` back to `'` inside string literals; (F3) CLI REPL and `run_script` strip trailing `;` + whitespace before dispatch so `CREATE TABLE t (a INT64);` at the prompt no longer hits the server tokenizer's semicolon-reject; (F4) `TODO(devlog 088)` marker on the Binance `parse_trade_message / parse_depth_message` public declarations to remind the next author to demote them once WebSocket transport lands; (F5) inline `NOTE` on Binance `qty_s → uint64_t` truncation semantics and the scale-factor workaround; (F6) `tools/run-aarch64-tests.sh` early-exits if `REPO` still contains `REPLACE_ME` (bypassable with `SKIP_PUSH=1`). C++ suite 1210 (unchanged); Python suite 219 (+5 new tests); all fixes arch-neutral.
- [x] **Multi-table feature closed end-to-end** (devlog 090) — four final residuals closed, declaring table-scoped partitioning complete across every client surface. (E1) `X-Zepto-Allowed-Tables` ACL in the HTTP POST handler now covers `CREATE TABLE` / `DROP TABLE` / `ALTER TABLE` / `DESCRIBE` in addition to SELECT/INSERT/UPDATE/DELETE — parsing is hoisted once and every statement kind resolves its touched table name (`src/server/http_server.cpp:441-495`). (E2) `TenantManager::can_access_table` (previously defined but unused from HTTP) is wired in: `AuthContext.tenant_id` is stashed as `X-Zepto-Tenant-Id` at auth time and the POST path enforces the tenant `table_namespace` prefix before query execution — responses now return `403 "Tenant 't' cannot access table 'x'"` when a tenant steps outside its namespace. (E3) `tests/python/test_table_aware_ingest.py` expanded with two new tests that round-trip the `zepto_py.dataframe.from_polars` and `from_arrow` adapters through CREATE TABLE + `table_name=` + SELECT count (guarded by `pytest.importorskip`). (E4) Web UI `/tables` (SHOW TABLES) and `/tables/[name]` (DESCRIBE + SELECT) verified end-to-end against the devlog-082 table-id system via curl smoke + `pnpm test` (9 files / 61 vitest tests passed). C++ suite 1219 (+9 new `TableAclDdlTest.*`); Python suite +2 new tests.
- [x] **Multi-table residuals closeout** (devlog 091) — four residuals from devlog 090 resolved. (F1) `zepto_http_server --tenant <id:namespace>` CLI flag added (repeatable), wires a `TenantManager` at startup so operators can provision tenants without an admin API round-trip; new `tests/integration/test_http_tenant.sh` smoke test covers inside-namespace (200) and outside-namespace (403) cases. (F2) aarch64/Graviton full-suite run attempted via EKS bench pool — Karpenter requires a pending pod and `tools/run-aarch64-tests.sh` requires a real ECR `REPO` (current value is the `REPLACE_ME` guard), so this residual is deferred with a documented unblocker in the devlog; arch-neutrality evidence from earlier SIMD bit-exact tests still stands. (F3) `TenantNamespace_AllowTableInsideNamespace` tightened from `EXPECT_NE(403)` on a quoted identifier to `EXPECT_EQ(200)` on an unquoted-identifier-safe namespace `deska_`. (F4) Double parse in HTTP POST path eliminated — new `QueryExecutor::cache_prepared(sql, ps)` helper primes the prepared-statement cache from the HTTP ACL parse so the subsequent `execute()` is a cache hit instead of a re-parse; new `SqlExecutorTest.CachePreparedAvoidsReparse` unit test. C++ suite 1220 (+1 new CachePrepared test), all passing.
- [x] **Full test matrix orchestrator** (devlog 092) — new `tools/run-full-matrix.sh` composes every existing runner (ninja build, `ctest -j$(nproc) -E 'bench_\|k8s_'`, `tests/integration/*.sh`, `pytest tests/python/`, `tests/bench/run_arch_bench.sh`, `tools/run-aarch64-tests.sh`, `tests/k8s/run_eks_bench.sh`) into a single 7-stage pipeline. Stage-selectable via `--stages=…` / `--local` / `--eks` / `--all`, fail-fast by default (`--keep-going` to override), `--dry-run` to preview plan + estimated USD cost, `--skip-build` shortcut, `--repo=<ecr>` for aarch64 image push. When both stages 6 (aarch64) and 7 (EKS full) are selected, the orchestrator wakes the EKS bench cluster exactly once (guard file in `$LOG_DIR`) and installs a single global `trap … EXIT INT TERM` to sleep it exactly once on any exit path; inner `run_eks_bench.sh` is invoked with `--skip-wake --keep` to prevent double wake/sleep. Per-stage stdout+stderr tee'd to `/tmp/zepto_full_matrix_<timestamp>/stage_<n>_<name>.log`, final PASS/FAIL + wall-time summary table. Verified: `--dry-run --all` prints 7-stage plan with \$1.50 estimate; `--local` executes stages 1–4 cleanly (build + 1374 ctest cases in ~74 s + 3 integration scripts in ~9 s + pytest stage which correctly emits a WARN and skips when the system `python3` has no `zeptodb` module). Stages 6/7 were dry-run only per cost guardrail. CONTRIBUTING.md now points at the new script.
- [x] **Parallel arm64 unit stage in full-matrix orchestrator** (devlog 093) — new stage 8 `aarch64_unit_ssh` wired into `tools/run-full-matrix.sh`. Runs in parallel with stage 2 (x86 unit) via `run_stages_parallel()` (`bash &` + `wait`, per-child log + `.rc_<n>` / `.sec_<n>` side-files, `set +e` inside the fork so non-zero rc is captured rather than aborting). Transport is `rsync -az --delete` + `ssh` against a persistent Graviton EC2 host (defaults `GRAVITON_HOST=ec2-user@172.31.71.135`, `GRAVITON_KEY=$HOME/ec2-jinmp.pem`, overridable via env), same exclusion set as `.githooks/pre-push`, remote invocation `ninja -j$(nproc) zepto_tests test_feeds test_migration && ctest -j$(nproc) -E "Benchmark\.|K8s" --output-on-failure --timeout 180` — identical ctest regex to stage 2. 3s `ConnectTimeout`/`BatchMode=yes` preflight: on failure the stage prints a `WARN` and skips with rc=0 so local-dev without VPN still passes. Included by default in `--local` (`1,2,8,3,4`), `--eks` (`1,2,8,3,4,6,7`), `--all` (`1,2,8,3,4,5,6,7`); opt out with `--no-arm64` / `--skip-arm64`. Cost \$0 — the Graviton instance is persistent, not owned by the orchestrator. Verified: `--dry-run --local` prints 5-stage plan; `--dry-run --local --no-arm64` prints 4-stage plan; live `--local --keep-going` run on the dev workstation forked PIDs for stages 2 & 8, stage 2 wall 73 s, stage 8 wall 251 s, parallel wall ≈ max(73,251) = 251 s (70 s saving versus serial); unreachable-host preflight test (`GRAVITON_HOST=ec2-user@10.255.255.1`) skipped cleanly with rc=0 in 3 s. CONTRIBUTING.md updated.
- [x] **Full-matrix orchestrator perf optimizations** (devlog 094) — eight code-level speedups to `tools/run-full-matrix.sh` and `tests/k8s/run_eks_bench.sh`. **#2/#8** `run_stages_parallel_many` N-way helper: when stages 2+8+3+4 are all selected (the `--local` default), they all run in a single 4-way parallel fork group instead of 3+4 serializing after 2‖8. **#3** Stage 8 skips `rsync` entirely when `git rev-parse HEAD` matches the last-synced SHA cached at `$HOME/.cache/zepto_matrix/last_sync_<host8>` and the working tree is clean; adds `--info=stats2 --human-readable` to the rsync invocation, and a new `--force-resync` flag that adds `--checksum` and ignores the marker. **#6** Stage 6 (`aarch64_unit` via EKS buildx+ECR) removed from `--eks` (now `1,2,8,3,4,7`) and `--all` (now `1,2,8,3,4,5,7`) shortcuts; still callable via `--stages=6` and marked deprecated in `--help`. Stage 8 now covers the same aarch64 unit suite for free. **#7** Stage 8 remote `ctest --timeout` bumped 180 → 300 s (Graviton dev box is 4c vs local 8c; `WorkerPool.WaitIdle` has been observed to hit 180.02 s). **#9** `tests/k8s/run_eks_bench.sh` parallelizes the amd64 chain (compat→HA+perf) against the arm64 chain (compat+HA+perf) — disjoint namespaces, no cluster-level state contention. **#10** Same script short-circuits the Karpenter arm64 NodePool provision when ≥3 arm64 nodes are already Ready (saves 1–5 min on warm clusters). **#11** Stage 5 fallback bench cap 60 s → 15 s per binary (smoke only; real numbers from `run_arch_bench.sh`). Infra item #1 (Graviton 4c→8c resize, ~3.43× arm64 gap closer) is deferred. Verified dry-runs: `--all`/`--eks`/`--stages=6`/`--local` plans all correct; `bash -n` clean on both scripts; `grep` confirms #9 and #10 are in the k8s script. CONTRIBUTING.md, devlog 092 (stage 6 row deprecated), devlog 093 (see-also footer) all updated.
- [x] **`zepto-bench` `EC2NodeClass` apply-failure — investigation** (devlog 095) — investigation-only: `tests/k8s/run_eks_bench.sh` stage 2 fails with `no matches for kind "EC2NodeClass" in version "karpenter.k8s.aws/v1"`. Initial hypothesis was "self-hosted Karpenter with one CRD missing, install `oci://public.ecr.aws/karpenter/karpenter-crd`". Verified via `aws eks describe-cluster` that `zepto-bench` is actually an **EKS Auto Mode** cluster (`computeConfig.enabled: true`, managed `general-purpose`/`system` NodePools, AutoModeNodeRole); no Karpenter controller pods, no Helm release, the `karpenter.sh` CRDs were installed by Auto Mode itself. Auto Mode ships `nodeclasses.eks.amazonaws.com` (kind `NodeClass`, group `eks.amazonaws.com`) **instead of** `ec2nodeclasses.karpenter.k8s.aws` by design, and the existing `zepto-bench-{arm64,x86}` NodePools already reference it (`group: eks.amazonaws.com, kind: NodeClass, name: default`). Installing `karpenter-crd` was therefore **not performed**: `helm template` showed the chart bundles all three CRDs including the two AWS-managed ones, which would (a) take Helm ownership of AWS-reconciled CRDs currently serving the 3 live nodes and (b) add an `EC2NodeClass` CRD with no controller and no matching IAM role (`KarpenterNodeRole-zepto-bench` does not exist — Auto Mode uses `AutoModeNodeRole-*`). Correct fix is to rewrite `run_eks_bench.sh` stage 2 to drop the `EC2NodeClass` object and point the NodePool at `group: eks.amazonaws.com, kind: NodeClass, name: default` — deferred to a follow-up task because the orchestrator prompt forbade modifying that script. No code or infra change applied in this devlog; verification commands and recommended manifest are captured in the devlog body.
- [x] **Flake tally update — `QueryCoordinator.TwoNodeRemote_OrderByLimit`** (devlog 096) — documentation-only: full-matrix run 2026-04-18 observed 1× flake on arm64 stage 8 for `QueryCoordinator.TwoNodeRemote_OrderByLimit`. Test is already covered by the existing `tcp_rpc_pool` RESOURCE_LOCK via the explicit `QueryCoordinator.TwoNodeRemote_*` enumeration in `tests/serial_tests.cmake` (verified) — no cmake or .cpp change required. Appended post-fix observation subsection to devlog 087; post-fix `tcp_rpc_pool` family tally now at 2 observations (prior: devlog 090 `TwoNodeRemote_DistributedAvg_Correct`).
- [x] **Data Durability** — Intra-day auto-snapshot (60s default), recovery replays on restart — max data loss ≤ 60s

## Ingestion & Feed Handlers
- [x] **Feed Handlers** — FIX, NASDAQ ITCH (350ns parsing)
- [x] **Kafka consumer** — JSON/binary/human-readable decode, backpressure retry, Prometheus metrics, commit modes — 26 tests
- [x] **MQTT consumer** — IoT / Physical AI ingestion, QoS 0/1/2, topic wildcards (`#`, `+`), shared JSON/BINARY/JSON_HUMAN decoders with Kafka, Paho async client with `ZEPTO_USE_MQTT` optional-dep pattern — 18 tests (devlog 081)
- [x] **Connection hooks & session tracking** — on_connect/on_disconnect callbacks, session list, idle eviction, query count — 7 tests

## Python Ecosystem
- [x] **Python Ecosystem** — zepto_py: from_pandas/polars/arrow, ArrowSession, StreamingSession, ApexConnection — 208 tests
- [x] **Python execute()** — Full SQL access (SELECT, INSERT, UPDATE, DELETE, DDL, MV)

## Security & Multi-Tenancy
- [x] **Enterprise Security** — TLS/HTTPS, API Key + JWT/OIDC, RBAC, Rate Limiting, Admin REST API, Query Timeout/Kill, Secrets Management (Vault/File/Env), Audit Log (SOC2/EMIR/MiFID II) — 69 tests
- [x] **Vault-backed API Key Store** — Write-through sync of API keys to HashiCorp Vault KV v2, multi-node key sharing via Vault, graceful degradation when Vault unavailable — 8 tests
- [x] **Multi-tenancy** — TenantManager, per-tenant query concurrency quota, table namespace isolation, usage tracking
- [x] **License validator** — RS256-signed JWT license keys, 2-tier edition system (Community/Enterprise), Feature bitmask gating, env/file/direct key loading, 30-day grace period, `license().hasFeature()` singleton API (devlog 065, 066)
- [x] **Edition foundation** — Startup banner with edition/upgrade hint, trial key support (unsigned JWT, 30-day, single-node), HTTP 402 response standard, `GET /api/license` public endpoint, `GET/POST /admin/license` + `POST /admin/license/trial` admin endpoints (devlog 068)
- [x] **Web UI upgrade prompts** — UpgradeCard component with lock icon + "View Plans" link to zeptodb.com/pricing. Cluster page gated on `cluster` feature, Tenants page gated on `advanced_rbac`. Sidebar shows "Enterprise" chip on gated items. `useLicense()` hook + `fetchLicense()` API client (devlog 071)

## Cluster & HA
- [x] **Cluster Integrity** — Unified PartitionRouter, FencingToken in RPC (24-byte header), split-brain defense (K8s Lease), CoordinatorHA auto re-registration — 13 tests
- [x] **Distributed DML routing** — INSERT routes to symbol node, UPDATE/DELETE broadcast, DDL broadcast
- [x] **RingConsensus (P8-Critical)** — `RingConsensus` abstract interface + `EpochBroadcastConsensus` implementation. Coordinator epoch broadcast synchronizes the ring across all nodes. `RING_UPDATE`/`RING_ACK` RPC messages. `ClusterConfig::is_coordinator` flag. Plugin architecture replaceable with Raft (`set_consensus()`)
- [x] **CoordinatorHA ↔ K8sLease integration (P8-Critical)** — K8sLease acquisition required on standby→active promotion path (`require_lease`), `FencingToken::advance()` + RPC client epoch propagation, automatic demote on lease loss
- [x] **WalReplicator replication guarantee (P8-Critical)** — Quorum write (`quorum_w`), failure retry queue (`max_retries`/`retry_queue_capacity`), backpressure (`backpressure` — producer block), backward compatible with existing async/sync modes
- [x] **Failover data recovery (P8-Critical)** — Auto re-replication built into FailoverManager (`auto_re_replicate`/`async_re_replicate`). PartitionMigrator integration, node registration via `register_node()`, graceful fallback when unregistered
- [x] **Internal RPC security (P8-Critical)** — `RpcSecurityConfig` shared-secret HMAC authentication. AUTH_HANDSHAKE/AUTH_OK/AUTH_REJECT protocol. mTLS configuration structure prepared
- [x] **HealthMonitor DEAD recovery (P8-High)** — `REJOINING` state added (DEAD→REJOINING→ACTIVE). `on_rejoin()` callback for data resynchronization control. Router auto-readds node on REJOINING→ACTIVE transition in ClusterNode
- [x] **HealthMonitor UDP fault tolerance (P8-High)** — Consecutive miss verification (default 3 times), fatal error on bind failure, secondary TCP heartbeat (dual verification with TCP probe before SUSPECT→DEAD transition)
- [x] **TcpRpcServer resource management (P8-High)** — Thread pool conversion (detach→fixed worker pool+task queue), payload size limit (64MB), graceful drain (30-second timeout), concurrent connection limit (1024)
- [x] **PartitionRouter concurrency (P8-High)** — Built-in `ring_mutex_` (shared_mutex). add/remove uses unique_lock, route/plan uses shared_lock. TOCTOU eliminated
- [x] **TcpRpcClient::ping() connection leak (P8-High)** — connect_to_server()+close() → acquire()/release() pool recycling
- [x] **GossipNodeRegistry data race (P8-Medium)** — `bool running_` → `std::atomic<bool>`. Multithreaded UB eliminated
- [x] **K8sNodeRegistry deadlock (P8-Medium)** — `fire_event_unlocked()` removed. Changed to release lock before invoking callbacks
- [x] **ClusterNode node rejoin (P8-Medium)** — Seed connection success count, `std::runtime_error` on total failure. Bootstrap (no seeds) allowed normally
- [x] **SnapshotCoordinator consistency (P8-Medium)** — 2PC (PREPARE→COMMIT/ABORT). Pauses ingest on all nodes then flushes at a consistent point-in-time. ABORT on all nodes on failure. `take_snapshot_legacy()` backward compatible
- [x] **K8sNodeRegistry actual implementation (P8-Medium)** — poll_loop() performs K8s Endpoints API HTTP GET. Auto-detects environment variables, SA token authentication, parse_endpoints_json()+reconcile() diff→JOINED/LEFT events
- [x] **PartitionMigrator atomicity (P8-Medium)** — MoveState state machine (PENDING→DUAL_WRITE→COPYING→COMMITTED/FAILED), MigrationCheckpoint JSON disk persistence (save/load), resume_plan() retry (max_retries=3), rollback_move() — sends DELETE to dest on failure
- [x] **Dual-write ingestion wiring (P8-Feature)** — `ClusterNode::ingest_tick()` checks `migration_target()` before routing; during partition migration, ticks are sent to both source and destination nodes to prevent data loss
- [x] **Live rebalancing (P8-Feature)** — `RebalanceManager` orchestrates zero-downtime partition migration on node add/remove. Background thread with pause/resume/cancel, checkpoint support, sequential move execution via `PartitionMigrator`
- [x] **Load-based auto-rebalancing (P8-Feature)** — `RebalancePolicy` with configurable imbalance ratio, check interval, and cooldown. Background policy thread monitors per-node partition counts via `LoadProvider` callback and auto-triggers `start_remove_node()` on overloaded nodes
- [x] **Rebalance admin HTTP API (P8-Feature)** — 5 REST endpoints (`/admin/rebalance/{status,start,pause,resume,cancel}`) for live rebalance control. Admin RBAC enforced, JSON request/response, 503 when not in cluster mode
- [x] **Rebalance hardening: `peer_rpc_clients_` thread safety (P8-Feature)** — `std::shared_mutex` protects `peer_rpc_clients_` map in `ClusterNode`. `shared_lock` for reads in `remote_ingest()` hot path, `unique_lock` for writes. Race-safe lazy client creation — 1 test
- [x] **Rebalance hardening: move timeout (P8-Feature)** — `move_timeout_sec` in `RebalanceConfig` (default 300s). `PartitionMigrator::execute_move()` wraps `migrate_symbol()` in `std::async` + `wait_for`. On timeout: FAILED + dual-write ended — 2 tests
- [x] **Rebalance hardening: query routing safety (P8-Feature)** — `recently_migrated_` map in `PartitionRouter`. After `end_migration()`, `recently_migrated(symbol)` returns `{from, to}` during grace period (default 30s). Auto-expires. Query layer reads from both nodes during transition — 5 tests
- [x] **Partial-move rebalance API (P8-Feature)** — `start_move_partitions(vector<Move>)` moves specific symbols between existing nodes without full drain. HTTP `move_partitions` action in `/admin/rebalance/start`. No ring topology broadcast — 6 tests
- [x] **Rebalance progress in Web UI (P8-Feature)** — cluster dashboard panel showing live rebalance state, progress bar, completed/failed/total moves, current symbol. Auto-refreshes every 2s via `/admin/rebalance/status`
- [x] **Rebalance history endpoint (P8-Feature)** — `GET /admin/rebalance/history` returns past rebalance events (action, node, moves, duration, cancelled). In-memory ring buffer (max 50). Web UI history table on cluster dashboard — 5 tests
- [x] **Rebalance ring broadcast (P8-Feature)** — `RebalanceManager` calls `RingConsensus::propose_add/remove()` after all moves complete, synchronizing hash ring across all cluster nodes. Skipped on cancel. `set_consensus()` setter, `RebalanceAction` enum — 3 tests
- [x] **Rebalance bandwidth throttling (P8-Feature)** — `BandwidthThrottler` rate-limits partition migration data transfer. Configurable `max_bandwidth_mbps` (0=unlimited). Sliding window with sleep-based backpressure. Thread-safe atomic counters — 10 tests
- [x] **PTP clock sync detection (P8-Feature)** — `PtpClockDetector` checks PTP hardware/chrony/timesyncd synchronization quality. 4 states (SYNCED/DEGRADED/UNSYNC/UNAVAILABLE). `strict_mode` rejects distributed ASOF JOIN on bad sync. `GET /admin/clock` endpoint — 22 tests
- [x] **Rebalance bandwidth throttling (P8-Feature)** — `BandwidthThrottler` rate-limits partition migration data transfer. Sliding 1-second window, thread-safe atomics, runtime adjustable via `set_max_bandwidth_mbps()`. Wired into `PartitionMigrator::migrate_symbol()`. Exposed in `/admin/rebalance/status` JSON — 10 tests

## Operations & Deployment
- [x] **Fast parallel cross-arch EKS test pipeline** (devlog 083) — `run_arch_comparison_fast.sh` replaces the ~60 min sequential script. 8-stage pipeline with fail-fast trap teardown, parallel x86/arm64 Docker builds (buildx local + native on Graviton via SSH), parallel Helm installs on per-arch Karpenter NodePools (`zepto-bench-x86`, `zepto-bench-arm64`), pre-baked `Dockerfile.bench`/`Dockerfile.bench.arm64` images (bench_rebalance + libssl3 baked in, no `kubectl cp`/`apt-get`), ClusterIP service (no ELB). Wall time ~28 min cold, ~\$1.30/run. Auto Mode-compatible `tools/eks-bench.sh` (NodePool CPU limits instead of managed nodegroups)
- [x] **Production operations** — monitoring, backup, systemd service
- [x] **Kubernetes operations** — Helm chart (PDB/HPA/ServiceMonitor), rolling upgrade, K8s operations guide, Karpenter Fleet API
- [x] **K8s Operator** — Bash-based operator for `ZeptoDBCluster` CRD (`zeptodb.com/v1alpha1`). Reconciles CR spec → Helm release. Enterprise license gating for multi-node clusters (secret must exist, mounted as `ZEPTODB_LICENSE_KEY`). RBAC, Deployment, example CRs (devlog 072)
- [x] **ARM Graviton build verification** — aarch64 (Amazon Linux 2023, Clang 19.1.7), 766/766 tests passing, xbar 7.99ms (1M rows)
- [x] **Metrics provider** — pluggable Prometheus metrics, Kafka stats integration — 4 tests
- [x] **Task scheduler** — interval/once jobs, cancel, exception-safe, monotonic clock — 18 tests
- [x] **Multi-node metrics collection** — METRICS_REQUEST/METRICS_RESULT RPC, parallel fan-out, ClusterNode callback registration — 10 tests
- [x] **HTTP observability** — structured JSON access log, slow query log (>100ms), X-Request-Id tracing, server lifecycle events, Prometheus http_requests_total/active_sessions — 2 tests
- [x] **`/whoami` endpoint** — returns authenticated role and subject for reliable client-side role detection — 1 test
- [x] **Web UI cluster page** — node status table, per-node metrics history charts (ingestion/queries/latency), recharts type fix
- [x] **API key granular control** — symbol/table ACL, tenant binding, key expiry, PATCH update endpoint, Web UI create/edit dialogs — 6 tests
- [x] **Query Editor: resizable height (QE-10)** — drag divider between editor and result area, 80–600px range, replaces fixed 180px
- [x] **Query Editor: schema sidebar (QE-6)** — left panel with table/column tree, click to insert into editor, refresh button
- [x] **Query Editor: ZeptoDB function autocomplete (QE-7)** — `xbar`, `vwap`, `ema`, `wma`, `mavg`, `msum`, `deltas`, `ratios`, `fills` + SQL keyword snippets (ASOF JOIN, EXPLAIN, etc.)
- [x] **Query Editor: result chart view (QE-5)** — table/chart toggle (line/bar), X/Y column selectors, Recharts, 500-row cap
- [x] **Query Editor: multi-tab editor (QE-1)** — add/close/rename tabs, independent code & results per tab, localStorage persistence
- [x] **Query Editor: multi-statement run (QE-9)** — `;`-split sequential execution, per-statement result sub-tabs, per-statement error display
- [x] **SSO/JWT CLI + JWKS auto-fetch** — `--jwt-*` / `--jwks-url` CLI flags, JWKS background key rotation, kid-based multi-key, `POST /admin/auth/reload` runtime refresh — 3 tests
- [x] **Bare-metal tuning guide** — CPU pinning, NUMA, hugepages, C-state, tcmalloc/LTO/PGO build, network tuning, benchmarking — `docs/deployment/BARE_METAL_TUNING.md`

## Migration Toolkit
- [x] **Migration toolkit** — kdb+ HDB loader, q→SQL, ClickHouse DDL/query translation, DuckDB Parquet, TimescaleDB hypertable — 126 tests

## Data Types
- [x] **Native float/double** — IEEE 754 float32/float64 in storage, SQL, and HTTP output
- [x] **String symbol (dictionary-encoded)** — `INSERT/SELECT/WHERE/GROUP BY/VWAP/FIRST/LAST` with `'AAPL'` syntax, LowCardinality dictionary encoding, distributed scatter-gather support — 29 tests

## Connectivity
- [x] **Arrow Flight server (P3)** — gRPC-based Arrow Flight RPC: DoGet (SQL→RecordBatch stream), DoPut (ingest), GetFlightInfo, ListFlights, DoAction (ping/healthcheck). Python `pyarrow.flight.connect("grpc://host:8815")` for remote zero-copy-grade streaming. Stub mode when built without Flight. — 7 tests

## Documentation — Getting Started & Onboarding
- [x] **Quick Start Guide** — 5-minute onboarding: Docker → INSERT → SELECT → Python → Web UI
- [x] **Interactive Playground design** — Browser-based sandboxed SQL editor with preloaded datasets, session isolation, rate limiting
- [x] **Example Dataset Bundle design** — `--demo` flag: 350K rows (trades/quotes/sensors), deterministic generation, starter queries on stdout

## Query Editor Enhancements (Phase 2)
- [x] **Dark/light theme toggle (QE-11)** — CodeMirror theme syncs with MUI palette mode (TopBar toggle)
- [x] **Result column sorting (QE-13)** — click column header to cycle ASC/DESC/none, arrow indicators, numeric-aware sort
- [x] **Result column filtering (QE-14)** — per-column text filter row (toggle via filter icon), case-insensitive, match count display
- [x] **Query history search & pin (QE-2)** — search input in history panel, pin/unpin toggle, pinned items sorted to top, localStorage persistence
- [x] **Saved queries (QE-3)** — name + save to localStorage, load/delete from Saved panel, separate from history
- [x] **Syntax error inline marker (QE-8)** — parse error line from server response, highlight error line in CodeMirror with red decoration
- [x] **Query execution cancel (QE-12)** — AbortController-based cancellation, Run button becomes Cancel while loading, abort signal passed to fetch
- [x] **Execution time history sparkline (QE-15)** — SVG sparkline of last 20 query execution times, displayed in result header
- [x] **EXPLAIN visualization (QE-4)** — EXPLAIN results rendered as visual tree with colored operation/path/table nodes (+ server fix: string_rows JSON serialization for EXPLAIN/DDL)
- [x] **Table detail page (`/tables/[name]`)** — dedicated route with schema, column stats (min/max), row count cards, data preview; tables list navigates on click
- [x] **Settings page enhancement** — server info section (engine version, build date, health status) alongside runtime config
- [x] **Login page polish** — gradient accent, tagline chip, keyboard hint, Quick Start link, footer branding

## Web UI — Dashboard & Overview (P1)
- [x] **Dashboard overview page** — Health status, version info, 5 stat cards (ingested/stored/queries/partitions/latency), drop rate warning, ingestion rate live chart, tables summary with row counts, rows-per-table bar chart, avg query cost
- [x] **Cluster status dashboard** — Node topology ring visualization, partition distribution pie chart, node health table with store ratio bars, ticks-stored bar chart, time-series charts (ingestion/queries/latency per node), drop rate alert
- [x] **Dashboard as default landing** — `/` redirects to `/dashboard`, Dashboard first in sidebar, visible to all roles (admin/writer/reader/analyst/metrics)

## Bug Fixes
- [x] **API client template literal fix** — Fixed broken string literals in `api.ts` and `auth.tsx` (backtick+double-quote mix from `API` variable introduction), all fetch URLs now use proper template literals with `${API}` prefix
- [x] **API URL consistency** — All API calls use configurable `API` base path constant, supports both same-origin (Docker) and proxy (Next.js dev) modes

## Website & Docs (P2)
- [x] **Docs site (docs.zeptodb.com)** — mkdocs-material deployment
- [x] **Docs nav update** — Added 40+ missing pages (devlog 024-040, Flight API, multinode_stability, etc.)
- [x] **Performance comparison page** — vs kdb+/ClickHouse/TimescaleDB benchmark charts

## SEO & Community (P2)
- [x] **SEO basics** — sitemap, Open Graph, meta tags (mkdocs-material auto-generated)
- [x] **GitHub README renewal** — badges with logos, architecture diagram, emoji sections, GIF demo placeholder, navigation links, community section, updated test count (830+)
- [x] **Community infrastructure** — CONTRIBUTING.md, CODE_OF_CONDUCT.md, GitHub Issue templates (bug/feature/perf), FUNDING.yml
- [x] **Community setup guide** — Discord server structure (channels/roles/bots), GitHub Discussions categories — `docs/community/COMMUNITY_SETUP.md`
- [x] **Registry submission content** — Awesome Time-Series DB PR text, DB-Engines form data, DBDB/AlternativeTo/StackShare — `docs/community/REGISTRY_SUBMISSIONS.md`
- [x] **Launch post drafts** — Show HN, Reddit (r/programming, r/cpp, r/algotrading, r/selfhosted), timing strategy, launch day checklist — `docs/community/LAUNCH_POSTS.md`
- [x] **Discord server created** — Server ID 1492174712359354590, invite link https://discord.gg/zeptodb
- [x] **Discord links added to Web UI** — Join Discord button on home page, Discord link in sidebar

## SSO / Identity Enhancement (P6)
- [x] **OIDC Discovery** — `OidcDiscovery::fetch(issuer_url)` auto-populates jwks_uri, authorization/token endpoints from `/.well-known/openid-configuration`. AuthManager auto-registers IdP + JWT validator — 2 tests
- [x] **Server-side sessions** — `SessionStore` with cookie-based session management. Configurable TTL (1h default), sliding window refresh, HttpOnly/SameSite cookies. `AuthManager::check_session()` resolves cookie → AuthContext — 10 tests
- [x] **Web UI SSO login flow** — OAuth2 Authorization Code Flow: `/auth/login` (redirect to IdP), `/auth/callback` (code exchange → session cookie → redirect), `/auth/session` (Bearer → session), `/auth/logout`, `/auth/me`. Web UI "Sign in with SSO" button enabled, session-aware auth provider — 3 tests
- [x] **JWT Refresh Token** — `OAuth2TokenExchange::refresh()` exchanges refresh_token for new access_token. `POST /auth/refresh` server endpoint. Session store tracks refresh_token per session. Web UI `useAuth().refresh()` hook — 4 tests

## Engine Performance (P7 Tier A)
- [x] **Composite index (index intersection)** — Multi-predicate WHERE queries now combine all applicable s#/g#/p# indexes via intersection instead of single-winner waterfall. `IndexResult` accumulator intersects ranges and row sets. Applied to `exec_simple_select`, `exec_agg`, `exec_group_agg`. Zero regression on single-predicate queries — devlog 067
- [x] **INTERVAL syntax** — `INTERVAL 'N unit'` in SELECT and WHERE expressions. Supports seconds/minutes/hours/days/weeks/ms/μs/ns. Evaluates to nanoseconds. Works with `NOW() - INTERVAL '5 minutes'` in WHERE clauses — 3 tests
- [x] **Prepared statement cache** — Parsed AST cached by SQL hash (up to 4096 entries). Eliminates tokenize+parse overhead (~2-5μs) on repeated queries. Thread-safe with `clear_prepared_cache()` API — 1 test
- [x] **Query result cache** — TTL-based result cache for SELECT queries. `enable_result_cache(max_entries, ttl_seconds)`. Auto-invalidated on INSERT/UPDATE/DELETE. Oldest-entry eviction when full — 2 tests
- [x] **SAMPLE clause** — `SELECT * FROM trades SAMPLE 0.1` reads ~10% of rows. Deterministic hash-based sampling (splitmix64) for reproducible results. Works with WHERE, GROUP BY, aggregation. Shown in EXPLAIN plan — 8 tests
- [x] **Scalar subqueries in WHERE** — `WHERE price > (SELECT avg(price) FROM trades)` and `WHERE symbol IN (SELECT symbol FROM ...)`. Uncorrelated subqueries evaluated once and substituted as literals before outer scan. IN results auto-deduplicated. Error on multi-row/multi-column scalar subqueries — 8 tests
- [x] **JIT SIMD emit** — `compile_simd()` generates explicit `<4 x i64>` vector IR (256-bit). Vector load/compare, `bitcast <4 x i1>→i4`, cttz mask extraction loop. Scalar tail for remainder (n%4). Reuses existing AST parser — 8 tests (devlog 079)

## Package Distribution (P2)
- [x] **Docker Hub official image** — `docker pull zeptodb/zeptodb:0.0.1`. GitHub Actions workflow (`docker-publish.yml`) builds on tag push (`v*`) or manual dispatch. Multi-stage build, non-root user, health check endpoint
- [x] **GitHub Releases + binaries** — Release workflow builds amd64 + arm64 tarballs, creates GitHub Release with download links on tag push
- [x] **Homebrew Formula** — `homebrew-tap` repo with auto-update workflow triggered on release via repository_dispatch

## CI/CD (P2)
- [x] **Node.js 24 migration** — All workflows set `FORCE_JAVASCRIPT_ACTIONS_TO_NODE24=true` to preempt June 2026 deprecation
- [x] **Deprecated docs.yml cleanup** — Removed legacy MkDocs deploy workflow (replaced by Astro Starlight site)
- [x] **TestPyPI workflow fix** — Changed `test-pypi.yml` to target TestPyPI (test.pypi.org) with separate `testpypi` environment

## Website (P2)
- [x] **Product website** — Astro Starlight site (`zeptodb-site/`). Landing page with hero + benchmark comparison table + use case cards + CTA
- [x] **Features page** — Ingestion engine, query engine, storage, client APIs, security, clustering, deployment
- [x] **Benchmarks page** — Hardware specs, ingestion throughput, query latency, Python zero-copy numbers
- [x] **Use Cases (4 pages)** — Trading & Finance, IoT, Robotics, Autonomous Vehicles with architecture diagrams and SQL examples
- [x] **Competitor comparisons (4 pages)** — vs kdb+, vs ClickHouse, vs InfluxDB, vs TimescaleDB
- [x] **Pricing page** — Community (Free/OSS) vs Enterprise tiers with FAQ
- [x] **Blog (4 posts)** — Introducing ZeptoDB, How ASOF JOIN Works, Zero-Copy Python (522ns), Lock-Free Ingestion (5.52M/sec)
- [x] **About / Contact / Community pages** — Mission, tech philosophy, contributing guide, roadmap
- [x] **Security page** — TLS, Auth, RBAC, Rate Limiting, Audit, Compliance matrix (SOC2/MiFID II/GDPR/PCI)
- [x] **Integrations page** — Feed handlers, client libraries, monitoring, storage/cloud, auth providers, roadmap integrations
- [x] **Docs site deployment automation** — GitHub Actions `build-deploy.yml` (push + repository_dispatch), `sync-docs.mjs` for ZeptoDB docs sync
- [x] **Custom header navigation** — Product/Solutions/Docs/Pricing/Community top nav with GitHub Stars badge

---

## Kubernetes Compatibility & HA Testing
- [x] **K8s compatibility test suite** — 27 automated tests covering Helm lint/template, pod lifecycle, networking, rolling updates, PDB, scale up/down (`tests/k8s/test_k8s_compat.py`)
- [x] **K8s HA + performance test suite** — 6 HA tests (3-node spread, node drain, concurrent drain PDB block, pod kill recovery, zero-downtime rolling update, scale 3→5→3) + 5 performance benchmarks (`tests/k8s/test_k8s_ha_perf.py`)
- [x] **EKS test cluster config** — Lightweight cluster definition for automated testing (`tests/k8s/eks-compat-cluster.yaml`)
- [x] **K8s test report** — Full results, benchmark numbers, Helm chart issues found (`docs/operations/K8S_TEST_REPORT.md`)

---

## Live Rebalancing Load Test
- [x] **bench_rebalance binary** — HTTP-based load test measuring rebalance impact on throughput/latency (`tests/bench/bench_rebalance.cpp`)
- [x] **Helm rebalance config** — bench-rebalance-values.yaml with RebalanceManager enabled (`deploy/helm/bench-rebalance-values.yaml`)
- [x] **Orchestration script** — Automated test execution on EKS (`deploy/scripts/run_rebalance_bench.sh`)
- [x] **Benchmark guide** — Prerequisites, execution, expected results, cost estimate (`docs/bench/rebalance_benchmark_guide.md`)

---

## Test Infrastructure
- [x] **Parallel test execution (`ctest -j$(nproc)`)** — Removed static `/tmp/zepto_test_*` path collisions via `zepto_test_util::unique_test_path()`; serialised `HttpClusterHA.*` suite. 1364/1364 pass, ~7.5× faster than `-j1` on 8 cores (`docs/devlog/087_parallel_test_execution.md`)

---

> Client API Compatibility Matrix: [`docs/design/client_compatibility.md`](docs/design/client_compatibility.md)
 ❌ | ❌ | ✅ `/admin/audit` | ❌ | ❌ | ✅ `/admin/audit` | ❌ | ❌ | ✅ `/admin/audit` |
