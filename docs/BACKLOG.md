# ZeptoDB Backlog

> Completed features: [`COMPLETED.md`](COMPLETED.md) | 1293 tests passing
>
> Last cleaned: 2026-05-01
>
> Devlog: last `113_stateless_ingest_node.md` → next `114_*.md`

---

## Recent completions (last 2 weeks)

- ✅ **Stateless `zepto_ingest_node`** (devlog 113) — ingest-only binary, forwards all ticks to storage pods. Helm opt-in. Closes P8-I3.
- ✅ **DDL replication** (devlog 112) — fire-and-forget `CREATE/DROP/ALTER TABLE` scatter-gather. Closes P8-DDL-replication.
- ✅ **HTTP INSERT cluster routing** (devlog 111) — `CoordinatorRoutingAdapter` wired in `zepto_http_server`. EKS verified (Round 2+3). Closes P8-I3-wire.
- ✅ **OPC-UA Sprint 1–3** (devlogs 101–110) — PoC → SLA-grade in 3 sprints. Real UA_Client, security, reconnect, quality mapping, microbench parity.
- ✅ **Ingest Phase 1** (devlog 102) — `drain_threads` auto-sizing + configurable ring capacity.
- ✅ **Pod placement hardening** (devlog 104) — required antiAffinity + topologySpread + resource defaults.

---

## P2 — Visibility & Launch

| Task | Effort |
|------|--------|
| **YouTube / Loom demo video** | S |

Manual tasks: DB-Engines registration, demo GIF, Show HN, Reddit (5 subs). See `docs/community/`.

---

## P4 — Tool Integration

| Task | Why | Effort |
|------|-----|--------|
| **ClickHouse wire protocol** | DBeaver, DataGrip, Grafana native | L |
| **JDBC/ODBC drivers** | Tableau, Excel, Power BI | L |

---

## P5 — Data Pipelines

| Task | Why | Effort |
|------|-----|--------|
| **Kafka Connect Sink** | Enterprise pipeline standard | M |
| **CDC connector (Debezium)** | PostgreSQL/MySQL → real-time sync | M |
| **AWS Kinesis consumer** | AWS-native streaming | S |
| **Apache Pulsar consumer** | Kafka alternative | S |

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
| **Tier C cold query offload** | Historical data → DuckDB on S3 | M |
| **Global symbol registry** | Distributed string symbol routing | M |

### Horizontal Ingest (remaining)

| Task | Why | Effort |
|------|-----|--------|
| **P8-I4 — Ingest-rate HPA** | Custom Prometheus metric `zepto_ingest_ticks_per_sec` → HPA target. Autoscale on real ingest load, not CPU/mem proxy. | S |
| **P8-I5 — Python cluster hook** | `PyPipeline.set_cluster_node()` via pybind11. C++ plumbing in place (devlog 103); pending binding. | S |
| **Bench: symbol-aware / batched HTTP client** | Current HTTP bench is latency-bound at ~90/s under N≥2 (RPC hop per non-local INSERT). Need a driver that either batches or computes ownership client-side. | S |

> ✅ Done: P8-I3-wire (devlog 111), P8-I3 ingest node (devlog 113), P8-DDL-replication (devlog 112), Pod placement (devlog 104), Ingest Phase 1 (devlog 102), Cluster-aware INSERT routing (devlog 103). Live rebalancing, dual-write, partial-move, bandwidth throttling, PTP clock sync all shipped earlier.

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

---

## Summary

| Priority | Category | Open | Next action |
|----------|----------|:----:|-------------|
| **P2** | Visibility & Launch | 1 + 4 manual | Demo video → Show HN → Reddit |
| **P4** | Tool Integration | 2 | ClickHouse wire protocol |
| **P5** | Data Pipelines | 4 | Kafka Connect Sink |
| **P6** | Enterprise / Cloud | 3 | Marketplace |
| **P7** | Engine Performance | 3 | JOINs/Window virtual tables |
| **P8** | Cluster | 10 | Ingest-rate HPA, RDMA transport |
| **P9** | Physical AI / IoT | 17 | ROS2 plugin, OPC-UA browse CLI |
| **P10** | Extensions | 9 | UDF, Edge mode |

**Total open: 49 items + 4 manual tasks**

**Critical path: P2 (launch) → P8-I4 (ingest HPA) → P4 (ClickHouse protocol)**
