# ZeptoDB — Completed Features

Last updated: 2026-06-13

---

## Latest

- [x] **P2 replication-vs-MPP cluster design doc** (devlog 181) —
  `docs/design/phase_c_distributed.md` now includes the formal comparison
  between replication clusters and MPP clusters. It defines replication as
  ZeptoDB's durability/HA layer, MPP-style shard ownership as the write and
  query scale-out layer, and clearly marks current capabilities versus
  non-goals: no full cost-based distributed SQL optimizer yet, no arbitrary
  cross-node joins/windows/DISTINCT yet, and no global cross-shard
  transactions. Closes BACKLOG P2 "Replication-cluster vs MPP-cluster design
  doc".

- [x] **P5 Apache Pulsar consumer** (devlog 180) —
  `PulsarConsumer` now mirrors the Kafka/MQTT/Kinesis feed pattern for Pulsar:
  shared JSON/BINARY/JSON_HUMAN decoders, table-aware `TickMessage` stamping,
  single-node and cluster routing, backpressure retries, Prometheus metric
  formatting, config validation, and optional live broker polling behind
  `-DZEPTO_USE_PULSAR=ON`. Default builds keep decode/routing tests available
  and return false from `start()` when the Pulsar C++ client is absent. Closes
  BACKLOG P5 "Apache Pulsar consumer".

- [x] **Physical AI Agent Memory demo** (devlog 179) —
  `examples/agent_memory/physical_ai_agent_demo.py` now loads realistic AGV,
  ROS odometry/LaserScan replay, and cold-chain telemetry rows; seeds scoped
  Agent Memory with route, sensor, and quality-policy knowledge; retrieves
  context for Physical AI decisions; and records AgentOps context trace/replay
  rows. Pytest coverage exercises multi-table inserts, memory metadata,
  trace/replay SQL, empty rows, and epoch-zero timestamps.

- [x] **Release pipeline speedups** (devlogs 177-178) —
  release binary builds now use per-architecture ccache, Docker context upload
  is trimmed through `.dockerignore`, non-code Graviton runs are skipped, and
  Docker release builds warm the BuildKit cache in parallel with amd64/arm64
  binary builds while the final Docker Hub push remains gated behind successful
  binaries. The backlog header and recent-completion index are refreshed
  through devlog 178.

- [x] **Dev branch and main release pipeline** (devlog 176) —
  development now targets `dev`, while `main` is treated as the release branch.
  A new `Version Main Release` workflow synchronizes CMake, Python, and web
  package versions, creates a `vX.Y.Z` tag, and dispatches the existing release
  publisher for binaries, Docker, GitHub Releases, PyPI, and Homebrew. Graviton
  and PR documentation checks now include `dev`, and the local pre-push hook
  blocks direct local pushes to `main` unless explicitly overridden.

- [x] **P5 AWS Kinesis consumer** (devlog 175) —
  `KinesisConsumer` now mirrors the Kafka/MQTT feed pattern for AWS-native
  streams: shared JSON/BINARY/JSON_HUMAN decoders, table-aware
  `TickMessage` stamping, single-node and cluster routing, backpressure
  retries, Prometheus metric formatting, and optional AWS SDK Kinesis polling
  behind `-DZEPTO_USE_KINESIS=ON`. Default builds keep decode/routing tests
  available and return false from `start()` when the SDK is absent. Closes
  BACKLOG P5 "AWS Kinesis consumer".

- [x] **P4 MessagePack columnar ingest endpoint** (devlog 174) —
  `POST /insert/msgpack` now accepts a dependency-light MessagePack map of
  column arrays, supports configurable symbol/price/volume/timestamp/msg_type
  columns and numeric scales, enforces table ACL and tenant namespace checks,
  and routes rows through `QueryExecutor::ingest_tick_batch()` so table-aware
  ingest, cluster routing, synchronous drain, and schema `has_data` marking
  match Arrow IPC ingest. Closes BACKLOG P4 "MessagePack columnar ingest
  endpoint".

- [x] **P3 Agent Memory ANN maintenance, footprint, and IVF** (devlog 172) —
  clean sparse-projection, HNSW, and IVF ANN indexes now handle embedding
  updates, deletes, tombstones, and compacting row-id remaps incrementally
  instead of forcing whole-index rebuilds. Agent Memory stats, cluster stats,
  Prometheus, and `bench_agent_memory` now expose estimated ANN memory bytes,
  ANN tombstone entries, and persisted `records.bin` / `vectors.bin` sidecar
  byte sizes. `bench_agent_memory --compare-ann` adds `ivf_fast` and
  `ivf_recall` profiles, and `zepto_http_server` accepts
  `--agent-memory-ann ivf` plus IVF centroid/probe tuning flags. Narrows
  BACKLOG P3 "Agent Memory stronger ANN family".

- [x] **P3 Agent Memory real embedding fixture** (devlog 171) —
  `bench_agent_memory` now accepts `--fixture real --embedding-file PATH` for
  vector-only precomputed embedding files. The loader accepts comma, semicolon,
  or whitespace-separated finite floats with optional brackets and comments,
  validates consistent dimensions, defaults `--records` to the vector count,
  and uses the loaded vectors for seeded memories, cache entries, and recall
  queries. Narrows BACKLOG P3 "Agent Memory stronger ANN family".

- [x] **P3 Agent Memory clustered ANN fixture** (devlog 170) —
  `bench_agent_memory` now accepts `--fixture mixed|semantic|clustered`,
  preserving the old `--semantic-fixture` alias while adding deterministic
  clustered embeddings built from center-plus-noise vectors. Recall queries use
  the same fixture distribution, and the ANN decision table now prints the
  fixture name so sparse projection and HNSW comparisons are easier to audit.
  Narrows BACKLOG P3 "Agent Memory stronger ANN family".

- [x] **P3 Multi-node Agent Memory capacity rollback** (devlog 169) —
  `AgentMemoryEvictionEvent` now carries rollback snapshots for automatic TTL,
  tenant-quota, and capacity evictions. Owner-side HTTP/RPC writes restore those
  evicted entries when primary durability fails, and restore only the failed and
  later eviction tombstones when tombstone persistence fails after partial
  success. A sync-replication missing-replica regression verifies that failed
  memory/cache writes do not leave capacity-eviction side effects behind.
  Closes BACKLOG P3 "Multi-node Agent Memory".

- [x] **P3 Multi-node Agent Memory failover status** (devlog 168) —
  `AgentMemoryOwnerFailoverResult` now reports source/replacement node ids,
  source/new ring epochs, replay promotion, degraded state, and missing
  replay-source status. Local `/api/ai/stats` includes the last owner-failover
  status so operators can distinguish clean successor replay from a degraded
  failover with no usable replay source. Narrows BACKLOG P3 "Multi-node Agent
  Memory".

