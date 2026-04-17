# ZeptoDB — Completed Features

Last updated: 2026-04-16

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
- [x] **Data Durability** — Intra-day auto-snapshot (60s default), recovery replays on restart — max data loss ≤ 60s

## Ingestion & Feed Handlers
- [x] **Feed Handlers** — FIX, NASDAQ ITCH (350ns parsing)
- [x] **Kafka consumer** — JSON/binary/human-readable decode, backpressure retry, Prometheus metrics, commit modes — 26 tests
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

> Client API Compatibility Matrix: [`docs/design/client_compatibility.md`](docs/design/client_compatibility.md)
 ❌ | ❌ | ✅ `/admin/audit` | ❌ | ❌ | ✅ `/admin/audit` | ❌ | ❌ | ✅ `/admin/audit` |
