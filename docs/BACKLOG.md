# ZeptoDB Backlog

> Completed features: [`COMPLETED.md`](COMPLETED.md) | latest full local C++ suite:
> 1546 tests run (1544 passed, 1 live S3 opt-in skipped,
> 1 `HttpCluster.DynamicMode_StandaloneToCluster` failure; isolated repeat
> 3/3 passed; 3 disabled)
>
> Last cleaned: 2026-07-04
>
> Devlog: last `207_physical_ai_action_outcome_sql_adapter.md` -> next `208_*.md`

---

## Recent completions (last 2 weeks)

- ✅ **P0 Physical AI Action-Outcome SQL adapter**
  (devlog 207) — added a SQL-backed source/sink adapter for the experimental
  `ActionOutcomeSupervisorRuntime`. The server can now install hooks that load
  proposals from ZeptoDB SQL tables, check duplicate decisions by
  `proposal_id`, compute a deterministic historical-outcome policy, and write
  evidence summary plus decision rows. `POST /admin/action-outcome-supervisor`
  supports `sql_adapter_enabled` and optional default table creation for demos
  and controlled pilots. Product promotion still needs transactional or
  idempotent evidence writes, persisted config/catalog state, cluster-safe
  worker ownership, broader fault/soak tests, and cross-architecture
  verification.
- ✅ **P0 Physical AI Action-Outcome supervisor runtime foundation**
  (devlog 206) — added an experimental, shadow-only
  `ActionOutcomeSupervisorRuntime` plus
  `GET`/`POST`/`DELETE /admin/action-outcome-supervisor`. The runtime can run
  one bounded pass or a background worker when embeddings install proposal
  loader, already-decided, decision provider, and decision sink hooks. It
  records proposal, duplicate, rejection, allow/suppress, fail-closed,
  evidence-row, worker, and latency metrics. Devlog 207 adds the first
  SQL-backed source/sink adapter; remaining product-promotion blockers are
  durable config/catalog state, transactional or idempotent evidence writes,
  broader RBAC/auth tests for mutating controls, restart/node-replacement
  validation, soak/fault tests, rate/backpressure limits, and
  cross-architecture verification.
- ✅ **P3 Physical AI edge/fleet worker runtime foundation** (devlog 205) —
  extended `EdgeFleetConnectorRuntime` with a bounded server-managed worker
  loop, injected outbox-loader/fleet-sink hooks, manual `runOnce()`, worker
  lifecycle counters, last-pass telemetry, HTTP status fields, Prometheus
  worker metrics, and admin lifecycle tests with installed hooks. This closes
  the server-owned worker lifecycle gap, while the remaining product-promotion
  blockers are the concrete built-in SQL/HTTP adapter, persisted connector
  config, idempotent sink documentation, long-running soak/fault tests, and
  cross-architecture verification.
- ✅ **P3 Physical AI edge/fleet server lifecycle** (devlog 204) —
  added `EdgeFleetConnectorRuntime` and admin endpoints for the experimental
  connector lifecycle: `GET`, `POST`, and `DELETE`
  `/admin/edge-fleet-connector`. The server now owns process-local connector
  config/enabled state, local checkpoint start/stop behavior, lifecycle
  counters, and `/metrics` output. Tests cover runtime start/stop/clear,
  missing-checkpoint startup, invalid limits, metrics, and admin-only HTTP
  access. This narrows the remaining promotion work to the server-managed SQL
  poll/apply worker and persisted connector configuration.
- ✅ **P3 Physical AI edge/fleet C++ connector replay** (devlog 203) —
  added `zepto_edge_fleet_replay`, a standalone experimental runtime tool that
  connects `EdgeFleetFeedConnector` to two live ZeptoDB HTTP SQL nodes. The
  tool seeds the Experiment 016 edge/fleet tables, reads the edge outbox
  through native SQL, materializes fleet inbox/final/ACK/telemetry rows through
  SQL inserts, and validates outage, dropped, duplicate, late, restart, ACK
  convergence, recovery JOIN, and suppression audit JOIN behavior. This closes
  the SQL/HTTP source-sink adapter validation gap, while product promotion
  still needs server lifecycle hooks, RBAC/admin controls, catalog or documented
  runtime persistence, and operator docs.
- ✅ **P3 Physical AI edge/fleet runtime connector** (devlog 202) —
  promoted the bounded edge-to-fleet feed semantics into an experimental C++
  runtime connector. `EdgeFleetFeedConnector` now owns bounded passes,
  duplicate/late handling, transient retry accounting, optional ACK checkpoint
  reload, `AppliedButAckFailed` behavior, and Prometheus formatting. Focused
  C++ tests cover bounded processing, dropped/outage-style retry, duplicate
  handling, late delivery, restart reload, malformed input, ACK-boundary
  failure, and metrics. Next: wire concrete SQL/HTTP outbox source and fleet
  sink adapters, then replay Experiment 016 through the runtime connector.
- ✅ **P3 Physical AI bounded edge/fleet feed replay** (devlog 201) —
  added Experiment 016, a research-only explicit edge-to-fleet feed replay.
  The edge node materializes 52 outbox events while preserving immediate
  risky-action suppression; the bounded feed worker transfers decision,
  retrieval, and suppression events to fleet through inbox, ACK, and telemetry
  tables. Live two-node validation passes duplicate, dropped, late, outage,
  restart reload, bounded batch, fleet recovery JOIN, suppression audit JOIN,
  and ACK window checks. Next: promote the feed semantics into an experimental
  runtime connector with persisted cursor state and explicit ACK failure
  behavior.
- ✅ **P3 Physical AI edge/fleet Action-Outcome replay** (devlog 200) —
  added Experiment 015, a research-only two-endpoint replay that separates
  edge-local immediate unsafe-action suppression from fleet-global delayed
  consolidation and audit. The edge node stored 82 research rows and passed
  immediate recovery, risky-action suppression, robot ASOF, and sensor ASOF
  checks. The fleet node stored 87 research rows and passed consolidated
  recovery, suppression audit JOIN, consolidation lag, ROW_NUMBER, and LAG
  checks. Next: explicit bounded edge-to-fleet feed/replication with duplicate,
  dropped, late, outage, and restart cases.