- [x] **P3 Multi-node Agent Memory eviction tombstones** (devlog 167) —
  `AgentMemoryStore` now reports automatic TTL, tenant-quota, and capacity
  eviction tombstone keys from `put_memory()` and `store_cache()`. Owner-side
  HTTP/RPC writes persist those keys through the existing delete WAL
  prepare/commit path, so restart replay and replica shard adoption do not
  resurrect entries evicted by a live owner write. Narrows BACKLOG P3
  "Multi-node Agent Memory".

- [x] **P3 Multi-node Agent Memory cluster stats** (devlog 166) — adds
  `GET /api/ai/stats?scope=cluster`, backed by a new Agent Memory stats
  `TcpRpc` payload. Routed deployments now return aggregate memory/cache,
  eviction, snapshot, quota, and ANN counters across reachable Agent Memory
  nodes plus per-node stats and `partial_failures` for missing or invalid
  remote stats responses. Default `/api/ai/stats` remains local and
  backward-compatible. Narrows BACKLOG P3 "Multi-node Agent Memory".

- [x] **P3 Context trace/replay** (devlog 165) — adds
  `examples/agent_memory/context_trace.py` plus AgentOps schema tables
  `context_traces` and `context_replay_events`. The helper emits SQL rows that
  explain why each memory entered a prompt and record surrounding time-series
  replay snapshots for audit/debug workflows. Closes BACKLOG P3 "Context
  trace/replay".

- [x] **P3 OpenTelemetry/LLM trace ingest mapping** (devlog 164) — adds
  `examples/agent_memory/otel_mapping.py`, a dependency-free mapper from OTLP
  JSON-style GenAI spans into AgentOps SQL INSERT statements. It maps
  provider/model calls, prompt/completion token counts, cache-hit attributes,
  tool-call spans, latency, and model errors; the AgentOps schema now includes
  `llm_errors`. Closes BACKLOG P3 "OpenTelemetry/LLM trace ingest mapping".

- [x] **P3 Agent Memory tenant/namespace eviction quotas** (devlog 163) —
  adds `AgentMemoryTenantQuota` and `AgentMemoryEvictionConfig::tenant_quotas`
  for scoped memory/cache retention limits. Quotas match a whole tenant when
  `namespace_id` is empty or one tenant namespace when it is set, evict only
  matching entries before global caps run, preserve pinned-memory overflow
  behavior, and expose configured quota count through `/api/ai/stats` and
  `zepto_agent_memory_tenant_quotas`. Closes BACKLOG P3 "Tenant-scoped Agent
  Memory eviction".

- [x] **P3 Agent Memory snapshot failure/latency metrics** (devlog 162) —
  adds local Agent Memory snapshot observability to the store, HTTP stats, and
  Prometheus output. `save_to_directory()` records the last snapshot attempt
  duration and total failed snapshot attempts; `/api/ai/stats` exposes
  `snapshot_latency_seconds` and `snapshot_failures_total`; `/metrics` exports
  `zepto_agent_memory_snapshot_latency_seconds` and
  `zepto_agent_memory_snapshot_failures_total`. Closes BACKLOG P3 "Agent Memory
  snapshot failure/latency metrics".

- [x] **P3 Agent Memory agent-only EKS harness mode** (devlog 161) — adds
  `tests/k8s/run_eks_bench.sh --agent-only` for rerunning only the Agent
  Memory E2E stage on amd64 and arm64 after the core EKS compat/HA harness is
  already green. The mode keeps EKS wake/node-readiness checks, image repo/tag
  overrides, cleanup, result summaries, and `--keep` handling, while skipping
  compat/HA and native engine benchmark stages. Closes BACKLOG P3 "EKS Agent
  Memory agent-only harness mode".

- [x] **P5 Telegraf external output plugin** (devlog 160) — adds
  `zepto-telegraf-output`, a Telegraf `outputs.execd` writer that reads
  Influx line protocol from stdin, maps metrics into ZeptoDB
  `(symbol, price, volume, timestamp)` tick rows, and sends HTTP SQL INSERT
  batches to the ZeptoDB server. The tool supports destination table,
  Bearer-token auth, no-auth tenant headers, symbol tag selection,
  measurement-as-symbol fallback, price/volume field mapping, numeric scales,
  timestamp precision conversion, batch size, and fail-on-parse-error mode.
  New parser/mapping coverage exercises escaped tags and string fields,
  malformed input, missing/non-numeric fields, SQL escaping, unsafe table
  names, timestamp unit parsing, and timestamp overflow. Closes BACKLOG P5
  "Telegraf output plugin".

- [x] **P9 factory 10KHz live competitor run** (devlog 159) — adds a
  Docker-backed factory proof harness that starts real local deployments of
  InfluxDB and TimescaleDB, runs the same deterministic 10KHz factory workload
  against ZeptoDB/InfluxDB/TimescaleDB, and records verified row counts in the
  existing JSONL competitor summary. The closure run
  `bench-results/factory-10khz/p9-live-20260603` sustained 10,000 rows/sec for
  60 seconds on each system with 600,000 inserted and 600,000 verified rows,
  zero failed writes, and no skipped competitors. P9 now has no open backlog
  items.

- [x] **EKS full rebalance scenario integrity repair** (devlog 158) —
  cluster-mode SQL `SELECT` now routes through `QueryCoordinator`, table ids
  are stable across pods, and peer single-tick RPC ingest drains the owner
  pipeline before acknowledging forwarded HTTP INSERTs. The fast EKS harness
  now requires an explicit scenario PASS in each result file and waits for the
  x86_64 and arm64 benchmark jobs by PID. Verified with focused local
  distributed routing tests and a clean EKS full `--scenario all --arrow-smoke`
  run where basic, add/remove, pause/resume, heavy query, back-to-back, and
  status polling all passed on both architectures.

- [x] **P9 OPC-UA live HA + server mode** (devlog 155) — adds
  `OpcUaConsumer::read_history()` for live open62541 `HistoryRead_raw`
  backfills when `UA_ENABLE_HISTORIZING` is available, reusing the existing
  subscription decode/quality/routing path. Adds `OpcUaServer` to expose
  configured ZeptoDB symbols as OPC-UA Int64 variable nodes and publish current
  values through `publish_value()`. Default builds fail closed without
  open62541 while keeping replay/snapshot contracts testable. Verified with
  focused OPC-UA HA/server-mode tests and the full `OpcUa*` suite.

