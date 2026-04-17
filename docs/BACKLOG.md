# ZeptoDB Backlog

> Completed features: [`COMPLETED.md`](COMPLETED.md) | 1129 tests passing
>
> Last cleaned: 2026-04-16

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
| **Table-scoped partitioning** | 🔴 Critical for Physical AI / IoT | M |
| ~~**Cost-based planner**~~ | ✅ Phase 1-7 done (devlog 066-067, 075) — TableStatistics + CostModel + LogicalPlan + PhysicalPlan + EXPLAIN v2 + Wiring (HASH_JOIN build side), 47 tests | — |
| **JOINs/Window on virtual tables** | 🟠 Moderate | M |
| ~~**SIMD-ify WindowJoin aggregate loop**~~ | ✅ Done (devlog 080) — Contiguous fast-path + sum_i64() SIMD for SUM/AVG, gather+SIMD for large non-contiguous, scalar fallback for small windows — 10 tests | — |
| ~~**JIT SIMD emit**~~ | ✅ Done (devlog 079) — Explicit `<4 x i64>` vector IR generation in LLVM JIT, cttz mask extraction, scalar tail | — |
| ~~**DuckDB embedding**~~ | ✅ Done (devlog 076) — Embedded DuckDB engine, Arrow bridge, Parquet offload, `duckdb()` table function | — |
| **Limited DSL AOT compilation** | — | M |

> ✅ Done: Composite index, MV query rewrite, INTERVAL, Prepared statements, Query result cache, SAMPLE, Scalar subqueries, FlatHashMap joins, DuckDB embedding

### Table-Scoped Partitioning (P7 — Critical)

**Problem**: PartitionKey is `(symbol_id, hour_epoch)` — no table dimension. All tables share the same partition pool. `SELECT * FROM empty_table` returns data from other tables. Physical AI / IoT workloads with dozens of tables (temperature, vibration, lidar, etc.) scan all partitions regardless of table, causing 10–50x unnecessary overhead.

**Solution**: Add `table_id` (uint16_t) to PartitionKey → `(table_id, symbol_id, hour_epoch)`.

**Changes required**:
- `PartitionKey`: add `table_id` field (2 bytes)
- `PartitionManager`: table-scoped partition index (`get_partitions_for_table()`)
- `TickMessage`: add `table_id` field
- `exec_insert()`: pass `table_id` from table name
- `find_partitions()`: filter by `table_id`
- `PartitionRouter` (cluster): include `table_id` in routing key

**Performance impact**:
- Ingest: 0% change (2 bytes added to hash — unmeasurable)
- Query (HFT, 2–3 tables): 0–2x improvement
- Query (IoT, 10+ tables): 2–10x improvement
- Query (Physical AI, 50+ tables): 10–50x improvement
- Memory: +1% per partition metadata (2 bytes on ~200 byte struct)
- Fully backward compatible (table_id=0 for legacy single-table mode)

**Test plan** (multi-table ingest + isolation verification):
- Unit: CREATE 5 tables (temperature, vibration, pressure, gps, lidar), INSERT 1000 rows each with different schemas, verify `SELECT * FROM temperature` returns only temperature data
- Unit: ASOF JOIN between two different tables (trades JOIN quotes ON symbol, timestamp)
- Unit: GROUP BY on one table doesn't include rows from other tables
- Unit: DROP TABLE removes only that table's partitions
- Unit: Empty table returns 0 rows even when other tables have data
- Bench: 50 tables × 10K devices × 100Hz ingest, measure throughput vs single-table baseline
- Cluster: Multi-table INSERT routed to correct nodes, cross-node SELECT returns correct table data
- K8s: 3-node cluster with 10 tables, concurrent ingest + query on different tables

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