- ✅ **P3 Physical AI Action-Outcome SQL replay** (devlog 199) — added
  Experiment 014, a research-only live ZeptoDB SQL replay for the Physical AI
  Action-Outcome fixture. The replay materializes robot incidents, expected
  actions, historical outcomes, robot state, sensor summaries, poses,
  recommendations, retrievals, and suppressions, then validates row counts,
  failed-repeat JOIN, context top-action JOIN, suppression audit JOIN,
  action/outcome JOIN, robot and sensor ASOF JOINs, ROW_NUMBER, LAG, and
  ST_Within as pass. Next: two-node edge-local versus fleet-global replay.
- ✅ **P3 Physical AI Action-Outcome baseline experiment** (devlog 198) —
  added Experiment 013, a research-only comparison of similar robot incident
  retrieval, runbook/action-prior recommendation, reflection-only memory, and
  context-gated Physical AI Action-Outcome Memory. Hard robot-safety
  distractors make the non-gated baselines repeat unsafe top actions, while the
  context-gated variant reaches 1.00 recovery Top-1 hit rate, 1.00 risky-repeat
  avoidance, and 0.00 hazardous top-action rate.
- ✅ **P3 research experiment governance policy** (devlog 197) — added
  `docs/research/EXPERIMENT_GOVERNANCE.md` and linked it from `AGENTS.md`.
  Future experiments must be classified as research-only, experimental runtime
  path, or promoted product feature before docs/API surfaces imply support.
- ✅ **P3 Experiment 012 operational placement validation and telemetry**
  (devlog 196) — validated an experimental runtime placement path for declared
  operational/Action-Outcome tables, exposed admin-only runtime placement
  through `POST /admin/table-placement`, and surfaced bounded small-table JOIN
  candidates, accepted joins, row-cap rejections, errors, materialized rows,
  and last-side row counts through `/stats` and Prometheus. Product promotion
  still requires persisted catalog/DDL placement and broader operational
  semantics.
- ✅ **P3 experimental small-table distributed hash JOIN validation**
  (devlog 195) — added a bounded coordinator-local hash JOIN path for small
  operational tables. Experiment 011 now reports `suppression_join` as pass
  across node 1 and node 8, and strict full distributed SQL/JOIN/window replay
  succeeds. This is not a general distributed JOIN optimizer.
- ✅ **P3 experimental cluster-mode window value materialization**
  (devlog 194) — fixed the distributed full-data window replay path for the
  validated declared operational-table shape. `ROW_NUMBER` and `LAG` over
  `action_outcome_vendor_recommendations_010` now preserve `group_id`,
  `recommendation_rank`, and `score_micros` values in two-node cluster mode.
  Product promotion still needs limits and telemetry for large-table windows.
- ✅ **P3 Action-Outcome distributed vendor SQL replay classification**
  (devlog 193) — added Experiment 011, a two-node replay that materializes
  Experiment 010 through the distributed HTTP/RPC path. It verifies seed and
  vendor row counts, distributed ingest, co-located JOINs, and records current
  distributed planner gaps.
- ✅ **P3 Action-Outcome vendor SQL/JOIN/window replay** (devlog 192) —
  implemented alias-aware hash JOIN `WHERE` predicate handling and added a
  live Experiment 010 SQL replay. The run materializes vendor baseline query,
  recommendation, retrieval, and suppression tables, then validates
  failed-repeat JOIN, context top-action JOIN, suppression JOIN, misleading
  retrieval JOIN, ROW_NUMBER, and LAG checks as pass.
- ✅ **P3 Action-Outcome vendor baseline experiment** (devlog 191) —
  added Experiment 010, comparing similar-incident retrieval,
  runbook/action-prior recommendation, reflection-only memory, and
  context-gated Action-Outcome Memory on the noisy AIOps fixture. The
  context-gated variant preserved Top-3 hit rate at 1.00 and improved
  failed-action avoidance to 1.00 while recording 21 context suppressions.
- ✅ **P3 string-key hash JOIN materialization** (devlog 190) —
  added a C++ regression for declared `STRING` key hash JOIN and fixed the
  executor to read JOIN keys and joined result cells through type-aware
  `ColumnVector` access. Experiment 009 now reports native string JOIN as pass.
- ✅ **P3 Action-Outcome JOIN/window replay acceptance** (devlog 189) —
  added Experiment 009, which validates native string-window rank ordering and
  numeric SQL JOIN/ROW_NUMBER/LAG acceptance over Action-Outcome replay
  recommendations.
- ✅ **P3 Action-Outcome distributed replay C++ regression** (devlog 188) —
  added a CI-sized two-pipeline TCP RPC regression for Action-Outcome
  schema-aware `STRING` rows, and hardened typed-row RPC so remote owners bind
  sender dictionary codes to original `STRING`/`SYMBOL` text before storage.
- ✅ **P3 Action-Outcome distributed two-node live replay** (devlog 187) —
  the Action-Outcome SQL seed now passes through a real two-node HTTP/RPC
  topology. Schema-aware typed-row INSERT routes through the
  `CoordinatorRoutingAdapter`, remote `TYPED_ROW_INGEST` lands in owner
  pipelines, distributed concat SELECT preserves decoded `STRING`/`SYMBOL`
  values, and Experiment 008 records node-local deltas of 140 rows on node 1
  and 58 rows on node 8.
- ✅ **P2 replication-vs-MPP cluster design doc** (devlog 181) —
  `docs/design/phase_c_distributed.md` now formalises ZeptoDB's distributed
  positioning: replication is the HA/durability layer, while shard ownership,
  routed writes, and scatter-gather query paths are the MPP scale-out layer.
  The section includes a comparison table, current implementation status, and
  honest non-goals so launch collateral can say "scale beyond a single
  DuckDB-style node" without overstating the distributed SQL planner.
