# ZeptoDB Backlog

> Completed features: [`COMPLETED.md`](COMPLETED.md) | 1317 tests passing
>
> Last cleaned: 2026-05-15
>
> Devlog: last `141_eks_agent_memory_e2e.md` → next `142_*.md`

---

## Recent completions (last 2 weeks)

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
| **Replication-cluster vs MPP-cluster design doc** | S | New section in `docs/design/phase_c_distributed.md` formalising the architectural diff vs Arc/MotherDuck/DuckDB-replicas (HA + write-sharding vs true scatter-gather). Doubles as an enterprise-sales artefact ("scale beyond a single DuckDB") and an internal design north star. From Arc competitive analysis (2026-05-13). |

Manual tasks: DB-Engines registration, demo GIF, Show HN, Reddit (5 subs). See `docs/community/`.

---

## P3 — Agent Memory / AI Context

| Task | Why | Effort |
|------|-----|--------|
| **OpenTelemetry/LLM trace ingest mapping** | Map GenAI spans, tool calls, token counts, latency, model errors, and cache hits into ZeptoDB tables to make observability the first production-grade agent use case. | M |
| **Context trace/replay** | Explain why each memory entered the prompt and replay the time-series state that surrounded the decision; useful for debugging, audits, and prompt-injection review. | M |
| **Multi-node Agent Memory** | Foundation done in devlog 122, routed memory/cache write RPCs done in devlog 124, point memory/exact-cache lookup routing done in devlog 126, fan-out search/context merge done in devlog 128, shard-local snapshot paths with startup ownership validation done in devlog 130, local Agent Memory WAL replay done in devlog 131, remote owner WAL/fencing regression coverage done in devlog 132, semantic-cache fan-out done in devlog 133, explicit failed-owner shard adoption done in devlog 134, quorum/sync replica WAL ACK policy done in devlog 135, WAL prepare/commit markers with failed-write rollback done in devlog 136, explicit delete tombstones done in devlog 137, and deterministic owner failover orchestration done in devlog 138. Remaining gaps: tenant quotas, cluster stats, automatic TTL/capacity-eviction tombstones, replica promotion/degraded-state reporting, and full transactional rollback for capacity-eviction side effects. | L |
| **Agent Memory snapshot failure/latency metrics** | Flush cadence and memory/eviction metrics now exist; operators still need snapshot latency and failure gauges. | S |
| **Tenant-scoped Agent Memory eviction** | Global caps now exist; larger deployments may need per-tenant/namespace limits for fair sharing under memory pressure. | S |
| **EKS Agent Memory agent-only harness mode** | Add `--agent-only` to `tests/k8s/run_eks_bench.sh` so Agent Memory E2E can be rerun quickly after the core K8s compat/HA harness is already green. | S |
| **Agent Memory stronger ANN family** | Sparse projection and optional hnswlib HNSW are now comparable with the devlog 121/123 harness. HNSW gives much better semantic-only recall at low search latency, but first-build graph cost is high and mixed-ranking recall is still weak on random vectors. Devlog 125 moved rebuild work outside the store mutex and added append-only incremental maintenance for clean indexes; devlog 127 moved lazy rebuilds to a background worker with generation coalescing. Next: clustered/real embedding fixtures, update/delete graph maintenance, memory overhead, tenant-filter behavior, persisted index footprint, and IVF comparison before choosing a production default. | M |
| **Optional managed embedding provider** | Enterprise convenience only; default path remains client-supplied embeddings. | M |

> ✅ Done: v0 Agent Memory Layer (devlog 120) — `MemoryRecord` store, parallel top-K filtered search, sparse-projection ANN candidate index, context assembly, exact/semantic cache, sidecar persistence with configurable flush cadence, bounded eviction, HTTP stats/metrics, Python client, examples, optional provider/framework adapters, AgentOps schema/demo, and current-instance benchmark report.

---

## P4 — Tool Integration

| Task | Why | Effort |
|------|-----|--------|
| **ClickHouse wire protocol** | DBeaver, DataGrip, Grafana native | L |
| **JDBC/ODBC drivers** | Tableau, Excel, Power BI | L |
| **Arrow IPC ingest endpoint** (`POST /insert/arrow`) | First-class binary columnar ingest path. Removes per-tick JSON parse, enables a symbol-aware batched client. Targets the N≥2 cluster ceiling currently capped at ~90/s by the JSON HTTP bench. Pairs with the existing P8 "batched HTTP client" item. From Arc analysis (2026-05-13). | M |
| **MessagePack columnar ingest endpoint** | Wire-compatible with Telegraf and InfluxDB Line Protocol clients. Single batch decode replaces per-tick JSON parse. Smaller scope than Arrow IPC, faster to ship; complementary to it. From Arc analysis (2026-05-13). | S |

> ✅ Done: Arrow IPC query response (devlog 119) — `POST /` content negotiation via `Accept: application/vnd.apache.arrow.stream` / `?default_format=Arrow` / `?format=arrow`; ~2–3× faster than JSON on large result sets. JSON remains default; errors stay JSON regardless of Accept.

---

## P5 — Data Pipelines