- [x] **P9 OPC-UA production-profile closeout** (devlog 154) — adds the
  `zepto-opcua-browse` address-space discovery CLI, array expansion to
  per-element `TickMessage` rows, UA String mapping to dictionary/symbol codes,
  Structured-field hooks with engineering-unit metadata, Historical Access
  replay hooks, and Alarms & Conditions event hooks. The live open62541
  callback path now forwards array and string variants into the same public
  hooks. `tools/run-factory-10khz-competitor-bench.sh` standardizes the
  ZeptoDB/InfluxDB/TimescaleDB factory proof run, with explicit skipped/failed
  competitor recording. Verified with `OpcUa*` tests and browse CLI default
  stub validation.

- [x] **P9 Physical AI closeout** (devlog 153) — completes the remaining ROS 2
  roadmap track and logistics documentation slice. `TypedProfile` ROS 2 rows
  now forward through cluster RPC when the table-scoped partition owner is
  remote; the SQL engine supports `haversine`, `ST_Distance`, and `ST_Within`
  over `FLOAT32`/`FLOAT64` latitude and longitude columns; and new docs and
  examples cover Isaac Sim/digital twin replay, robot RL replay, LiDAR ASOF
  JOIN, fleet anomaly detection, robot/factory edge deployment, Physical AI
  use cases, cold-chain audit recipes, logistics design, and logistics
  benchmark workloads. Verified with focused SQL spatial tests and ROS 2 typed
  row/RPC tests.

- [x] **ROS 2 schema-aware typed ingest** (devlog 152) — adds
  `Ros2IngestMode::TypedProfile` and `ZeptoPipeline::ingest_typed_row()` for
  standard Physical AI messages stored as wide typed tables instead of scalar
  `TickMessage` rows. Typed schemas cover IMU, JointState, Odometry, TFMessage,
  and LaserScan with `timestamp`, `recv_ts`, robot/session/topic/frame
  metadata, quality, and profile-specific `FLOAT64`/`SYMBOL`/count columns.
  The bridge validates typed tables against `SchemaRegistry`, default-fills
  user-added table columns, supports live ROS and rosbag2 typed paths, and SQL
  now reads `SYMBOL`, `STRING`, `INT32`, `BOOL`, `FLOAT32`, and `FLOAT64`
  columns through type-aware accessors in simple SELECT/WHERE paths. Verified
  with the default build, `Ros2ConsumerTest.*` 41/41, and the RoboStack Jazzy
  smoke with ROS scalar + standard + typed profiles + rosbag2 enabled, where
  `Ros2ConsumerTest.*` passed 46/46 including a live IMU typed-profile publish
  into ZeptoDB.

- [x] **ROS 2 standard message profiles** (devlog 151) — adds
  `Ros2IngestMode::StandardProfile` for `sensor_msgs/msg/Imu`,
  `sensor_msgs/msg/JointState`, `nav_msgs/msg/Odometry`,
  `tf2_msgs/msg/TFMessage`, and `sensor_msgs/msg/LaserScan`. The bridge now
  flattens configured standard message fields into the existing table-aware
  scalar ingest path, expands JointState and TF arrays with `symbol_id +
  index`, summarizes finite LaserScan range/intensity values, and supports the
  same profiles in live ROS and rosbag2 paths when standard packages are
  available. Verified with the default non-ROS build, `Ros2ConsumerTest.*`
  36/36, and the RoboStack Jazzy smoke with ROS scalar + standard profiles +
  rosbag2 enabled, where `Ros2ConsumerTest.*` passed 40/40 including a live
  IMU standard-profile publish into ZeptoDB.

- [x] **EKS rebalance bench hardening** (devlog 150) — `bench_rebalance`
  now has a non-rebalance `smoke` scenario, bounded rebalance trigger failure
  handling with preserved HTTP status/body, and configurable
  `--rebalance-timeout-sec`. The fast EKS x86_64/arm64 harness injects the
  bench Enterprise license, wraps benchmarks with `--bench-timeout`, reports
  smoke-only summaries correctly, and deletes bench NodeClaims during teardown.
  EKS verification passed Stage 4 SQL + Arrow smoke, Stage 5
  `bench_rebalance --scenario smoke`, Stage 6 `PASS/PASS`, and Stage 7 cleanup
  with NodePool CPU/status CPU and bench node count all at `0`.

- [x] **Arrow-enabled EKS bench images** (devlog 149) — the x86_64 and
  arm64 EKS bench images now build with `-DZEPTO_USE_FLIGHT=ON` and Apache
  Arrow runtime packages, so `/insert/arrow` can be verified on the
  `zepto-bench` Auto Mode cluster instead of only checking the no-Arrow `406`
  fallback. `run_arch_comparison_fast.sh` now has `--arrow-smoke` for posting a
  generated Arrow IPC payload from each load generator pod and
  `--skip-benchmark` for endpoint-only validation. EKS verification pushed
  `bench-x86` and `bench-arm64`, deployed both, and confirmed
  `{"inserted":3,...}` on both architectures.

- [x] **Arrow IPC ingest endpoint** (devlog 147) — adds
  `POST /insert/arrow` for Apache Arrow IPC RecordBatchStream ingestion.
  The server decodes `sym`/`symbol`, `price`, `volume`, optional `timestamp`,
  and optional `msg_type` columns into `TickMessage` batches and ingests them
  through `QueryExecutor::ingest_tick_batch()` so table_id resolution, cluster
  routing, synchronous drain, and `SchemaRegistry::mark_has_data()` match SQL
  `INSERT`. String symbols are interned through the pipeline dictionary,
  Arrow timestamp arrays are converted to nanoseconds, missing timestamps get
  ingest-time ns stamps, `price_scale` / `volume_scale` support decimal inputs,
  table ACL and tenant namespace checks are enforced in the HTTP route, and
  no-Arrow builds return `406`. New HTTP Arrow coverage brings the focused
  `*ArrowIpc*:*HttpArrow*` suite to 14/14 passing. Closes BACKLOG P4
  "Arrow IPC ingest endpoint".

- [x] **ROS 2 rosbag2 import/replay** (devlog 146) — adds optional
  `rosbag2_cpp` detection under `-DZEPTO_USE_ROS2=ON` and extends
  `Ros2Consumer` with `Ros2BagConfig`, `Ros2BagStats`, `import_bag()`, and
  `replay_bag()`. The first bag path supports configured
  `std_msgs/msg/{Float64,Float32,Int64,Int32,UInt64,UInt32}` scalar topics,
  preserves rosbag send/receive timestamps, filters explicit topic allowlists,
  skips or fails on unknown topics by policy, and writes through the same
  table-aware `ZeptoPipeline` path as live subscriptions. Verified with the
  RoboStack Jazzy smoke: `rosbag2_cpp/storage` detected, generated sqlite3
  bags imported into ZeptoDB, live rclcpp pub/sub still passed, and
  `Ros2ConsumerTest.*` passed 31/31.