- ✅ **P5 Apache Pulsar consumer** (devlog 180) —
  `PulsarConsumer` adds a Pulsar topic/subscription ingress path with shared
  JSON/BINARY/JSON_HUMAN decoders, table-aware routing, backpressure retries,
  Prometheus metrics, subscription type controls, and optional live broker
  polling behind `-DZEPTO_USE_PULSAR=ON`.
- ✅ **Physical AI Agent Memory demo** (devlog 179) —
  `examples/agent_memory/physical_ai_agent_demo.py` now loads realistic AGV,
  ROS odometry/LaserScan replay, and cold-chain telemetry rows; seeds scoped
  Agent Memory with route, sensor, and quality-policy knowledge; retrieves
  context for Physical AI decisions; and records AgentOps context trace/replay
  rows.
- ✅ **Release pipeline parallel Docker cache** (devlog 178) —
  the tag-triggered release workflow now warms Docker BuildKit cache in
  parallel with the amd64/arm64 binary matrix, then performs the Docker Hub
  push only after both binary builds and cache warm-up succeed. This overlaps
  the v0.1.2 19m22s Docker build with the 24m31s slowest binary leg without
  publishing Docker tags for failed binary releases.
- ✅ **CI pipeline speedups** (devlog 177) —
  release binary builds now use per-architecture ccache restore keys,
  `.dockerignore` cuts Docker context upload to low single-digit MB, and
  Graviton checks skip docs/web/deploy/workflow-only changes plus generated
  `chore(release): vX.Y.Z` version commits.
- ✅ **Dev branch and main release pipeline** (devlog 176) —
  `dev` is now the integration branch and `main` is the promotion-only release
  branch. `Version Main Release` synchronizes project versions, commits
  `chore(release): vX.Y.Z`, creates the protected `v*` tag, and dispatches
  binary, Docker, GitHub Release, PyPI, and Homebrew publishing.
- ✅ **P5 AWS Kinesis consumer** (devlog 175) —
  `KinesisConsumer` adds AWS-native stream polling behind
  `-DZEPTO_USE_KINESIS=ON`, while default builds keep shared
  JSON/BINARY/JSON_HUMAN decode, table-aware routing, backpressure retries,
  Prometheus metrics, and no-SDK fallback testable.
- ✅ **P4 MessagePack columnar ingest endpoint** (devlog 174) —
  `POST /insert/msgpack` accepts dependency-light map-of-column-arrays payloads
  with configurable symbol, price, volume, timestamp, and message-type columns,
  sharing Arrow IPC ingest's table-aware batch path, ACL checks, tenant checks,
  cluster routing, and synchronous drain semantics.
- ✅ **Graviton Arrow IPC / Flight verification** (devlog 173) —
  the fast cross-arch harness now reconfigures stale Graviton CMake caches when
  `ZEPTO_USE_FLIGHT=ON` is missing, and Graviton verification confirmed Arrow
  IPC unit tests, live S3, standalone HTTP Arrow ingest, and multi-node
  rebalance smoke with Arrow compiled in.
- ✅ **P3 Agent Memory ANN maintenance, footprint, and IVF** (devlog 172) —
  sparse-projection, HNSW, and IVF ANN indexes now support incremental
  update/delete maintenance for clean indexes, including tombstone accounting
  and row-id remaps after compacting deletes. `/api/ai/stats`, cluster stats,
  Prometheus, and `bench_agent_memory` now expose ANN memory bytes, ANN
  tombstones, and persisted `records.bin` / `vectors.bin` sidecar sizes.
  `bench_agent_memory --compare-ann` includes `ivf_fast` and `ivf_recall`
  profiles, and `zepto_http_server` accepts
  `--agent-memory-ann ivf`, `--agent-memory-ann-ivf-centroids`, and
  `--agent-memory-ann-ivf-probe`. Narrows the P3 stronger ANN row to larger
  production embedding-dump runs, tenant-filter/default-policy evaluation, and
  optional managed embedding provider work.
- ✅ **P3 Agent Memory real embedding fixture** (devlog 171) —
  `bench_agent_memory` now supports `--fixture real --embedding-file PATH`
  for precomputed vector files. The loader validates finite float tokens,
  consistent dimensions, and record-count bounds, then uses those vectors for
  seeded memories, cache entries, and recall queries. Devlog 172 adds
  update/delete maintenance, footprint telemetry, and IVF comparison profiles.
- ✅ **P3 Agent Memory clustered ANN fixture** (devlog 170) —
  `bench_agent_memory` now supports `--fixture mixed|semantic|clustered`.
  The clustered fixture generates deterministic center-plus-noise embeddings
  and query vectors so sparse projection and HNSW can be compared on
  cluster-shaped semantic neighborhoods instead of only random vectors. Devlog
  171 adds precomputed real-vector file fixtures.
- ✅ **P3 Multi-node Agent Memory capacity rollback** (devlog 169) —
  owner-side HTTP/RPC writes now restore write-triggered automatic eviction
  side effects when primary durability fails, and restore only the failed and
  later eviction tombstones when tombstone persistence fails after partial
  success. This closes the remaining P3 Multi-node Agent Memory backlog gap;
  shard migration dual-write catch-up remains documented future work outside
  the closed P3 row.
- ✅ **P3 Multi-node Agent Memory failover status** (devlog 168) —
  `AgentMemoryOwnerFailoverResult` now reports source/replacement ids, epochs,
  replay promotion, degraded state, and missing replay-source status. Local
  `/api/ai/stats` exposes the last failover status so operators can distinguish
  clean successor replay from a degraded failover with no usable replay source.
  Devlog 169 closes the final capacity-eviction rollback gap.
