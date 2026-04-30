# ZeptoDB Backlog

> Completed features: [`COMPLETED.md`](COMPLETED.md) | 1292 tests passing
>
> Last cleaned: 2026-04-30

> ✅ 2026-04-30: **DDL replication across cluster pods** (devlog 112). `QueryCoordinator::forward_ddl_to_remotes()` fire-and-forget replicates `CREATE / DROP / ALTER TABLE` to every remote pod after the HTTP server executes the DDL locally. Uses the already-parsed `cached_ps` (devlog 091 F4) for classification — no extra parse, no string matching. Remote failures emit `ZEPTO_WARN` but never fail the client request. Known limitation: not strongly consistent — a pod that is down during DDL misses the change until the next DDL or restart; acceptable for the benchmark-harness use case that motivated the fix. 4 new `DDLReplication.*` tests (1288 → 1292). **Closes P8-DDL-replication.**

> ✅ 2026-04-30: **HTTP INSERT cluster-aware routing wired + verified on EKS** (devlog 111 + `docs/bench/results_multinode.md` Round 2). `CoordinatorRoutingAdapter` bridges `QueryExecutor::set_cluster_node()` to the existing `QueryCoordinator` + peer `TcpRpcClient` pool in `tools/zepto_http_server.cpp`. Router unified — `RebalanceManager` now mutates `coordinator->router()` in place (no more stale ring view). Non-HA cluster mode now starts a peer `TcpRpcServer` on `port + 100` for forwarded ticks. `zepto_data_node` documented as a leaf node (no wire-up needed). 4 new `CoordinatorRoutingAdapter.*` tests (1284 → 1288). **EKS stage-3 verification:** routing distributes per hash ring (30/49/21 at N=3 vs 100/0/0 before); HTTP-curl aggregate throughput drops at N≥2 because curl-per-INSERT is latency-bound by TCP RPC hops (benchmark-driver limitation, not engine limitation — see results Round 2). DDL-replication gap surfaced as separate Tier-2 item. **Closes P8-I3-wire.**

> ✅ 2026-04-27: **OPC-UA Sprint 3 — Tier-3 observability closeout** shipped (devlog 110). Three Tier-3 items (2r atomic-stats audit, 2s `RpcClientBase` extraction closing `route_remote` unit-test coverage across all three consumers, 2t Kafka/MQTT/OPC-UA microbench parity — Kafka 2.69 M / MQTT 2.48 M / OPC-UA 6.69 M ticks/s cheap-path) plus three Sprint-2 polish items (explicit `decode_errors` for unsupported variants, devlog-107 wording, reconnect test comment). OPC-UA connector is now SLA-grade.

> ✅ 2026-04-26: **OPC-UA Sprint 2 — first-commercial-ready closeout** shipped (devlogs 107, 108, 109). Four Tier-2 items: Basic256Sha256 security (2c), integration test against open62541 tutorial server (2k), reconnect / failover with exponential backoff + jitter (2i), UA StatusCode → `TickMessage.volume` quality mapping (2j). OPC-UA connector is now ready for first-commercial industrial deployments (Samsung / SK / TSMC / POSCO sectors).

> ✅ 2026-04-26: **OPC-UA Sprint 1 — production-blocker closeout** shipped (devlogs 105, 106). Five Tier-1 items in one sprint: real open62541 `UA_Client` integration (2b), float-safety clamp in `coerce_variant_to_int64` (2n), duplicate/empty `node_id` validation (2p), sector-aware profiles Fab/Auto/Steel/Generic (2q), reconnect/timeout config knobs (2o). OPC-UA connector is now pilot-ready for industrial deployments — Sprint 2 closeout above brings it to first-commercial-ready.

> ✅ 2026-04-26: **Pod placement hardening** shipped (devlog 104). Helm `podAntiAffinity.required: true` default + `topologySpreadConstraints` (`maxSkew: 1`) + ingest-tuned resource defaults. Closes P8-I3-placement: soft `preferredDuringScheduling` was silently co-locating replicas on HPA scale-out, halving ingest CPU scaling and breaking failure isolation. `required: false` toggle and full sector-sized sizing table documented in `docs/operations/KUBERNETES_OPERATIONS.md`.