- [x] **ROS 2 runtime smoke packaging** (devlog 145) — adds
  `tools/run-ros2-smoke.sh` and `docs/operations/ROS2_SETUP.md` so developers
  and CI workers can reproduce the ROS 2 Jazzy/RoboStack install, ROS CLI
  pub/sub smoke, focused ROS-enabled ZeptoDB build, and `Ros2ConsumerTest.*`
  run from one command. The smoke includes `lttng-ust` for RoboStack
  `tracetools` link dependencies and keeps the connector build small by
  disabling embedded DuckDB.

- [x] **ROS 2 live scalar subscriber** (devlog 144) — first end-to-end live
  ROS 2 ingest path for Physical AI. With `-DZEPTO_USE_ROS2=ON` and
  `rclcpp`/`std_msgs` available, `Ros2Consumer::start()` creates a private ROS
  2 context, node, executor, and `std_msgs/msg/{Float64,Float32,Int64,Int32,
  UInt64,UInt32}` subscriptions, maps each `data` field into the existing
  scalar sample path, and writes table-scoped rows through `ZeptoPipeline`.
  Verified on Amazon Linux 2023 with RoboStack Jazzy under
  `/home/ec2-user/ros2_jazzy`: ROS CLI pub/sub smoke received `data: 42.5`,
  ROS-enabled `zepto_ros2`/`zepto_tests` build passed, and
  `Ros2ConsumerTest.*` passed 27/27 including a live `rclcpp` publish →
  ZeptoDB table ingest test.

- [x] **ROS 2 connector skeleton** (devlog 143) — first implementation slice
  of the ROS 2 / Physical AI roadmap. Adds optional `ZEPTO_USE_ROS2` CMake
  probing, a `zepto_ros2` static library, public `Ros2Consumer` C++ skeleton,
  config validation, ROS time conversion, scalar sample → `TickMessage`
  mapping, table-aware ZeptoPipeline routing, local/remote route counters,
  Prometheus/OpenMetrics formatter, and no-live-ROS unit coverage.

- [x] **EKS Agent Memory E2E** (devlog 141) — adds a real ZeptoDB Agent Memory EKS test that runs on both x86 and arm64 bench images, covers memory put/search/context, exact and semantic cache, tenant isolation, tombstones, stats/metrics, and restart persistence. The `run_eks_bench.sh --k8s-only` harness now includes the Agent Memory stage and the final EKS run passed amd64 compat 27/27, amd64 HA 11/11, arm64 compat 27/27, arm64 HA 11/11, and Agent Memory E2E 2/2.

- [x] **Agent Memory deterministic owner failover** (devlog 138) — adds `HttpServer::handle_agent_memory_owner_failover()` as the automatic recovery hook for Agent Memory failover callbacks and wires it into `zepto_http_server --failover-enabled` for non-HA cluster mode. Surviving pods advance to the new routed ring epoch, rewrite their local shard manifest under that epoch, and only the deterministic successor adopts the failed owner's persisted shard via snapshot/WAL replay. Tests cover successor adoption, non-successor no-op behavior, empty survivor rejection, and compatibility with older same-node shard manifests during epoch advance.

- [x] **Agent-attached time-series demos** (devlog 129) — adds `examples/agent_memory/agent_attached_timeseries_demo.py`, a runnable five-vertical demo that pairs live ZeptoDB time-series tables with scoped `MemoryRecord` context for finance/HFT, IoT smart factory, observability/APM, robotics fleet, and game/live-ops workflows. Each scenario installs a table, inserts representative timeline rows, stores domain memory with metadata, retrieves context under a token budget, and builds the attached-agent prompt that combines "what happened" with "what the agent learned." Example tests cover all verticals, scoped metadata, empty-row behavior, and timestamp-zero validity.

- [x] **Agent Memory background ANN rebuild** (devlog 127) — lazy ANN search no longer performs dirty sparse/HNSW rebuilds inline. `AgentMemoryStore` now owns a background ANN rebuild worker, coalesces duplicate rebuild requests by generation, falls back to exact scan until a fresh index is ready, keeps explicit `rebuild_ann_index()` as the synchronous startup/benchmark path, and avoids dirtying ANN for metadata-only memory updates that preserve tenant, namespace, and embedding. Append-only inserts into a clean ANN index remain incremental.

- [x] **Agent Memory HNSW backend** (devlog 123) — adds optional hnswlib-backed ANN behind `ZEPTO_ENABLE_HNSWLIB=ON` plus `AgentMemoryAnnMode::Hnsw`, `--agent-memory-ann hnsw`, and compare-bench profiles `hnsw_fast` / `hnsw_recall`. HNSW uses normalized vectors with L2 distance, preserving cosine ordering for unit vectors. On the 100K semantic-only fixture, `hnsw_recall` reached 0.875 average recall@16 with 0.48 ms search p50 and 1.79 ms context p50, but rebuild cost was 35.6 s. Mixed-ranking recall remained weak because final Agent Memory ranking includes importance, recency, pinned status, and access count. HNSW is therefore a useful optional comparison backend, not the default ANN path.

- [x] **Agent Memory ANN compare benchmark** (devlog 121) — adds read-only `MemoryQuery::update_access = false` diagnostics plus `bench_agent_memory --compare-ann --recall-queries N` so exact scan, sparse-fast, and sparse-wide ANN profiles can be compared from one seeded store without access-count/recency drift. 100K 128-dim comparison on the current host keeps exact scan at 8.24 ms search p50, shows sparse-fast at 0.75 ms with 0.042 average recall@16, and sparse-wide at 5.97 ms with 0.333 average recall@16, reinforcing HNSW/IVF as the next ANN evaluation track.