- ✅ **P3 Multi-node Agent Memory eviction tombstones** (devlog 167) —
  owner-side HTTP/RPC writes now carry automatic TTL, tenant-quota, and capacity
  eviction keys out of `AgentMemoryStore` and persist them as committed delete
  WAL tombstones. Restart replay and replica shard adoption no longer resurrect
  entries evicted by a live owner write. Devlog 168 adds failover status and
  devlog 169 closes rollback for non-durable eviction side effects.
- ✅ **P3 Multi-node Agent Memory cluster stats** (devlog 166) —
  `/api/ai/stats?scope=cluster` now scatters Agent Memory stats over routed
  `TcpRpc`, returns per-node stats plus aggregate counts, and reports missing
  or invalid remote stats responses as `partial_failures`. Default
  `/api/ai/stats` remains local/backward-compatible. Devlogs 167-169 close the
  remaining P3 multi-node durability/failover gaps.
- ✅ **P3 Context trace/replay** (devlog 165) —
  AgentOps schema helpers now include `context_traces` and
  `context_replay_events`, with `examples/agent_memory/context_trace.py`
  building SQL rows that explain selected memories by rank, score, similarity,
  token budget fit, pinned/importance/prior-use signals, and related
  time-series replay snapshots. Closes the P3 context trace/replay row.
- ✅ **P3 OpenTelemetry/LLM trace ingest mapping** (devlog 164) —
  `examples/agent_memory/otel_mapping.py` now converts OTLP JSON-style GenAI,
  cache, tool-call, and error spans into AgentOps SQL INSERT statements.
  AgentOps schema helpers include `llm_errors`, and tests cover provider/model
  mapping, token counts, cache hits, latency, tool calls, and model errors.
  Closes the P3 OpenTelemetry trace mapping row.
- ✅ **P3 Agent Memory tenant/namespace eviction quotas** (devlog 163) —
  `AgentMemoryEvictionConfig` now supports scoped tenant quotas for memory and
  cache entries. Quotas can target a whole tenant or one tenant namespace,
  evict only matching entries before global caps run, keep pinned-memory
  overflow protection, and expose configured quota count through
  `/api/ai/stats` and Prometheus. Closes the P3 tenant-scoped eviction row.
- ✅ **P3 Agent Memory snapshot failure/latency metrics** (devlog 162) —
  `AgentMemoryStore::save_to_directory()` now records the last snapshot attempt
  duration and total failed snapshot attempts. `/api/ai/stats` exposes
  `snapshot_latency_seconds` and `snapshot_failures_total`, while `/metrics`
  exports `zepto_agent_memory_snapshot_latency_seconds` and
  `zepto_agent_memory_snapshot_failures_total`. Closes the P3 snapshot
  observability row.
- ✅ **P3 Agent Memory agent-only EKS harness mode** (devlog 161) —
  `tests/k8s/run_eks_bench.sh --agent-only` now reruns only the Agent Memory
  E2E stage after the core EKS compat/HA harness is already green. The mode
  preserves wake/node-readiness checks, image tag overrides, cleanup, result
  summaries, and `--keep` handling while skipping compat/HA and native engine
  benchmark stages. Closes the P3 harness row.
- ✅ **P5 Telegraf external output plugin** (devlog 160) — adds
  `zepto-telegraf-output`, an `outputs.execd` writer that reads Telegraf
  Influx line protocol from stdin, maps metrics to ZeptoDB
  `(symbol, price, volume, timestamp)` rows, and writes HTTP SQL INSERT
  batches with auth/tenant headers, field mapping, scaling, timestamp-unit,
  and batch-size options. Includes parser/mapping tests and an operations
  guide. Closes the P5 Telegraf row.
- ✅ **P9 factory 10KHz live competitor run** (devlog 159) — adds
  `tools/factory-10khz-workload.py` and
  `tools/run-factory-10khz-live-docker.sh` to run the Sector-B factory proof
  against real local Docker deployments of ZeptoDB, InfluxDB, and
  TimescaleDB. The closure run sustained 10,000 rows/sec for 60 seconds on
  each system with 600,000 inserted and 600,000 verified rows, zero failed
  writes, and no skipped competitors. Closes the last P9 open item.
- ✅ **P9 OPC-UA live HA + server mode** (devlog 155) — adds
  `OpcUaConsumer::read_history()` for live open62541 `HistoryRead_raw`
  backfills when historizing support is compiled in, plus `OpcUaServer`
  for exposing configured ZeptoDB symbols as OPC-UA Int64 variable nodes.
  Default builds fail closed with clear stats/diagnostics while preserving
  testable snapshot and replay contracts. Devlog 159 closes the remaining
  factory competitor live run, leaving P9 with no open backlog items.
- ✅ **P9 OPC-UA production-profile closeout** (devlog 154) — adds
  `zepto-opcua-browse` address-space discovery, array expansion, string value
  mapping, Structured-field hooks with engineering-unit metadata, Historical
  Access replay hooks, Alarms & Conditions event hooks, and a factory 10 KHz
  competitor benchmark harness. Devlog 155 closes the live HistoryRead and
  server-mode follow-ups; devlog 159 closes the actual InfluxDB /
  TimescaleDB factory live run.
- ✅ **P9 Physical AI closeout** (devlog 153) — closes the remaining ROS 2
  roadmap track and logistics documentation slice: `TypedProfile` rows now
  forward over cluster RPC when the owner is remote, SQL supports
  `haversine`, `ST_Distance`, and `ST_Within` over float latitude/longitude
  columns, and new docs/examples cover Isaac Sim/digital twin replay, robot
  RL replay, LiDAR ASOF JOIN, fleet anomaly detection, edge deployment,
  Physical AI use cases, cold-chain audit recipes, logistics design, and the
  logistics benchmark suite. Devlogs 154-155 close the OPC-UA production
  extensions, and devlog 159 closes the external competitor factory
  benchmark.