> ✅ 2026-04-18: **Fast parallel cross-arch EKS test pipeline** shipped (devlog 083). `run_arch_comparison_fast.sh` replaces the legacy sequential script: ~28 min cold, ~\$1.30/run, fail-fast teardown, Auto Mode-compatible.
>
> ✅ 2026-04-19: **Stage 7 / EKS Auto Mode fully green** (devlog 100, closes devlog 095 follow-up). Both parts of the fix landed: (1) `tests/k8s/run_eks_bench.sh` stopped applying the Karpenter self-managed `EC2NodeClass` — now reuses the persistent `zepto-bench-arm64` NodePool via `eks-bench.sh wake` + trigger Deployment; (2) `zepto-bench-{x86,arm64}` NodePools recreated with Auto-Mode-compatible `eks.amazonaws.com/instance-family` + `eks.amazonaws.com/instance-cpu` keys (were `karpenter.k8s.aws/instance-*`, which Auto Mode rejects). Live `./tools/run-full-matrix.sh --stages=7` run: 76/76 green (amd64 compat 27/27 + amd64 HA+perf 11/11 + arm64 all 38/38), 465 s wall, 0 flakes, 0 ZeptoDB regressions, cluster back asleep via global EXIT trap. First-ever fully green amd64+arm64 K8s matrix under Auto Mode.

---

## P2 — "A Product People Can Find"

### Content & Onboarding

| Task | Why | Effort |
|------|-----|--------|
| **YouTube / Loom demo video** | 2-minute demo. Embed in README + landing page. 3x conversion vs text | S |

### Manual TODO (requires manual execution)

| Task | Guide Document | Prerequisites | Effort |
|------|---------------|---------------|--------|
| **Submit DB-Engines registration form** | `docs/community/REGISTRY_SUBMISSIONS.md` | zeptodb.com live | XS |
| **Record demo GIF** | `asciinema rec` → convert to GIF → uncomment in README | Docker image | XS |
| **Post Show HN** | `docs/community/LAUNCH_POSTS.md` | Docker image + website + Discord | XS |
| **Post on Reddit (5 subreddits)** | `docs/community/LAUNCH_POSTS.md` | 1-2 day gap after Show HN | XS |