- [x] **Agent Memory Layer** (devlog 120) — additive AI context subsystem on top of ZeptoDB Core. Adds `zeptodb::ai::AgentMemoryStore` for in-memory `MemoryRecord` put/get/search, TTL filtering, write-time TTL scan skipping for non-expiring stores, parallel exact top-K pinned/importance/recency/access-count ranking, bounded eviction, token-budget context assembly, exact normalized prompt cache, semantic cache fallback, and optional sparse-projection ANN candidate generation with client-supplied `float32[]` embeddings. HTTP API now exposes `/api/ai/stats`, `/api/ai/memories`, `/api/ai/memories/search`, `/api/ai/context`, `/api/ai/cache/store`, and `/api/ai/cache/lookup`; Prometheus metrics expose memory/cache counts, embedding dimension, eviction counters, capacity limits, and ANN counters; `zepto_py` exposes matching `connection.memory` and `connection.cache` helpers. v0 deliberately avoids server-side LLM calls and embedding provider calls. Persistence is supported via Agent Memory sidecar snapshots (`records.bin` + `vectors.bin`) using `--agent-memory-dir`, defaulting to `<hdb-dir>/agent_memory` when HDB is enabled, with configurable `--agent-memory-flush-every` cadence and stop-time force flush. CLI capacity knobs `--agent-memory-max-memories` and `--agent-memory-max-cache-entries` bound growth while protecting pinned memories; `--agent-memory-ann auto|sparse_projection` enables experimental ANN. `examples/agent_memory/` includes provider-cache and LangGraph-style flows with mock providers, optional OpenAI Responses, Anthropic Messages, and LangGraph adapters, AgentOps telemetry schemas, and a production-shaped turn demo. `bench_agent_memory` establishes scan/ANN baselines and supports sweep-mode ANN threshold decisions; the current-instance 128-dim exact scan is under the 10 ms target at 100K and 300K records and ~16.7 ms at 1M, while sparse projection remains a recall-sensitive ANN baseline. New tests cover store behavior, invalid embeddings, TTL, tenant filtering, top-K ordering, ANN tenant partitioning, eviction, stats/metrics, exact/semantic cache, concurrent puts, HTTP happy/error paths, persistence round trips, deferred stop-time flush, Python wrappers, example control flow, adapter calls, AgentOps schema installation, and production demo telemetry. Files: `include/zeptodb/ai/agent_memory.h`, `include/zeptodb/ai/ann_index.h`, `src/ai/agent_memory.cpp`, `src/ai/ann_index.cpp`, `src/server/http_server.cpp`, `tools/zepto_http_server.cpp`, `zepto_py/connection.py`, `examples/agent_memory/agentops_schema.py`, `examples/agent_memory/production_agent_demo.py`, `examples/agent_memory/`, `tests/unit/test_agent_memory.cpp`, `tests/python/test_agent_memory_client.py`, `tests/python/test_agent_memory_examples.py`, `tests/bench/bench_agent_memory.cpp`, API/design/devlog docs.