- ✅ **ROS 2 schema-aware typed ingest** (devlog 152) —
  `Ros2IngestMode::TypedProfile` now maps standard Physical AI messages into
  wide typed tables backed by `SchemaRegistry` and
  `ZeptoPipeline::ingest_typed_row()`. IMU, JointState, Odometry, TFMessage,
  and LaserScan typed schemas carry timestamp, receive time, robot/session,
  topic/frame metadata, quality, and profile-specific numeric columns. Live
  ROS and rosbag2 paths share the typed mapping, extra table columns are
  default-filled, and SQL reads `SYMBOL`/narrow/floating columns with
  type-aware access. Closes P9 1e.
- ✅ **ROS 2 standard message profiles** (devlog 151) — `Ros2Consumer`
  now supports `Ros2IngestMode::StandardProfile` for
  `sensor_msgs/msg/Imu`, `sensor_msgs/msg/JointState`,
  `nav_msgs/msg/Odometry`, `tf2_msgs/msg/TFMessage`, and
  `sensor_msgs/msg/LaserScan`. Profiles flatten configured numeric fields
  into the existing table-aware scalar ingest path; JointState and TF expand
  arrays with `symbol_id + index`; LaserScan records metadata plus finite
  range/intensity summaries. The RoboStack Jazzy smoke now installs/verifies
  standard message packages and passed with ROS scalar + standard profiles +
  rosbag2 enabled. Closes P9 1d.
- ✅ **EKS rebalance bench hardening** (devlog 150) — `bench_rebalance`
  now fails fast when a rebalance trigger cannot be issued, preserves HTTP
  status/body diagnostics, supports `--rebalance-timeout-sec`, and has a
  baseline `smoke` scenario. The fast EKS harness injects the bench license,
  adds hard benchmark timeouts, produces correct smoke-only x86_64/arm64
  summaries, and force-deletes bench NodeClaims during teardown. EKS
  verification passed SQL + Arrow smoke, Stage 5 smoke, summary `PASS/PASS`,
  and final NodePool/node cleanup.
- ✅ **Arrow-enabled EKS bench images** (devlog 149) —
  `Dockerfile.bench` and `Dockerfile.bench.arm64` now build with
  `-DZEPTO_USE_FLIGHT=ON` and include Apache Arrow runtime packages, while the
  fast cross-arch harness has `--arrow-smoke` and `--skip-benchmark` modes.
  EKS verification pushed `bench-x86` and `bench-arm64`, deployed both on the
  `zepto-bench` Auto Mode cluster, and confirmed `/insert/arrow` returns
  `{"inserted":3,...}` on both x86_64 and arm64. Closes P4
  "Arrow-enabled EKS ingest verification image".
- ✅ **Arrow IPC ingest endpoint** (devlog 147) — `POST /insert/arrow`
  accepts Apache Arrow IPC RecordBatchStream payloads and maps
  `sym|symbol`, `price`, `volume`, optional `timestamp`, and optional
  `msg_type` columns directly into table-aware `TickMessage` batches without
  per-row SQL. String symbols are interned, timestamp arrays are converted to
  nanoseconds, `price_scale` / `volume_scale` handle decimal-to-int64 storage,
  table ACLs and tenant namespace checks are enforced, and no-Arrow builds
  return `406`. +6 HTTP Arrow tests cover success, generated timestamps,
  empty batches, malformed IPC, missing required columns, and unknown tables.
  Closes P4
  "Arrow IPC ingest endpoint" (M effort).
- ✅ **ROS 2 rosbag2 import/replay** (devlog 146) — `Ros2Consumer` now has
  `Ros2BagConfig`, `Ros2BagStats`, `import_bag()`, and `replay_bag()` for
  deterministic scalar rosbag2 ingestion. The CMake path detects
  `rosbag2_cpp`/`rosbag2_storage` under `-DZEPTO_USE_ROS2=ON`, the smoke script
  verifies the bag packages, and generated sqlite3 bags are imported through
  the same table-aware ZeptoPipeline route as live scalar subscriptions.
- ✅ **ROS 2 runtime smoke packaging** (devlog 145) — new
  `tools/run-ros2-smoke.sh` creates/reuses the RoboStack Jazzy environment,
  verifies ROS package availability, runs a real `std_msgs/msg/Float64` CLI
  pub/sub smoke, builds `zepto_ros2`/`zepto_tests` with DuckDB disabled, and
  runs `Ros2ConsumerTest.*`. `docs/operations/ROS2_SETUP.md` now documents the
  supported local/CI smoke path and common RoboStack linker/runtime issues.
- ✅ **ROS 2 live scalar subscriber** (devlog 144) — live
  `std_msgs/msg/{Float64,Float32,Int64,Int32,UInt64,UInt32}` subscriptions now
  compile when `rclcpp`/`std_msgs` are present, use a private ROS 2 context and
  executor, map `data` into the table-aware ZeptoDB ingest path, and have a
  RoboStack Jazzy smoke test proving `rclcpp` publish → `Ros2Consumer` →
  ZeptoDB table partition.
- ✅ **ROS 2 connector skeleton** (devlog 143) — new optional
  `zepto_ros2` feed target, `Ros2Consumer` public C++ skeleton, pure config
  validation, ROS time conversion, scalar sample → `TickMessage` mapping,
  table-aware local/cluster routing, Prometheus formatter, and no-live-ROS
  unit tests.
- ✅ **ROS 2 / Physical AI roadmap** (devlog 142) — new design doc
  `docs/design/ros2_physical_ai_roadmap.md` promotes ROS 2 from a single P9
  plugin row into a product roadmap: live subscriber bridge, rosbag2
  import/replay, message profiles, schema-aware typed ingest, Isaac Sim,
  reference examples, and edge deployment. P9 now tracks implementation slices
  instead of one broad "ROS 2 plugin" bucket.