> ✅ Done: Website, Docs site, Blog (52 posts, full devlog migration), Docker, GitHub Releases, Homebrew, PyPI (v0.0.3), GitHub Discussions, Awesome Time-Series DB PR (#122)

---

## P2.5 — Monetization (Edition System)

| Task | Where | Effort |
|------|-------|--------|
| **Geo-replication gate** | Gate when implemented | — |

> ✅ Done: License validator, Feature gate, Trial keys, Startup banner, SSO/Audit/RBAC/Kafka/Cluster/Migration/Rolling Upgrade gates, Web UI upgrade prompts, HTTP 402 standard, /admin/license endpoints, K8s operator with license gating

---

## P4 — Existing Tool Integration

| Task | Why | Effort |
|------|-----|--------|
| **ClickHouse wire protocol** | Native connection for DBeaver, DataGrip, Grafana | L |
| **JDBC/ODBC drivers** | Tableau, Excel, Power BI | L |

---

## P5 — Data Pipelines

| Task | Why | Effort |
|------|-----|--------|
| **Kafka Connect Sink** | Enterprise data pipeline standard | M |
| **CDC connector** | PostgreSQL/MySQL → real-time sync | M |
| **AWS Kinesis consumer** | AWS-native streaming | S |
| **Apache Pulsar consumer** | Kafka alternative | S |

---

## P6 — Enterprise / Cloud

| Task | Why | Effort |
|------|-----|--------|
| **Cloud Marketplace** | AWS/GCP one-click deployment | M |
| **Geo-replication** | Multi-region, global trading desks | L |
| **SAML 2.0 support** | For SAML-only environments such as banks/insurance | L |

> ✅ Done: Vault-backed API Key Store, OIDC Discovery, SSO login flow, JWT Refresh, Server-side sessions, IdP group→role mapping

---

## P7 — Performance / Engine

| Task | Engine Impact | Effort |
|------|---------------|--------|
| ~~**Table-scoped partitioning**~~ | ✅ Done (devlog 082) — PartitionKey `(table_id, symbol_id, hour_epoch)`; `SELECT * FROM empty_table` returns 0 rows; 7 new tests | — |
| ~~**Cost-based planner**~~ | ✅ Phase 1-7 done (devlog 066-067, 075) — TableStatistics + CostModel + LogicalPlan + PhysicalPlan + EXPLAIN v2 + Wiring (HASH_JOIN build side), 47 tests | — |
| **JOINs/Window on virtual tables** | 🟠 Moderate | M |
| **VWAP 1M p50 sub-600 µs restore** | Inherent residual after 097+098 recovery (625→697 µs median, +11.5% vs realistic rebuilt baseline; +20% vs 582 µs best-case). 582→625 µs gap was best-case-run artefact. Root cause: clang-19 register allocator spills the `v_sum` int64 accumulator to stack under multi-table `query_vwap`'s raised register pressure — `query_vwap` inner loop is byte-identical to baseline (36 instr/iter), only the allocator decision differs. Bounded recovery path: ~30 LOC kernel extraction to standalone `[[gnu::hot, gnu::flatten]] execution::vwap_fused(const int64_t*, const int64_t*, size_t)` behind the `[[unlikely]]` HDB branch; estimated +25 µs recovery. PartitionKey hash/eq was packed-uint64 + splitmix64 in 098 — confirmed <0.01% of profile, no query-path benefit but no regression on ingest. Not in urgency-track P7 scope; revisit if perf pressure rises. (devlog 097, 098) | M |
| ~~**BENCH 1 ingest peak throughput restore**~~ | ✅ Done (devlog 099) — `store_tick` column-pointer caching closes the post-multi-table ingest regression. Root cause was `Partition::get_column(const std::string&)` being called 6×/tick (vs 4× baseline) due to FLOAT64 branch re-lookup, pushing `__memcmp_evex_movbe` from 14.42% → 20.85% of samples. Fix: cache 4 `ColumnVector*` locals in `src/core/pipeline.cpp::store_tick` (10 LOC, no header/API/test change). x86 5-run medians recovered: batch=1 4.76 / batch=64 **5.06** / batch=512 5.05 / batch=4096 5.05 / batch=65535 5.04 M t/s — all within ±2% of `875a4c3` baseline (batch=64 = −0.8%). Graviton 3-run medians 3.97–4.03 M t/s. 1361/1361 tests pass on both arches. Remaining ingest residuals (`clock_gettime` vdso, arena first-touch `memset`) are inherent. | — |
| ~~**SIMD-ify WindowJoin aggregate loop**~~ | ✅ Done (devlog 080) — Contiguous fast-path + sum_i64() SIMD for SUM/AVG, gather+SIMD for large non-contiguous, scalar fallback for small windows — 10 tests | — |
| ~~**JIT SIMD emit**~~ | ✅ Done (devlog 079) — Explicit `<4 x i64>` vector IR generation in LLVM JIT, cttz mask extraction, scalar tail | — |
| ~~**DuckDB embedding**~~ | ✅ Done (devlog 076) — Embedded DuckDB engine, Arrow bridge, Parquet offload, `duckdb()` table function | — |
| ~~**P7-I1 — `drain_threads` auto default**~~ | ✅ Done (devlog 102) — Sentinel `0` in `PipelineConfig::drain_threads` resolves to `max(2, hw_concurrency()/4)` at `start()`; explicit `>=1` values honored verbatim; raises single-pod ingest ceiling by lifting the single-drain-thread cap without any architectural change. Exposed as `pipeline.drainThreads` in Helm + `--drain-threads` CLI flag. | — |
| ~~**P7-I2 — Configurable ring-buffer capacity**~~ | ✅ Done (devlog 102) — New `PipelineConfig::ring_buffer_capacity` (default `65536` via `0` sentinel; power of two in `[4096, 16 777 216]`) with new `MPMCRingBufferDynamic<T>` runtime-sized twin. Absorbs ingest bursts before the synchronous `store_tick()` fallback (~34× cliff, 6.6 M/s → 197 K/s). Exposed as `pipeline.ringBufferCapacity` in Helm + `--ring-buffer-capacity` CLI flag; invalid values throw at construction. | — |
| **Limited DSL AOT compilation** | — | M |

> ✅ Done: Composite index, MV query rewrite, INTERVAL, Prepared statements, Query result cache, SAMPLE, Scalar subqueries, FlatHashMap joins, DuckDB embedding, Table-scoped partitioning (devlog 082)

---

## P8 — Cluster

### P8-RDMA — Transport Layer

| Task | Impact | Effort |
|------|--------|--------|
| **WAL replication RDMA PUT** | TCP ~50μs → RDMA ~1-2μs | M |
| **Remote column scan RDMA GET** | Zero DataNode CPU overhead for scatter-gather | L |
| **Partition migration RDMA GET** | Zero service impact during live rebalancing | M |
| **Failover re-replication RDMA GET** | Minimize replica overhead on node failure | M |
| **`remote_ingest_regions_` wire-up** | Actual connection of RDMA ingest path | S |

### P8-Feature — Remaining

| Task | Why | Effort |
|------|-----|--------|
| **Tier C cold query offload** | Historical data → DuckDB on S3 | M |
| **Global symbol registry** | Distributed string symbol routing | M |

### P8-Ingest — Horizontal ingest scale-out (Phase 2 of devlog 102)

Phase 1 (devlog 102, ✅ P7-I1 + P7-I2) lifted the single-pod ingest
ceiling by exposing `drain_threads` + `ring_buffer_capacity`. Phase 2
scales ingest past a single pod by splitting it into its own stateless
tier.

| Task | Why | Effort |
|------|-----|--------|
| ~~**P8-I3-prep — Cluster-aware `INSERT` routing**~~ ✅ (devlog 103) — HTTP/SQL/Python INSERT now dispatches via `ClusterNodeBase::ingest_tick` when `QueryExecutor::set_cluster_node()` is wired, restoring the one-owner-per-partition invariant across pods. Prerequisite for P8-I3. | Prerequisite unblocked | — |
| ~~**P8-I3-placement — Pod placement hardening**~~ ✅ (devlog 104) — Helm `podAntiAffinity.required: true` default + `topologySpreadConstraints` (`maxSkew: 1`) + ingest-tuned resource defaults. Prevents silent co-location that halved ingest CPU scaling and broke failure isolation on HPA scale-out. | Scale-out model validated | — |
| ~~**P8-I3-wire — `zepto_http_server` set_cluster_node wire-up**~~ ✅ (devlog 111) — `CoordinatorRoutingAdapter` bridges `QueryExecutor::set_cluster_node()` to the existing `QueryCoordinator` `PartitionRouter` + a peer `TcpRpcClient` pool. Router unified (rebalance manager now mutates `coordinator->router()` in place). Non-HA cluster mode starts a `TcpRpcServer` on `port + 100` so peers can forward ticks. `zepto_data_node` confirmed as a leaf node — no wire-up needed. 4 new unit tests (1284 → 1288). EKS re-bench pending as stage 3. | Unblocks horizontal INSERT scaling | — |
| **P8-I3 — Stateless `zepto_ingest_node` binary** | Ingest-only pod, no storage/query; forwards over cluster RPC to data nodes. Lets ingest scale independently of the query tier. Wiring is a one-liner (`executor.set_cluster_node(&cluster_node)`) once P8-I3-wire lands. | M |
| **P8-I4 — Ingest-rate HPA** | Custom metric `zepto_pipeline_ticks_per_sec` → HPA target so the ingest tier autoscales on real load, not on CPU/mem proxy. | S |
| **P8-I5 — Python cluster hook** | Expose `PyPipeline.set_cluster_node(ClusterNodeBase*)` to Python via pybind11 so Python-side DSL writes also route correctly. Plumbing in place (devlog 103); pending pybind11 binding. | S |
| **Bench with batched HTTP / symbol-aware client** | EKS Round 2 showed HTTP curl per-INSERT is latency-bound at ~70/s under N≥2 because every LB-miss pays a TCP RPC hop. Add a benchmark driver that either (a) uses multi-row INSERTs, or (b) computes partition ownership client-side and sends to the correct pod directly. Would measure engine capacity instead of per-request RPC latency. | S |

> ✅ Done: P8-Critical, P8-High, P8-Medium all complete. Live rebalancing, Dual-write, Partial-move, Bandwidth throttling, PTP clock sync, Cluster-aware INSERT routing (devlog 103), Pod placement hardening (devlog 104).

---

## P9 — Physical AI / Industry

Ordered by strategic impact. Engine is ready (MQTT ✅, table-scoped partitioning ✅, ASOF/Window JOIN ✅, Python zero-copy ✅); the gap is at the ingestion-protocol layer. See [`docs/design/physical_ai_market.md`](design/physical_ai_market.md).

| # | Task | Why | Unlocks | Effort |
|---|------|-----|---------|--------|
| 1 | **ROS2 plugin** | `rclcpp` subscriber → `TickMessage`; sensor_data / reliable QoS; bag replay; Isaac Sim hook | Autonomous vehicles + robotics (2 sectors at once) | M |

### OPC-UA — priority-ordered

**Shipped (✅)**

| # | Task | Notes |
|---|------|-------|
| 2a | ~~**OPC-UA connector (PoC)**~~ ✅ (devlog 101) | Scalar client connector, open62541 optional-dep scaffolding, NodeId→SymbolId mapping, SourceTimestamp conversion, routing + backpressure + table-aware ingest, 22 unit tests |
| 2m | ~~**OPC-UA: PoC test-coverage hardening**~~ ✅ (devlog 101) | 11 edge tests — unknown-`table_name`, local-route counter, float overflow/NaN/Inf, far-future datetime, stats-under-contention, large node map, tiny/zero `value_scale`, duplicate and empty `node_id`. Tests 2/3/4/10/11 document latent bugs → tracked as 2n/2p below |

**Tier 1 — Production blockers (Sprint 1 ✅ shipped)**

| # | Task | Notes |
|---|------|-------|
| 2b | ~~**OPC-UA: real open62541 client integration**~~ ✅ (devlog 106) | `UA_Client` session honouring `connect_timeout_ms` / `session_timeout_ms`, `CreateSubscription`, `CreateMonitoredItems` per configured node, data-change callback wired through `on_data_change()`, single `UA_Client_run_iterate` thread, idempotent `stop()`. `ZEPTO_USE_OPCUA=OFF` default build byte-identical to PoC |
| 2n | ~~**OPC-UA: float-safety clamp in `coerce_variant_to_int64`**~~ ✅ (devlog 105) | `std::isfinite` guard rejects `NaN` / ±`Inf` → `decode_errors++`; saturating clamp to `INT64_MIN` / `INT64_MAX` on overflow |
| 2p | ~~**OPC-UA: config validation pass**~~ ✅ (devlog 105) | `start()` rejects empty and duplicate `node_id` up-front with `ZEPTO_ERROR` |
| 2q | ~~**OPC-UA: sector-aware default profiles**~~ ✅ (devlog 105) | `OpcUaConfig::apply_profile(Profile{Fab,Auto,Steel,Generic})` presets `queue_size`, `sampling_interval_ms`, `publishing_interval_ms`, `backpressure_retries` |
| 2o | ~~**OPC-UA: `OpcUaConfig` reconnect / timeout knobs**~~ ✅ (devlog 105) | `connect_timeout_ms=5000`, `session_timeout_ms=60000`, `reconnect_interval_ms=2000`; 2b consumes the first two, 2i (devlog 109) consumes the third |

**Tier 2 — Production MVP (first pilot → first commercial deployment)**

| # | Task | Why | Effort |
|---|------|-----|--------|
| 2c | ~~**OPC-UA: Basic256Sha256 security**~~ ✅ (devlog 108) | Certificate-based Sign / SignAndEncrypt via `UA_ClientConfig_setDefaultEncryption`; cert/key/server-cert paths already on `OpcUaConfig` are now wired; `start()` rejects Sign/SignAndEncrypt with empty cert/key paths before the license gate. MVP limits: single-cert trust list, no revocation list — Sprint 3 follow-up | M |
| 2k | ~~**OPC-UA: integration test**~~ ✅ (devlog 107) | Round-trip against open62541's bundled tutorial-style server (`ns=1;s=the.answer`, Int32=42) in `tests/unit/test_opcua_integration.cpp` — guarded by `ZEPTO_OPCUA_AVAILABLE`, in-process server thread, `messages_consumed ≥ 1` assertion, clean <10 s teardown. CI must install `open62541-devel`/`libopen62541-dev` for the test to run | S |
| 2i | ~~**OPC-UA: reconnect / failover policy**~~ ✅ (devlog 109) | Background iterate-thread now detects `BADCONNECTIONCLOSED` / `BADSERVERNOTCONNECTED` / `BADSECURECHANNELCLOSED`, sleeps `reconnect_interval_ms` ± 25% jitter, retries `UA_Client_connect` with exponential backoff up to 16× base (≈ 32 s ceiling on default 2 s base). Successful reconnect rebuilds subscription + MonitoredItems via new `setup_subscription()` and bumps `OpcUaStats::reconnects`. Live disconnect simulation deferred to Sprint 3 | S |
| 2j | ~~**OPC-UA: volume/quality field mapping**~~ ✅ (devlog 107) | `OpcUaConfig::QualityHandling { IgnoreBad, AcceptAll, AcceptAllGoodAs1 (default) }` — default stamps `TickMessage.volume = 1` if `UA_STATUSCODE_GOOD` else `0`; `AcceptAll` preserves raw 32-bit status; `IgnoreBad` drops non-GOOD at decode time with `decode_errors++` | S |

**Tier 3 — Quality & observability (before SLA commitments, Sprint 3 ✅ shipped)**

| # | Task | Notes |
|---|------|-------|
| 2r | ~~**OPC-UA: atomic stats snapshot**~~ ✅ (devlog 110) | Audit of every multi-field stats transition in `opcua_consumer.cpp` — no torn-read windows exist; every outcome path updates exactly one field under one `lock_guard`, and the `messages_consumed++/bytes_consumed++` pre-dispatch pair is already combined. Existing Sprint-1 test `OpcUaEdgeConcurrency.StatsUnderThreadedWrites` covers the post-quiescence invariant. Audit comment added to code |
| 2s | ~~**OPC-UA: `TcpRpcClient` test injection**~~ ✅ (devlog 110) | `include/zeptodb/cluster/rpc_client_base.h` — virtual base; `TcpRpcClient` inherits; `remotes_` map in all three consumers (`OpcUaConsumer`, `MqttConsumer`, `KafkaConsumer`) widened to `shared_ptr<RpcClientBase>`. Tests: `OpcUaRouting.RouteRemote_IncrementsOnSuccessfulDispatch` + `..._DoesNotIncrementOnFailedDispatch` via `CountingRpcClient` stub |
| 2t | ~~**Kafka/MQTT/OPC-UA connector microbench parity**~~ ✅ (devlog 110) | `KafkaPerf.DISABLED_SingleThreadHotPath` + `MqttPerf.DISABLED_SingleThreadHotPath` mirror the existing OPC-UA harness (pass1 1 M on_message + pass2 p50/p99 + pass3 cheap-path). Pass3 baselines: Kafka 2.69 M ticks/s, MQTT 2.48 M ticks/s, OPC-UA 6.69 M ticks/s (x86_64 single-thread, directly comparable) |

**Tier 4 — DX & advanced data (after MVP)**

| # | Task | Why | Effort |
|---|------|-----|--------|
| 2f | **OPC-UA: browse + auto-discover** | `zepto-opcua-browse` CLI to enumerate a server's address space and auto-populate `nodes[]` | S |
| 2d | **OPC-UA: structured & array variant support** | Beyond scalar coercion — engineering units, array → multiple `TickMessage` | M |
| 2g | **OPC-UA: Historical Access (HA)** | Backfill from server-side historian for initial load — Sector-B initial-load story | M |
| 2h | **OPC-UA: Alarms & Conditions (A&C)** | Ingest alarm events as a separate tick stream — factory ops observability | M |
| 2e | **OPC-UA: string values** | Map UA `String` variants to symbol columns — blocked on string-column support | S |

**Tier 5 — Long-term / P10 candidates**

| # | Task | Why | Effort |
|---|------|-----|--------|
| 2l | **OPC-UA: server mode** | Expose ZeptoDB query results as an OPC-UA server — reverse integration (rare but asked) | L |

### Remaining Physical AI items (non-OPC-UA)

| # | Task | Why | Unlocks | Effort |
|---|------|-----|---------|--------|
| 3 | **Physical AI reference examples** | End-to-end: (a) robot joint RL replay buffer, (b) LiDAR+camera ASOF JOIN, (c) CMP sensor anomaly | Sales artifacts, onboarding | S |
| 4 | **Factory 10KHz benchmark vs InfluxDB / TimescaleDB** | Key Sector-B sales proof point | Smart-factory GTM | S |
| 5 | **Physical AI use-case docs promotion** | Promote from business-only to first-class `docs/usecases/` vertical; add `design/ros2_plugin.md` + `design/opcua_connector.md` ✅ (2a) | Discoverability | S |

Notes:
- Items 1 and 2 both reuse the `MqttConsumer` / `KafkaConsumer` `set_pipeline` + `set_routing` pattern — no new architecture needed.
- Each uses the `ZEPTO_USE_*` optional-dep pattern so core builds stay dependency-free.
- Devlog numbering: OPC-UA PoC landed as `101_opcua_connector_poc.md`; next available is `102_*.md` — ROS2 plugin should claim it.

### Logistics / Warehouse — priority-ordered

Logistics centers (Coupang, CJ, Amazon FC, DHL, Maersk / AMR vendors Geek+, Locus, HAI) sit at the intersection of Physical AI + Industrial IoT + Event Sourcing. Ingestion layer is already covered by OPC-UA ✅ / MQTT ✅ / Kafka ✅. The gap is **domain-specific query primitives** (spatial, entity-timeline) and **regulatory features** (cold-chain immutable audit). See proposal synthesized 2026-04-27 — design doc `docs/design/logistics_warehouse.md` to be authored alongside the first shipped item.

| # | Task | Why | Unlocks | Effort |
|---|------|-----|---------|--------|
| 6a | **Spatial functions (`haversine`, `ST_Distance`, `ST_Within`)** | AGV/AMR collision prediction, geofence alerts, drone-swarm proximity. `physical_ai_market.md` already uses `haversine(...)` in the drone-swarm SQL example but the function is not implemented. Scalar SQL built-ins first; R-tree index later | Logistics + drone-swarm + autonomous-vehicle (3 sectors at once) | M |
| 6b | **Cold-chain immutable table flag + retention** | Pharma/food/vaccine logistics require regulator-auditable temperature log (FDA 21 CFR Part 11, EU GDP, KR GMP). `CREATE TABLE ... WITH (immutable=true, retention='7 years')` — WAL-backed, DELETE/UPDATE rejected at executor. Premium-price sub-sector | Regulated cold-chain vertical | S |
| 6c | **Entity-timeline / event-sourcing recipes** | Order/pallet state tracking: "current status of order X", "Pick→Ship SLA violations". Likely achievable with existing `LAST()` + window functions — verify then add to `SQL_REFERENCE.md` as a named recipe. If a gap exists, add `LAST(value, timestamp)` aggregate | WMS/OMS query patterns | S |
| 6d | **`docs/design/logistics_warehouse.md` + physical_ai_market.md section E** | Formalize logistics as a first-class GTM sector (currently only implicit in drone-swarm D). Sales message, data profile, competitive positioning | Sales collateral, website target-market expansion | S |
| 6e | **Edge-deployment guide (`docs/deployment/EDGE_DEPLOYMENT.md`)** | Logistics centers prefer on-prem edge (network isolation, latency). Single-binary ARM64 already proven via Graviton CI; needs documented recipe (k3s / Docker Compose / systemd on industrial PC) | On-prem pilot onboarding | S |
| 6f | **Logistics benchmark suite** | 2K AGV @ 10Hz + 1M sorter pts/s + 50K RFID peak events — vs TimescaleDB/InfluxDB/Redis. Companion to item 4 (factory bench) | Logistics GTM proof point | S |
| 6g | **Digital Twin / Isaac Sim hook for logistics** | Omniverse-based warehouse digital twin real-time feed. Depends on ROS2 plugin (P9 #1) | Long-term Isaac ecosystem tie-in | M |

Notes:
- 6a is the highest-leverage item — it simultaneously unlocks logistics, drone-swarm (item 1's stated use case), and autonomous-vehicle proximity queries. Design doc `docs/design/spatial_functions.md` should land with the first implementation.
- 6b is small but opens the only regulated sub-sector of logistics — direct path to premium pricing.
- 6c/6d/6e/6f are documentation-weighted and can proceed in parallel with the code items above.
- No new ingestion connector is needed for logistics — OPC-UA (PLC/sorter), MQTT (IoT gateway), Kafka (WMS event bus) already cover all three primary data sources.

---

## P10 — Extensions / Long-term

| Task | Why | Effort |
|------|-----|--------|
| **User-Defined Functions** | Python/WASM UDF | L |
| **Pluggable partition strategy** | symbol_affinity / hash_mod / site_id | M |
| **Edge mode** (`--mode edge`) | Single node + async cloud sync | M |
| **HyperLogLog** | Distributed approximate COUNT DISTINCT | S |
| **Variable-length strings** | Logs, comments, and other free-text | M |
| **HDB Compaction** | Parquet merge | S |
| **Snowflake/Delta Lake hybrid** | — | M |
| **Graph index (CSR)** | Fund flow tracking | L |
| **InfluxDB migration** | InfluxQL → SQL | S |

---

## Summary

| Priority | Category | Remaining | Next Action |
|----------|----------|:---------:|-------------|
| **P2** | Visibility & Launch | 1 + 4 manual | Demo video, Show HN, Reddit |
| **P2.5** | Monetization | 1 (deferred) | Gate when features ship |
| **P4** | Tool Integration | 2 | ClickHouse protocol |
| **P5** | Data Pipelines | 4 | Kafka Connect, CDC |
| **P6** | Enterprise / Cloud | 3 | Marketplace, Geo-rep, SAML |
| **P7** | Engine Performance | 3 | JOINs/Window virtual tables |
| **P8** | Cluster | 9 | RDMA transport, Cold query, Horizontal ingest tier |
| **P9** | Physical AI / IoT | 15 | ROS2, OPC-UA HA + A&C + CLI browse + string variants + structured variants + examples, docs |
| **P10** | Extensions | 9 | UDF, Edge mode |

**Total remaining: 47 items + 4 manual tasks**

**Critical path: P2 (launch) → P4 (ClickHouse protocol) → P7 (JOINs/Window virtual tables)**