| Task | Why | Effort |
|------|-----|--------|
| **Kafka Connect Sink** | Enterprise pipeline standard | M |
| **CDC connector (Debezium)** | PostgreSQL/MySQL → real-time sync | M |
| **AWS Kinesis consumer** | AWS-native streaming | S |
| **Apache Pulsar consumer** | Kafka alternative | S |
| **Telegraf output plugin** | One plugin unlocks 300+ Telegraf input integrations (industrial PLCs, server metrics, network gear). Highest leverage-per-effort connector. Arc shipped this and it visibly drove adoption. From Arc analysis (2026-05-13). | S |
| **MQTT consumer** | Industrial IoT standard. Topic → measurement mapping; QoS 0/1/2; reconnection. Currently a gap vs Arc (`internal/mqtt/`) and a P9 prerequisite for many IoT/Smart-Factory use cases. From Arc analysis (2026-05-13). | S |

> ✅ Done: S3 Parquet sink connector (devlog 118) — Hive-partitioned S3 keys, Helm `coldTier.*`, `--cold-tier-*` CLI flags, `ZEPTO_COLD_TIER_*` env vars, operator recipe doc.

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

> ✅ Done: P8-I4 ingest-rate HPA (devlog 117), P8-I5 Python cluster hook (devlog 114), P8-I3-wire (devlog 111), P8-I3 ingest node (devlog 113), P8-DDL-replication (devlog 112), Pod placement (devlog 104), Ingest Phase 1 (devlog 102), Cluster-aware INSERT routing (devlog 103). Live rebalancing, dual-write, partial-move, bandwidth throttling, PTP clock sync all shipped earlier.

---

## P9 — Physical AI / Industry

### Open items

| # | Task | Why | Effort |
|---|------|-----|--------|
| 1 | **ROS2 plugin** | `rclcpp` subscriber → `TickMessage`; bag replay; Isaac Sim hook | M |
| 2f | **OPC-UA: browse + auto-discover CLI** | Enumerate server address space, auto-populate `nodes[]` | S |
| 2d | **OPC-UA: structured & array variants** | Engineering units, array → multiple TickMessages | M |
| 2g | **OPC-UA: Historical Access (HA)** | Server-side historian backfill for initial load | M |
| 2h | **OPC-UA: Alarms & Conditions (A&C)** | Alarm events as separate tick stream | M |
| 2e | **OPC-UA: string values** | UA String → symbol columns (blocked on string-column engine) | S |
| 2l | **OPC-UA: server mode** | Expose ZeptoDB as OPC-UA server (P10 candidate) | L |
| 3 | **Physical AI reference examples** | Robot RL replay, LiDAR ASOF JOIN, CMP anomaly | S |
| 4 | **Factory 10KHz bench vs InfluxDB/TimescaleDB** | Sector-B sales proof | S |
| 5 | **Physical AI use-case docs** | Promote to first-class `docs/usecases/` vertical | S |
| 6a | **Spatial functions** (`haversine`, `ST_Distance`, `ST_Within`) | AGV collision, geofence, drone proximity | M |
| 6b | **Cold-chain immutable table** | FDA/EU GDP regulatory audit trail | S |
| 6c | **Entity-timeline recipes** | Order/pallet state tracking (WMS/OMS) | S |
| 6d | **Logistics design doc + market section** | Formalize logistics as GTM sector | S |
| 6e | **Edge deployment guide** | k3s / Docker Compose / systemd on industrial PC | S |
| 6f | **Logistics benchmark suite** | 2K AGV + 1M sorter + 50K RFID vs competitors | S |
| 6g | **Digital Twin / Isaac Sim hook** | Omniverse warehouse feed (depends on ROS2 #1) | M |

> ✅ Done: OPC-UA PoC (devlog 101), Sprint 1 (105-106), Sprint 2 (107-109), Sprint 3 (110). Connector is SLA-grade.

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
| **P2** | Visibility & Launch | 2 + 4 manual | Demo video → replication-vs-MPP design doc → Show HN → Reddit |
| **P3** | Agent Memory / AI Context | 7 | OpenTelemetry trace mapping → context trace/replay |
| **P4** | Tool Integration | 4 | Arrow IPC ingest endpoint (M) → ClickHouse wire protocol (L) |
| **P5** | Data Pipelines | 6 | Telegraf output plugin (S) → CDC connector (M) |
| **P6** | Enterprise / Cloud | 3 | Marketplace |
| **P7** | Engine Performance | 3 | JOINs/Window virtual tables |
| **P8** | Cluster | 8 | RDMA transport, Tier C cold offload (elevated) |
| **P9** | Physical AI / IoT | 17 | ROS2 plugin, OPC-UA browse CLI |
| **P10** | Extensions | 11 | Continuous queries scheduler, single-binary CLI |

**Total open: 61 items + 4 manual tasks**

**Critical path: P2 (launch) → P4 (ClickHouse protocol + Arrow IPC ingest) → P5 (Telegraf output plugin)**

> **2026-05-13 — Arc competitive analysis**: 9 new items added across P2/P4/P5/P10 and the P8 Tier C cold-offload row was elevated. Each added item is tagged "From Arc analysis (2026-05-13)" in its `Why` cell. Headline lessons: (1) batched columnar wire formats (Arrow IPC, MessagePack) are the single biggest ingest-throughput unlock; (2) Arrow IPC query responses are a near-free 2–3× win on large result sets; (3) ecosystem connectors (Telegraf/MQTT/S3 Parquet sink) are higher leverage than yet-another-streaming-source consumer; (4) our MPP-cluster vs replication-cluster distinction is a sales differentiator that deserves a formal design-doc section. We do **not** chase Arc's storage-first / batch-flush model — our memory-first / per-tick-durable / immediately-queryable architecture is the differentiator and stays.