- ✅ **EKS Agent Memory E2E** (devlog 141) — `tests/k8s/test_k8s_agent_memory.py` now deploys real ZeptoDB bench images on the x86 and arm64 EKS bench node pools, validates memory put/search/context/cache/tombstone/stats/metrics behavior, restarts the pod, and verifies persisted Agent Memory state replays. The EKS `--k8s-only` harness now includes this stage after compat/HA checks; final run `/tmp/eks_bench_20260530_035421/` passed amd64 compat 27/27, amd64 HA 11/11, arm64 compat 27/27, arm64 HA 11/11, and Agent Memory E2E 2/2.
- ✅ **Agent-attached time-series demos** (devlog 129) — `examples/agent_memory/agent_attached_timeseries_demo.py` now runs five vertical scenarios for finance/HFT, IoT smart factory, observability/APM, robotics fleet, and game/live-ops. Each scenario seeds a live time-series table, stores scoped `MemoryRecord` context, retrieves memory under a token budget, and builds the attached-agent prompt that combines current timeline evidence with learned context.
- ✅ **Agent Memory Layer** (devlog 120) — additive AI memory/context subsystem on top of ZeptoDB Core. Adds in-memory `MemoryRecord` storage, parallel exact top-K filtered embedding search, token-budget context assembly, exact prompt cache, semantic cache fallback, sidecar persistence with configurable flush cadence, bounded eviction, optional sparse-projection ANN candidate indexing, `/api/ai/stats` and Prometheus metrics, HTTP `/api/ai/*` endpoints, Python `connection.memory` / `connection.cache` helpers, provider-cache and LangGraph-style examples, optional OpenAI/Anthropic/LangGraph adapters, production-shaped AgentOps telemetry demo, and `bench_agent_memory` baseline/sweep harness. Current-instance 128-dim exact scan is under the 10 ms target at 100K and 300K records and ~16.7 ms at 1M; non-TTL 1M fixture load now completes in seconds instead of rescanning on every write. Sparse projection is faster but recall-sensitive. v0 uses client-supplied `float32[]` embeddings and deliberately avoids server-side LLM or embedding-provider calls.
- ✅ **Arrow IPC query response** (devlog 119) — `POST /` (port 8123) now honours Arrow IPC content negotiation: `Accept: application/vnd.apache.arrow.stream`, `?default_format=Arrow` (ClickHouse-style), or `?format=arrow` returns an Arrow IPC RecordBatchStream (~2–3× faster than JSON on large result sets, same DuckDB engine). JSON remains the default. Errors stay JSON regardless of Accept (matches ClickHouse). Built with `ZEPTO_USE_FLIGHT=ON` (default) → working; built without → `406 Not Acceptable` with JSON error. Pulled the Arrow encoder out of `flight_server.cpp` into a shared `zepto_arrow_ipc` static lib so HTTP and Flight share one `to_arrow_type` / `build_schema` / `result_to_batch`; encoder also now maps `SYMBOL` columns to Arrow `utf8` via `symbol_dict` (was returning raw int64 codes). +7 tests in new `test_http_arrow_ipc.cpp`. Closes P4 "Arrow IPC query response" (S effort).
- ✅ **S3 Parquet sink connector** (devlog 118) — operator-facing surface for the cold-tier S3 Parquet path that has shipped as C++ infra since devlog 012. New `S3Layout::HIVE` (default) emits Athena/DuckDB/Polars/Spark auto-discoverable `year=YYYY/month=MM/day=DD/symbol={ID}/{ID}-{hour_epoch}[-{hash}].parquet` keys; FLAT kept byte-identical for backward compat. New Helm `coldTier:` block, matching `--cold-tier-*` CLI flags, `ZEPTO_COLD_TIER_*` env vars (Helm interop), per-pod hostname-hash filename collision protection, new operator recipe doc (`docs/operations/COLD_TIER_S3.md`). +14 tests (`test_s3_sink.cpp`, `test_parquet_writer.cpp`, `test_cold_tier.cpp`). Closes the P5 row.
- ✅ **Ingest-rate HPA** (devlog 117) — `zepto_ingest_ticks_per_sec` per-pod gauge on `/metrics`, wired into the Helm HPA as a custom `Pods` metric (`autoscaling.ingestRateEnabled`). Kubernetes now autoscales on real ingest load instead of CPU/memory proxies; CPU/memory remain configured as the safety net. Closes P8-I4.
- ✅ **Marketing site rebrand** (devlog 115) — 5-page IA (`/home`, `/solutions`, `/features`, `/performance`, `/pricing`) pivots the site from "HFT/quant-only" to "general-purpose industry time-series DB" serving Physical AI, Finance, Game, IoT/Smart Factory, and real-time observability. WEBSITE_PRD.md updated to the Next.js + MUI stack that actually shipped. Unblocks the P2 demo-video item.
- ✅ **Python cluster hook** (devlog 114) — `Pipeline.enable_cluster_routing(self_id, peers, …)` pybind11 method. In-process cluster front-door finally wired. Closes P8-I5.
- ✅ **Stateless `zepto_ingest_node`** (devlog 113) — ingest-only binary, forwards all ticks to storage pods. Helm opt-in. Closes P8-I3.
- ✅ **DDL replication** (devlog 112) — fire-and-forget `CREATE/DROP/ALTER TABLE` scatter-gather. Closes P8-DDL-replication.
- ✅ **HTTP INSERT cluster routing** (devlog 111) — `CoordinatorRoutingAdapter` wired in `zepto_http_server`. EKS verified (Round 2+3). Closes P8-I3-wire.
- ✅ **OPC-UA Sprint 1–3** (devlogs 101–110) — PoC → SLA-grade in 3 sprints. Real UA_Client, security, reconnect, quality mapping, microbench parity.
- ✅ **Ingest Phase 1** (devlog 102) — `drain_threads` auto-sizing + configurable ring capacity.
- ✅ **Pod placement hardening** (devlog 104) — required antiAffinity + topologySpread + resource defaults.

---

## P2 — Visibility & Launch

| Task | Effort | Notes |
|------|--------|-------|
| **YouTube / Loom demo video** | S | Unblocked by devlog 115: `/solutions` is a 5-vertical script-ready walkthrough (Physical AI, Finance, Game, IoT, Observability). Multi-industry messaging foundation is live. |

