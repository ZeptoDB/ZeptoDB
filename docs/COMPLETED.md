# ZeptoDB — Completed Features

Last updated: 2026-03-27

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
- [x] **Window functions** — EMA, DELTA, RATIO, SUM, AVG, MIN, MAX, LAG, LEAD, ROW_NUMBER, RANK, DENSE_RANK
- [x] **Financial functions** — xbar, FIRST, LAST, Window JOIN, UNION JOIN (uj), PLUS JOIN (pj), AJ0

## Query Execution
- [x] **Parallel query** — LocalQueryScheduler (scatter/gather, 3.48x@8T), CHUNKED mode
- [x] **Time range index** — O(log n) binary search within partitions, O(1) partition skip
- [x] **Sorted column index** — `p#`/`g#` style sorted attribute, O(log n) binary search range scan, 269x vs full scan — 13 tests
- [x] **Materialized View** — CREATE/DROP MATERIALIZED VIEW, incremental aggregation on ingest, OHLCV/SUM/COUNT/MIN/MAX/FIRST/LAST, xbar time bucket

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
- [x] **Multi-tenancy** — TenantManager, per-tenant query concurrency quota, table namespace isolation, usage tracking

## Cluster & HA
- [x] **Cluster Integrity** — Unified PartitionRouter, FencingToken in RPC (24-byte header), split-brain defense (K8s Lease), CoordinatorHA auto re-registration — 13 tests
- [x] **Distributed DML routing** — INSERT routes to symbol node, UPDATE/DELETE broadcast, DDL broadcast
- [x] **RingConsensus (P8-Critical)** — `RingConsensus` 추상 인터페이스 + `EpochBroadcastConsensus` 구현. Coordinator epoch broadcast로 전체 노드 ring 동기화. `RING_UPDATE`/`RING_ACK` RPC 메시지. `ClusterConfig::is_coordinator` 플래그. Raft 교체 가능한 플러그인 구조 (`set_consensus()`)
- [x] **CoordinatorHA ↔ K8sLease 통합 (P8-Critical)** — promote 경로에서 K8sLease 획득 필수화 (`require_lease`), `FencingToken::advance()` + RPC client epoch 전파, lease 상실 시 자동 demote
- [x] **WalReplicator 복제 보장 (P8-Critical)** — Quorum write (`quorum_w`), 실패 재시도 큐 (`max_retries`/`retry_queue_capacity`), 백프레셔 (`backpressure` — producer block), 기존 async/sync 하위 호환
- [x] **Failover 데이터 복구 (P8-Critical)** — FailoverManager에 auto re-replication 내장 (`auto_re_replicate`/`async_re_replicate`). PartitionMigrator 통합, `register_node()`로 노드 등록, 미등록 시 graceful fallback
- [x] **내부 RPC 보안 (P8-Critical)** — `RpcSecurityConfig` shared-secret HMAC 인증. AUTH_HANDSHAKE/AUTH_OK/AUTH_REJECT 프로토콜. mTLS 설정 구조 준비
- [x] **HealthMonitor DEAD 복구 (P8-High)** — `REJOINING` 상태 추가 (DEAD→REJOINING→ACTIVE). `on_rejoin()` 콜백으로 데이터 재동기화 제어. ClusterNode에서 REJOINING→ACTIVE 시 라우터 자동 재추가
- [x] **HealthMonitor UDP 내결함성 (P8-High)** — 연속 miss 확인 (기본 3회), bind 실패 시 fatal error, 보조 TCP heartbeat (SUSPECT→DEAD 전이 전 TCP probe 이중 확인)
- [x] **TcpRpcServer 리소스 관리 (P8-High)** — 스레드 풀 전환 (detach→고정 워커 풀+작업 큐), payload 크기 상한 (64MB), graceful drain (30초 타임아웃), 동시 연결 수 제한 (1024)
- [x] **PartitionRouter 동시성 (P8-High)** — `ring_mutex_` (shared_mutex) 내장. add/remove는 unique_lock, route/plan은 shared_lock. TOCTOU 제거
- [x] **TcpRpcClient::ping() 연결 누수 (P8-High)** — connect_to_server()+close() → acquire()/release() 풀 재활용
- [x] **GossipNodeRegistry data race (P8-Medium)** — `bool running_` → `std::atomic<bool>`. 멀티스레드 UB 제거
- [x] **K8sNodeRegistry 데드락 (P8-Medium)** — `fire_event_unlocked()` 삭제. lock 해제 후 콜백 호출로 변경
- [x] **ClusterNode 노드 재합류 (P8-Medium)** — seed 연결 성공 카운트, 전체 실패 시 `std::runtime_error`. bootstrap(seeds 없음) 정상 허용
- [x] **SnapshotCoordinator 일관성 (P8-Medium)** — 2PC (PREPARE→COMMIT/ABORT). 전 노드 ingest 일시 중단 후 일관된 시점 flush. 실패 시 전 노드 ABORT. `take_snapshot_legacy()` 하위 호환
- [x] **K8sNodeRegistry 실제 구현 (P8-Medium)** — poll_loop()에서 K8s Endpoints API HTTP GET. 환경변수 자동 감지, SA token 인증, parse_endpoints_json()+reconcile() diff→JOINED/LEFT 이벤트
- [x] **PartitionMigrator 원자성 (P8-Medium)** — MoveState 상태 머신 (PENDING→DUAL_WRITE→COPYING→COMMITTED/FAILED), MigrationCheckpoint JSON 디스크 영속화 (save/load), resume_plan() 재시도 (max_retries=3), rollback_move() — 실패 시 dest에 DELETE 전송