- [x] **Arrow IPC query response** (devlog 119) — `POST /` (port 8123, ClickHouse-compatible) now honours Arrow IPC content negotiation. Triggered by `Accept: application/vnd.apache.arrow.stream`, `?default_format=Arrow` (ClickHouse-style), or `?format=arrow`; response is an Arrow IPC RecordBatchStream (`application/vnd.apache.arrow.stream`) with an `X-Zepto-Format: arrow-stream` header. JSON remains the default. Errors (parse, executor, ACL, tenant denial) **always** return JSON regardless of `Accept` — only successful result sets get encoded as Arrow (matches ClickHouse). When the build was made without Arrow, the same request returns `406 Not Acceptable` with a JSON error body. From the Arc 2026-05-13 competitive analysis: same DuckDB engine, ~2.86× faster than JSON on large result sets because JSON encoding was the bottleneck. Drop-in for Pandas/Polars/BI tools. Code organisation: extracted shared `arrow_ipc.{h,cpp}` from `flight_server.cpp` (single source of truth for `to_arrow_type` / `build_schema` / `result_to_batch`); new tiny `zepto_arrow_ipc` static lib linked from both `zepto_server` and `zepto_flight`. Encoder also now maps `SYMBOL` columns (via the result's `symbol_dict`) to Arrow `utf8` — the previous Flight server returned raw int64 codes for SYMBOL, which the Python/Polars side could not interpret. New `tests/unit/test_http_arrow_ipc.cpp` (7 tests covering happy path INT64+FLOAT64 with JSON parity, empty result, both query-param triggers, error-path-stays-JSON, 406-when-Arrow-disabled, and an encoder-level STRING + symbol_dict → utf8 unit test). Full local suite 1302 / 1302 PASS + 1 SKIPPED (opt-in S3) + 3 disabled — no regression on existing 38 Http/Flight tests after the helper extraction. Build gate: `ZEPTO_USE_FLIGHT=ON` (default ON). Closes BACKLOG P4 "Arrow IPC query response" (S effort). Files: `include/zeptodb/server/arrow_ipc.h` (new), `src/server/arrow_ipc.cpp` (new), `src/server/flight_server.cpp` (helpers extracted), `src/server/http_server.cpp` (POST / content-negotiation), `include/zeptodb/server/http_server.h` (header doc comment), `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/unit/test_http_arrow_ipc.cpp` (new), 4 doc files.

- [x] **S3 Parquet sink connector — operator surface** (devlog 118) — finishes the cold-tier S3 Parquet sink experience that has shipped as C++ infra since devlog 012. Adds `S3Layout::HIVE` (default for new deployments) emitting `s3://{bucket}/{prefix}/year=YYYY/month=MM/day=DD/symbol={ID}/{ID}-{hour_epoch}[-{hash}].parquet` keys auto-discoverable by Athena/DuckDB/Polars/Spark; `S3Layout::FLAT` retained byte-identical for backward compat. New Helm `coldTier:` block (default disabled), matching `--cold-tier-*` CLI flags + `ZEPTO_COLD_TIER_*` env vars (CLI > env > default). Per-pod 4-char hex hostname-hash suffix on HIVE filenames for multi-pod collision protection (FNV-1a, computed once at `S3Sink` ctor; `S3SinkConfig::disable_host_hash` for deterministic tests). Startup INFO log line lists the resolved cold-tier config (no secrets). New operator recipe doc `docs/operations/COLD_TIER_S3.md` (Helm + CLI + S3 IAM minimum policy + Athena/DuckDB/Polars/Spark read patterns + cost note). +14 tests across new `test_s3_sink.cpp` / `test_parquet_writer.cpp` / `test_cold_tier.cpp`. Closes the P5 backlog row. Files: `include/zeptodb/storage/s3_sink.h`, `src/storage/s3_sink.cpp`, `src/storage/flush_manager.cpp`, `tools/zepto_http_server.cpp`, `tests/CMakeLists.txt`, 3 Helm templates, 6 docs. Build clean (`zepto_storage`, `zepto_http_server`, `zepto_tests`), `helm lint` clean, full suite 1310 / 1308 PASS / 1 SKIPPED (opt-in live S3) / 1 pre-existing flake (`TcpRpc.StatsRequest_NoCallback_ReturnsEmptyJson`, devlog 096).

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
- [x] **Perf recovery + cross-arch compiler unification** (devlog 097) — three sequential fixes after the multi-table refactor (`5666f1b`) destabilised the bench surface. (1) `MaterializedViewRegistry::on_tick()` now takes a relaxed-load empty-check fast-path before the registry mutex, so zero-MV runs pay ~0 cost per tick (BENCH 1 peak recovered 4.66 M → 4.87 M ticks/sec; MV-off and MV-on VWAP 1M p50 now within 1 µs of each other). (2) `query_vwap()` HDB-fallback block (24 lines) extracted to `[[gnu::cold, gnu::noinline]]` helper `query_vwap_hdb_fallback()`; call-site is a single `[[unlikely]]` branch — shrinks the hot icache footprint and removes a cold-path deoptimizer (perf-neutral at 1M rows, semantically correct). (3) `CMakeLists.txt` soft-defaults to `clang++-19` when neither `-DCMAKE_*_COMPILER` nor `CC`/`CXX` are set — eliminates the silent gcc-11.5.0 drift that produced a phantom 55 µs VWAP 1M p50 discrepancy (637 µs gcc vs 582 µs clang on the same commit `875a4c3`) on dev boxes and on the persistent Graviton SSH host. Explicit `-DCMAKE_CXX_COMPILER=g++` still works (GNU 11.5.0 honored with no "Auto-selected" message). `tools/run-full-matrix.sh` stage 8 got an idempotent `if [[ ! -f build/build.ninja ]]; then cmake -B build ...; fi` guard so compiler swaps / `build/` purges don't break the remote ninja step. Installed `clang19 clang19-devel llvm19-devel` on Graviton `ec2-user@172.31.71.135` — `clang 19.1.7 (AWS 19.1.7-13.amzn2023.0.2)` exact match for the x86_64 baseline, so cross-arch bench now runs under identical compiler/version. `tests/bench/bench_pipeline.cpp` split BENCH 2 (MV-off) / BENCH 2b (MV-on) for attributable measurement. Final matrix: **2 722/2 722 tests PASS** across x86_64 + aarch64 parallel, 97.75 s wall. Final bench (5-run medians, clang-19 both archs): x86 VWAP 1M p50 687 µs MV-off / 688 µs MV-on; arm64 634.7 µs MV-off / 630.9 µs MV-on (Graviton now faster than x86 on VWAP, consistent with devlog 023 Highway-NEON win, previously invisible under gcc). Residual ~105 µs gap vs the 582 µs baseline is NOT compiler drift (both hosts clang-19 at measurement); carried from the multi-table refactor's MV-correctness fix and tracked in `BACKLOG.md` P7 "VWAP 1M p50 sub-600 µs restore". No Dockerfile / CI workflow / `.githooks/pre-push` edits (they already force clang-19 explicitly and/or piggyback on the `build/` that stage 8 now configures correctly).
- [x] **PartitionKey packing + VWAP recovery closeout** (devlog 098) — sequential follow-up to 097: (1) `PartitionKey::operator==` and `PartitionKeyHash` rewritten in `include/zeptodb/storage/partition_manager.h` to pack `(symbol_id[0:32], table_id[32:48], hour_epoch&0xFFFF[48:64])` into a single `uint64_t`, XOR the full `hour_epoch` back in, and hash via one splitmix64 round (`xor-shift-33; mul 0xff51afd7ed558ccd; xor-shift-33`); equality becomes two `uint64_t` compares, both operators `[[gnu::always_inline]]`. Header-only, 16 B ABI preserved. (2) Post-packing `perf` profile showed the hash/eq is <0.01 % in both pre and post profiles (partition map hit is per-partition, not per-row), so packing is a no-regression ingest-side win with no query-path effect. (3) Realistic rebuilt baseline (3-run clang-19 median) is 625 µs, not the 582 µs best-case cited in 097; post-packing median 669 µs → gap is **~44 µs (7.0 %)**, not 105 µs. (4) Residual classified **inherent (compiler register allocation)** — `query_vwap` inner-loop disasm is byte-identical to baseline (36 instr/iter) but clang-19 spills the `v_sum` int64 accumulator to `-0x40(%rbp)` under the multi-table version's raised register pressure (`table_id` threading + `[[unlikely]]` HDB call site); store-forwarding penalty = 8.5 % of `query_vwap` ≈ 25 µs. Only recovery path is a medium refactor (~30 LOC kernel extraction to `[[gnu::hot, gnu::flatten]]`), marginal ROI — deferred. (5) Final 5-run medians on x86 clang-19: BENCH 1 peak 4.81 M t/s, VWAP 1M MV-off 697 µs, MV-on 696 µs; arm64 Graviton 3-run medians: 3.98 M t/s / 632.7 µs / 631.8 µs. (6) Verification: 152/152 `*Partition*:*Storage*:*Ingest*:*Pipeline*:*TableScoped*:*VWAP*:*Query*` tests PASS; full local matrix 5/5 PASS (2722 tests across x86 + arm64, 95.34 s wall); 1× `QueryCoordinator.TwoNodeRemote_GroupBy_Concat` flake on arm64 stage 8 retry-passed (known `tcp_rpc_pool` family, devlog 096). No `.cpp` / test / CI change — header + three doc files only. P7 updated with the inherent-residual note rather than closed; the code-side recovery remaining (~25 µs) requires the kernel-extraction refactor and is out of scope for this iteration.
- [x] **Ingest path recovery — `store_tick` column-pointer caching** (devlog 099) — closed the residual ~5% BENCH 1 ingest regression left after 097/098. Root cause: post-multi-table `ZeptoPipeline::store_tick` was calling `Partition::get_column(const std::string&)` up to 6 times per tick (vs 4 in baseline) because the FLOAT64/INT64 price-type branch re-looked up `COL_PRICE` for both `->type()` and `->append<>` — `get_column` is a linear `memcmp` scan over `columns_`, so `__memcmp_evex_movbe` climbed from 14.42% → 20.85% of samples (+6.43 pp, matching the measured +12.9% instruction count). Fix: 10 LOC net in `src/core/pipeline.cpp` to cache `ts_col / px_col / vol_col / mt_col` as local `ColumnVector*` once per tick; FLOAT64 branch now CSE-friendly. No header, no public API, no test change. Post-fix 5-run medians on x86 clang-19: batch=1 4.76 M/s (−1.7% vs 4.84 baseline), **batch=64 5.06 M/s (−0.8% vs 5.10 baseline)**, batch=512 5.05 M/s, batch=4096 5.05 M/s (+0.6%), batch=65535 5.04 M/s (+1.0%) — all within ±2% of `875a4c3` baseline. Graviton 3-run medians: 4.03 / 3.97 / 3.99 / 4.00 / 3.99 M/s. Verification: 89/89 `*Ingest*:*Pipeline*:*Storage*:*Partition*:*TickPlant*:*Arena*:*ColumnStore*:*TableScoped*` tests PASS; full local matrix 5/5 PASS (1361/1361 on both x86 and aarch64, wall ~175 s). P7 ingest-throughput portion closed — residuals remaining are `clock_gettime` vdso (inherent) and arena first-touch `memset` (inherent, HugePages unavailable on bench host).
- [x] **Stage 7 / `run_eks_bench.sh` fully rewired for EKS Auto Mode** (devlog 100, closes devlog 095 follow-up) — two-part fix finalised end-to-end. **(Part 1, script)** `tests/k8s/run_eks_bench.sh` stage 2 no longer applies the self-managed Karpenter `EC2NodeClass` / `arm64-bench` `NodePool` pair (that CRD is absent on Auto Mode clusters by design — devlog 095). Stage 2 now (a) verifies the persistent `zepto-bench-arm64` NodePool, (b) calls `eks-bench.sh wake` when `limits.cpu=0`, (c) applies the 3-replica `arm64-trigger` pause Deployment, (d) keeps the devlog-094 #10 `≥3 arm64 Ready → skip` warm-cluster short-circuit. Cleanup trap cleaned up (removed two `kubectl delete nodepool/ec2nodeclass arm64-bench` lines); header comment updated. **(Part 2, NodePool bootstrap)** The persistent `zepto-bench-x86` / `zepto-bench-arm64` NodePools were deleted and recreated with Auto-Mode-compatible requirement keys: `eks.amazonaws.com/instance-family` (c7i/m7i/r7i/c6i/m6i for x86, c7g/m7g/r7g for arm64) and `eks.amazonaws.com/instance-cpu` `Gt 2`, `Lt 17` (caps at 16-vCPU instances). Originals backed up under `/tmp/zepto-nodepool-backup/{x86,arm64}.{yaml,old.yaml,new.yaml}`. Both NodePools reach `ValidationSucceeded=True` with `cpu=0` (asleep). **Live verification:** `./tools/run-full-matrix.sh --stages=7 --keep-going` wall 465.32 s, arm64 3/3 Ready in ~2 min, all three test suites green — amd64 compat 27/27, amd64 HA+perf 11/11, arm64 all 38/38 (**76/76 total, 0 failures**; first-ever green K8s matrix under Auto Mode). Per-arch perf within ±20% of `K8S_TEST_REPORT.md` baselines; 0 ZeptoDB code regressions, 0 flakes. Global EXIT trap cleanup verified (both NodePools back at `limits.cpu=0`). One pre-existing unrelated infra item observed but not caused by stage 7: StatefulSet pod `zeptodb/zeptodb-1` CrashLoopBackOff due to amd64-only `:automode` image landing on an arm64 bench node (missing `kubernetes.io/arch=amd64` nodeSelector or multi-arch image) — filed as separate infra item, not blocking. Files touched: `tests/k8s/run_eks_bench.sh`, `zepto-bench-{x86,arm64}` NodePool manifests (cluster-side), `docs/devlog/100_eks_automode_fix.md`, `docs/COMPLETED.md`, `docs/BACKLOG.md`.
- [x] **Ingest Phase 1: drain_threads default + configurable ring capacity** (devlog 102) — single-pod ingest ceiling raised from ~6.6 M/s (with 34× cliff at queue saturation) to sustained multi-core throughput. Helm values: pipeline.drainThreads / pipeline.ringBufferCapacity. Phase 2 (stateless ingest tier + ingest-rate HPA) tracked in BACKLOG P8.
- [x] **Cluster-aware INSERT routing** (devlog 103) — HTTP/SQL/Python INSERT now routes via PartitionRouter ring instead of landing on the receiving pod. Silent multi-pod mis-partitioning fixed; horizontal ingest scaling now actually works. Null cluster_node preserves single-node behaviour.
- [x] **Pod placement hardening** (devlog 104) — required antiAffinity toggle (default on), topologySpreadConstraints with maxSkew=1, ingest-tuned resource defaults. Fixes silent co-location that invalidated horizontal scale-out claims.
- [x] **Data Durability** — Intra-day auto-snapshot (60s default), recovery replays on restart — max data loss ≤ 60s

## Ingestion & Feed Handlers
- [x] **Feed Handlers** — NASDAQ ITCH (250ns parsing), FIX (350ns parsing)
- [x] **Kafka consumer** — JSON/binary/human-readable decode, backpressure retry, Prometheus metrics, commit modes — 26 tests
- [x] **MQTT consumer** — IoT / Physical AI ingestion, QoS 0/1/2, topic wildcards (`#`, `+`), shared JSON/BINARY/JSON_HUMAN decoders with Kafka, Paho async client with `ZEPTO_USE_MQTT` optional-dep pattern — 18 tests (devlog 081)
- [x] **Telegraf external output plugin** — `zepto-telegraf-output` for Telegraf `outputs.execd`; parses Influx line protocol from stdin, maps metrics to ZeptoDB tick columns, sends HTTP SQL INSERT batches, supports auth/tenant headers and field/scale/timestamp mapping — 10 tests (devlog 160)
- [x] **OPC-UA connector (PoC)** — scalar-only client connector, open62541 (MPL 2.0) optional-dep scaffolding (`ZEPTO_USE_OPCUA=ON` + `find_library(open62541)`), NodeId→SymbolId mapping, Int16/32/64 + Float/Double (with per-node `value_scale`) + Boolean variant coercion, SourceTimestamp UA DateTime 1601→1970 ns conversion, single-node + multi-node routing via shared dispatch path, backpressure retry, table-aware ingest (`OpcUaConfig::table_name` → `SchemaRegistry`), license-gated on `Feature::IOT_CONNECTORS` — 22 tests (devlog 101). Follow-on production features (real UA_Client integration, Basic256Sha256 security, reconnect, Historical Access, Alarms & Conditions, structured variants, browse/discover, server mode) are closed by devlogs 105-110 and 154-155.
- [x] **OPC-UA Sprint 1 — production-blocker closeout** (devlogs 105, 106) — real open62541 UA_Client integration (2b), float-safety clamp in coerce_variant_to_int64 (2n), duplicate/empty node_id validation (2p), sector-aware default profiles Fab/Auto/Steel/Generic (2q), reconnect/timeout config knobs (2o). OPC-UA connector is now pilot-ready for industrial deployments (pending 2c security + 2k integration test in Sprint 2).
- [x] **OPC-UA Sprint 2 — first-commercial-ready closeout** (devlogs 107, 108, 109) — Basic256Sha256 security (2c), integration test against open62541 tutorial server (2k), reconnect/failover with exponential backoff + jitter (2i), UA StatusCode → TickMessage.volume quality mapping (2j). Connector is now ready for first-commercial industrial deployments (Samsung/SK/TSMC/POSCO sectors per docs/design/physical_ai_market.md).
- [x] **OPC-UA Sprint 3 — Tier-3 observability closeout** (devlog 110) — atomic stats audit (2r), `RpcClientBase` extraction closing `route_remote` test coverage across all three consumers (2s), Kafka/MQTT/OPC-UA microbench parity (2t: Kafka 2.69 M ticks/s, MQTT 2.48 M ticks/s, OPC-UA 6.69 M ticks/s cheap-path baseline). Plus Sprint-2 polish items: explicit `decode_errors` for unsupported variants, devlog-107 wording tightened, reconnect test comment. Connector is now SLA-grade.
- [x] **P8-I3-wire — HTTP INSERT cluster-aware routing** (devlog 111) — `zepto_http_server` now constructs a `CoordinatorRoutingAdapter` and calls `executor.set_cluster_node()` in cluster mode. Bridges the library-level fix from devlog 103 to the production HTTP binary. Rebalance manager now mutates the same `PartitionRouter` the adapter reads (unified under `QueryCoordinator::router()` + `router_mutex()`). Non-HA cluster mode starts a peer `TcpRpcServer` on `port + 100`. `zepto_data_node` documented as a leaf node (no wire-up needed). 4 new `CoordinatorRoutingAdapter.*` tests (1284 → 1288). EKS re-bench tracked as stage 3.
- [x] **P8-DDL-replication — DDL replication across cluster pods** (devlog 112) — `QueryCoordinator::forward_ddl_to_remotes()` fire-and-forget replicates `CREATE / DROP / ALTER TABLE` to every remote pod after the HTTP server executes the DDL locally. Uses the already-parsed `cached_ps` for classification — no extra parse, no string matching. Remote failures emit `ZEPTO_WARN` but never fail the client request. Fixes the multi-pod test/benchmark gap discovered during EKS Round 2. 4 new `DDLReplication.*` tests (1288 → 1292).
- [x] **P8-I3 — Stateless `zepto_ingest_node` binary** (devlog 113) — new `tools/zepto_ingest_node.cpp` ingest-only pod that forwards every HTTP INSERT to the right storage pod via `CoordinatorRoutingAdapter` (devlog 111). Owns zero data: self `node_id=99999` keeps the consistent-hash ring from ever picking self (Option A — zero adapter changes). DDL replicates via devlog 112. Dockerfile.bench (x86_64 + arm64) ships the binary; Helm opt-in template `ingest-deployment.yaml` + `values.yaml` `ingest:` block (disabled by default). Unlocks independent ingest↔storage scaling. Baseline 1292/1292 preserved — routing covered by `test_coordinator_routing_adapter.cpp`.
- [x] **P8-I5 — Python cluster hook** (devlog 114) — `zeptodb.Pipeline.enable_cluster_routing(self_id, peers, remove_self_from_ring=True, rpc_timeout_ms=2000)` pybind11 method lands the last Sprint-2 horizontal-ingest item. Internally builds+owns a `QueryCoordinator`, a `NodeId → shared_ptr<RpcClientBase>` peer pool, and a `CoordinatorRoutingAdapter`; wires the executor via the devlog-103 `set_cluster_node()` hook. Idempotent teardown in the adapter → peer-map → coordinator order. Argument validation is strong-exception — a bad peer tuple raises `TypeError` / `ValueError` before any internal state is touched. 5 new `TestClusterRouting` pytest cases (empty peers, self-in-ring local ingest, idempotent rewiring, unreachable peer no-segfault, malformed tuple). Python module now links `zepto_cluster`.
- [x] **P8-I4 — Ingest-rate HPA** (devlog 117) — per-pod `zepto_ingest_ticks_per_sec` gauge on `/metrics`, computed lock-free from the last two `MetricsCollector` snapshots (clamped to 0 on counter wrap). Helm chart appends a `Pods` HPA metric (`AverageValue: targetIngestRate`) when `autoscaling.ingestRateEnabled=true`; default `helm install` keeps the existing CPU 70% / memory 80% `Resource` metrics as a safety net (off-by-default avoids a hard dependency on `prometheus-adapter`). 5 new `MetricsCollectorIngestRateTest` cases + 1 new `MetricsProviderTest.PrometheusMetricsExposesIngestRate` covering `/metrics` plumbing (1293 → 1299).
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

## Marketing / Website
- [x] **Marketing site rebrand — general-purpose time-series DB** (devlog 115) — 5-page IA (`/home`, `/solutions`, `/features`, `/performance`, `/pricing`) under `web/src/app/(marketing)/**` pivots the public-facing site from "HFT/quant-only" to "general-purpose industry time-series DB." `/home` now leads with "The Time-Series Database for Physical AI, IoT, and Real-Time Analytics" + 4-stat proof strip (5.52M ticks/s · 272µs filter · 6 native feeds · kdb+-class) + 5 industry cards deep-linking into `/solutions#{physical-ai,finance,game,iot}` + `/solutions` for "+ more verticals". `/solutions` has one section per vertical with a pain → capability table → proof-point → killer-line shape pulled verbatim from `docs/business/product_positioning.md` and `docs/design/physical_ai_market.md` (Physical AI cites OPC-UA SLA-grade per devlog 110 + sector profiles per devlog 105). `/features` replaces the legacy 3-card layout with 4 capability groups (Ingest / Query / Deploy / Secure). `/performance` is new: 4×5 benchmark comparison (ZeptoDB vs kdb+ vs ClickHouse vs InfluxDB on Ingestion, 1M-row filter p50, ASOF JOIN, License cost, Deployment) + per-op detail card + methodology footer citing clang-19 / devlogs 097–098 / `docs/bench/results_multinode.md`. `/pricing` neutralises the old finance-specific copy and now targets "finance, factory floors, game backends, and Physical AI platforms," with a "Cloud-hosted tier coming soon" teaser. Technical details: in-repo Next.js 14 App Router + MUI (not the separate-repo Astro + Tailwind proposed in the old PRD); new `(marketing)/layout.tsx` provides cross-page nav; every page uses `"use client"` so `component={Link}` works under Next.js 16's server-component build; pre-existing Vitest/Playwright spec collision fixed by adding `e2e/**` to `vitest.config.ts` excludes. Tests: 4 files (`home`, `solutions`, `performance`, `pricing`) for a total of 20 Vitest cases; full suite 79/79 green, 19-page Next.js static export succeeds, zero lint issues in new/modified files. Docs: `docs/business/WEBSITE_PRD.md` rewritten to reflect the shipped IA and stack; KIRO.md + orchestrator.md devlog pointers refreshed.

---

> Client API Compatibility Matrix: [`docs/design/client_compatibility.md`](docs/design/client_compatibility.md)
 ❌ | ❌ | ✅ `/admin/audit` | ❌ | ❌ | ✅ `/admin/audit` | ❌ | ❌ | ✅ `/admin/audit` |