Manual tasks: DB-Engines registration, demo GIF, Show HN, Reddit (5 subs). See `docs/community/`.

---

## P3 — Agent Memory / AI Context

| Task | Why | Effort |
|------|-----|--------|
| **Agent Memory stronger ANN family** | Sparse projection, optional hnswlib HNSW, and dependency-free IVF are now comparable with the devlog 121/123/172 harness. Clean ANN indexes support append, embedding update, delete, tombstone accounting, and compacting row-id remaps; stats expose ANN memory bytes and persisted sidecar footprint. Next: larger production embedding-dump runs and tenant-filter/default-policy evaluation before choosing a production default ANN mode. Persisted ANN index sidecars remain optional future work only if rebuild cost becomes the bottleneck. | M |
| **Persisted operational table placement option** | Devlog 196 validates the experimental runtime control-plane path and Experiment 012 telemetry proof. The next product step is to persist placement in the catalog/DDL layer, e.g. a table option for `hash_by_table`, default table+symbol hashing, or pinned operational tables, instead of relying on an admin-only runtime override. | M |
| **Productize bounded small-table JOIN policy** | Devlog 195 validates coordinator-local small-table JOIN under a strict row cap. Before promotion, decide whether it stays automatic, becomes a feature flag, or moves behind an optimizer rule with cost checks, and document memory/latency limits for operational/control tables. | M |
| **Bounded distributed window materialization policy** | Devlog 194 fixes correctness for the validated Action-Outcome window replay shape. Product promotion needs explicit row/memory limits, telemetry for full-data materialization, and fallback/error semantics for large tables. | M |
| **Built-in Physical AI edge/fleet SQL/HTTP adapter and promotion hardening** | Devlog 205 adds the bounded worker lifecycle and hook contract. Product promotion still needs a built-in SQL/HTTP outbox-loader plus fleet-sink adapter, persisted connector config/catalog metadata, documented idempotent sink requirements, restart/node-replacement tests over real ZeptoDB tables, long-running soak/fault injection, rate/backpressure limits, and cross-architecture verification. | M |
| **Productize Physical AI Action-Outcome supervisor** | Devlogs 206-207 add the admin-gated shadow runtime, hook contract, and first SQL-backed proposal/history/decision/evidence adapter. Product promotion still needs transactional or idempotent evidence writes, durable config/catalog state, cluster-safe worker ownership, broader RBAC/auth tests for mutating controls, restart/node-replacement idempotency validation, long-running similar-but-different scenario fault tests, rate/backpressure limits, and cross-architecture verification. | M |
| **Optional managed embedding provider** | Enterprise convenience only; default path remains client-supplied embeddings. | M |

> ✅ Done: v0 Agent Memory Layer (devlog 120) — `MemoryRecord` store, parallel top-K filtered search, sparse-projection ANN candidate index, context assembly, exact/semantic cache, sidecar persistence with configurable flush cadence, bounded eviction, HTTP stats/metrics, Python client, examples, optional provider/framework adapters, AgentOps schema/demo, and current-instance benchmark report.

---

## P4 — Tool Integration

| Task | Why | Effort |
|------|-----|--------|
| **ClickHouse wire protocol** | DBeaver, DataGrip, Grafana native | L |
| **JDBC/ODBC drivers** | Tableau, Excel, Power BI | L |

> ✅ Done: Arrow IPC query response (devlog 119) — `POST /` content negotiation via `Accept: application/vnd.apache.arrow.stream` / `?default_format=Arrow` / `?format=arrow`; ~2–3× faster than JSON on large result sets. JSON remains default; errors stay JSON regardless of Accept. Arrow IPC ingest endpoint (devlog 147) — `POST /insert/arrow` binary columnar tick ingest with table-aware routing. Arrow-enabled EKS bench images and cross-arch `/insert/arrow` smoke are complete (devlog 149). MessagePack columnar ingest (devlog 174) — `POST /insert/msgpack` accepts a dependency-light map-of-column-arrays payload with the same table-aware batch ingest path.

---

## P5 — Data Pipelines

| Task | Why | Effort |
|------|-----|--------|
| **CDC connector (Debezium)** | PostgreSQL/MySQL → real-time sync | M |
| **Kafka Connect Sink** | Enterprise pipeline standard | M |

> ✅ Done: MQTT consumer (devlog 081) — QoS 0/1/2, topic wildcards,
> shared Kafka JSON/BINARY/JSON_HUMAN decoders, Paho optional-dep pattern.
> S3 Parquet sink connector (devlog 118) — Hive-partitioned S3 keys, Helm
> `coldTier.*`, `--cold-tier-*` CLI flags, `ZEPTO_COLD_TIER_*` env vars,
> operator recipe doc. Telegraf external output plugin (devlog 160) —
> `outputs.execd` line-protocol stdin → ZeptoDB HTTP SQL INSERT writer.
> AWS Kinesis consumer (devlog 175) — shard polling surface with shared
> JSON/BINARY/JSON_HUMAN decoders, table-aware routing, metrics, and no-SDK
> fallback. Apache Pulsar consumer (devlog 180) — topic/subscription polling
> with Shared/Exclusive/Failover/KeyShared subscription modes, shared decoders,
> table-aware routing, metrics, and no-SDK fallback.

---

## P6 — Enterprise / Cloud

| Task | Why | Effort |
|------|-----|--------|
| **Cloud Marketplace** | AWS/GCP one-click | M |
| **Geo-replication** | Multi-region trading desks | L |
| **SAML 2.0** | Bank/insurance SAML-only environments | L |

---

## P7 — Engine Performance

| Task | Why | Effort |
|------|-----|--------|
| **JOINs/Window on virtual tables** | Moderate engine impact | M |
| **VWAP 1M p50 sub-600µs restore** | Inherent clang-19 register-spill residual (+11.5% vs baseline). ~30 LOC kernel extraction. Low urgency. | M |
| **DSL AOT compilation** | Nuitka/Cython | M |