## Operations & Deployment
- [x] **Production operations** — monitoring, backup, systemd service
- [x] **Kubernetes operations** — Helm chart (PDB/HPA/ServiceMonitor), rolling upgrade, K8s operations guide, Karpenter Fleet API
- [x] **ARM Graviton build verification** — aarch64 (Amazon Linux 2023, Clang 19.1.7), 766/766 tests passing, xbar 7.99ms (1M rows)
- [x] **Metrics provider** — pluggable Prometheus metrics, Kafka stats integration — 4 tests
- [x] **Task scheduler** — interval/once jobs, cancel, exception-safe, monotonic clock — 18 tests
- [x] **Multi-node metrics collection** — METRICS_REQUEST/METRICS_RESULT RPC, parallel fan-out, ClusterNode callback registration — 10 tests
- [x] **HTTP observability** — structured JSON access log, slow query log (>100ms), X-Request-Id tracing, server lifecycle events, Prometheus http_requests_total/active_sessions — 2 tests
- [x] **`/whoami` endpoint** — returns authenticated role and subject for reliable client-side role detection — 1 test
- [x] **Web UI cluster page** — node status table, per-node metrics history charts (ingestion/queries/latency), recharts type fix
- [x] **API key granular control** — symbol/table ACL, tenant binding, key expiry, PATCH update endpoint, Web UI create/edit dialogs — 6 tests
- [x] **SSO/JWT CLI + JWKS auto-fetch** — `--jwt-*` / `--jwks-url` CLI flags, JWKS background key rotation, kid-based multi-key, `POST /admin/auth/reload` runtime refresh — 3 tests

## Migration Toolkit
- [x] **Migration toolkit** — kdb+ HDB loader, q→SQL, ClickHouse DDL/query translation, DuckDB Parquet, TimescaleDB hypertable — 126 tests

## Data Types
- [x] **Native float/double** — IEEE 754 float32/float64 in storage, SQL, and HTTP output
- [x] **String symbol (dictionary-encoded)** — `INSERT/SELECT/WHERE/GROUP BY/VWAP/FIRST/LAST` with `'AAPL'` syntax, LowCardinality dictionary encoding, distributed scatter-gather support — 29 tests

---

> Client API Compatibility Matrix: [`docs/design/client_compatibility.md`](docs/design/client_compatibility.md)
 ❌ | ❌ | ✅ `/admin/audit` | ❌ | ❌ | ✅ `/admin/audit` | ❌ | ❌ | ✅ `/admin/audit` |
