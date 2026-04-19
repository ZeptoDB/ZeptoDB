# ZeptoDB Backlog

> Completed features: [`COMPLETED.md`](COMPLETED.md) | 1129 tests passing
>
> Last cleaned: 2026-04-19

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

> ✅ Done: P8-Critical, P8-High, P8-Medium all complete. Live rebalancing, Dual-write, Partial-move, Bandwidth throttling, PTP clock sync, etc.

---

## P9 — Physical AI / Industry

| Task | Why | Effort |
|------|-----|--------|
| **OPC-UA connector** | Siemens S7, industrial PLCs | M |
| **ROS2 plugin** | ROS2 topics → ZeptoDB | M |

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
| **P7** | Engine Performance | 5 | JOINs/Window virtual tables |
| **P8** | Cluster | 7 | RDMA transport, Cold query |
| **P9** | Physical AI / IoT | 2 | OPC-UA, ROS2 |
| **P10** | Extensions | 9 | UDF, Edge mode |

**Total remaining: 34 items + 4 manual tasks**

**Critical path: P2 (launch) → P4 (ClickHouse protocol) → P7 (JOINs/Window virtual tables)**