---

## P8 — Cluster

### RDMA Transport

| Task | Why | Effort |
|------|-----|--------|
| **WAL replication RDMA PUT** | TCP 50µs → RDMA 1-2µs | M |
| **Remote column scan RDMA GET** | Zero DataNode CPU for scatter-gather | L |
| **Partition migration RDMA GET** | Zero service impact during rebalancing | M |
| **Failover re-replication RDMA GET** | Minimize replica overhead on node failure | M |
| **`remote_ingest_regions_` wire-up** | Actual RDMA ingest path connection | S |

### Features

| Task | Why | Effort |
|------|-----|--------|
| **Tier C cold query offload** | Historical data → DuckDB on S3. **Elevated importance after Arc analysis (2026-05-13)**: Parquet+S3 is now the de-facto cold-tier standard, and shipping this neutralises the "vendor lock-in" critique without sacrificing our hot-tier differentiation. | M |
| **Global symbol registry** | Distributed string symbol routing | M |

### Horizontal Ingest (remaining)

| Task | Why | Effort |
|------|-----|--------|
| **Bench: symbol-aware / batched HTTP client** | Current HTTP bench is latency-bound at ~90/s under N≥2 (RPC hop per non-local INSERT). Need a driver that either batches or computes ownership client-side. | S |

> ✅ Done: P8-I4 ingest-rate HPA (devlog 117), P8-I5 Python cluster hook (devlog 114), P8-I3-wire (devlog 111), P8-I3 ingest node (devlog 113), P8-DDL-replication (devlog 112), Pod placement (devlog 104), Ingest Phase 1 (devlog 102), Cluster-aware INSERT routing (devlog 103), and full cross-arch EKS live rebalance integrity closure (devlog 158). Live rebalancing, dual-write, partial-move, bandwidth throttling, PTP clock sync all shipped earlier.

---

## P9 — Physical AI / Industry

Design anchor: [`docs/design/ros2_physical_ai_roadmap.md`](design/ros2_physical_ai_roadmap.md).

### Open items

No open P9 backlog items remain.

> ✅ Done: ROS 2 roadmap track 1f-1i, spatial SQL 6a, Physical AI docs 5,
> cold-chain recipe 6b, entity-timeline/logistics docs 6c-6e, and logistics
> benchmark suite spec 6f (devlog 153). OPC-UA PoC (devlog 101), Sprint 1
> (105-106), Sprint 2 (107-109), Sprint 3 (110), and P9 production-profile
> contracts / browse CLI / factory bench harness (154), live HistoryRead,
> server mode (155), and factory 10KHz live competitor run (159) are also
> done.

---

## P10 — Extensions / Long-term

| Task | Why | Effort |
|------|-----|--------|
| **User-Defined Functions** | Python/WASM UDF | L |
| **Pluggable partition strategy** | symbol_affinity / hash_mod / site_id | M |
| **Edge mode** (`--mode edge`) | Single node + async cloud sync | M |
| **HyperLogLog** | Distributed approximate COUNT DISTINCT | S |
| **Variable-length strings** | Logs, comments, free-text | M |
| **HDB Compaction** | Parquet merge | S |
| **Snowflake/Delta Lake hybrid** | — | M |
| **Graph index (CSR)** | Fund flow tracking | L |
| **InfluxDB migration** | InfluxQL → SQL | S |
| **Continuous queries + retention policies scheduler** | User-facing "run this SELECT every N seconds → INSERT INTO target" plus age-based partition retention. Common operational expectation; Arc has it as `internal/scheduler/`. Implementation = SQL + cron-style scheduler on top of the existing executor. From Arc analysis (2026-05-13). | M |
| **Single binary `zepto` with subcommands** | Replace `zepto_http_server` / `zepto_data_node` / `zepto_cli` with `zepto serve` / `zepto data-node` / `zepto cli`. Simplifies operator mental model; matches Arc's single-binary deployment story. CMake target consolidation + main dispatcher. From Arc analysis (2026-05-13). | S |

---

## Summary

| Priority | Category | Open | Next action |
|----------|----------|:----:|-------------|
| **P2** | Visibility & Launch | 1 + 4 manual | Demo video → Show HN → Reddit |
| **P3** | Agent Memory / AI Context | 2 | Production embedding-dump ANN policy → optional embedding provider |
| **P4** | Tool Integration | 2 | ClickHouse wire protocol (L) → JDBC/ODBC drivers (L) |
| **P5** | Data Pipelines | 2 | CDC connector (M) → Kafka Connect Sink (M) |
| **P6** | Enterprise / Cloud | 3 | Marketplace |
| **P7** | Engine Performance | 3 | JOINs/Window virtual tables |
| **P8** | Cluster | 8 | RDMA transport, Tier C cold offload (elevated) |
| **P9** | Physical AI / IoT | 0 | Closed |
| **P10** | Extensions | 11 | Continuous queries scheduler, single-binary CLI |

**Total open: 32 items + 4 manual tasks**

**Critical path: P5 CDC connector → P2 demo video / launch collateral**

> **2026-05-13 — Arc competitive analysis**: 9 new items added across P2/P4/P5/P10 and the P8 Tier C cold-offload row was elevated. Each added item is tagged "From Arc analysis (2026-05-13)" in its `Why` cell. Headline lessons: (1) batched columnar wire formats (Arrow IPC, MessagePack) are the single biggest ingest-throughput unlock; (2) Arrow IPC query responses are a near-free 2–3× win on large result sets; (3) ecosystem connectors (Telegraf/MQTT/S3 Parquet sink) are higher leverage than yet-another-streaming-source consumer; (4) the MPP-cluster vs replication-cluster distinction is now captured in `docs/design/phase_c_distributed.md` (devlog 181) as a launch and enterprise-sales artefact. We do **not** chase Arc's storage-first / batch-flush model — our memory-first / per-tick-durable / immediately-queryable architecture is the differentiator and stays.
