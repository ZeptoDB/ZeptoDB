# ZeptoDB Backlog

> Completed features: [`COMPLETED.md`](COMPLETED.md) | 830 tests passing
>
> Last cleaned: 2026-04-07

---

## P1 — "A Product We Can Demo"

> ✅ Fully complete: Dashboard overview (stats, tables, ingestion chart, drop rate), Cluster dashboard (topology, distribution, time-series), Query editor (all QE-1~15), Table schema browser

---

## P2 — "A Product People Can Find"

### Website

| Task | Why | Effort |
|------|-----|--------|
| **Website (zeptodb.io)** | A GitHub README alone lacks credibility. PRD: `docs/business/WEBSITE_PRD.md` | S |
| ↳ Landing page | Benchmark numbers are the key selling point | |
| ↳ Features / Performance / Use Cases | Feature details, benchmark comparisons, industry-specific examples | |
| ↳ Pricing / Blog / About | OSS vs Enterprise, devlog migration, company introduction | |
| Docs site deployment automation | GitHub Actions → Cloudflare Pages / GitHub Pages CI/CD | XS |
| Use Cases page | HFT, Quant, Crypto, IoT industry-specific examples. Keywords for search traffic | S |
| Blog (devlog migration) | Migrate 040 existing devlogs → tech blog. SEO long-tail traffic | S |

### Onboarding

| Task | Why | Effort |
|------|-----|--------|
| **YouTube / Loom demo video** | 2-minute demo video. Embed in README + landing page. 3x conversion vs text | S |

### Package Distribution

| Task | Why | Effort |
|------|-----|--------|
| Multi-arch Docker (amd64 + arm64) | Graviton build already verified. Covers M1 Mac users | S |
| **PyPI package (`pip install zeptodb`)** | The first path Python quants try. Arrow Flight client wrapper | S |
| **Homebrew Formula** | macOS developer accessibility. `brew install zeptodb` | S |
| **GitHub Releases + binaries** | Download without building. Linux amd64/arm64 tarball, `.deb`, `.rpm` | S |

### Manual TODO (requires manual execution)

| Task | Guide Document | Prerequisites | Effort |
|------|---------------|---------------|--------|
| Create Discord server | `docs/community/COMMUNITY_SETUP.md` | — | XS |
| Enable GitHub Discussions | repo Settings → Features → Discussions | — | XS |
| Submit Awesome Time-Series DB PR | `docs/community/REGISTRY_SUBMISSIONS.md` | GitHub repo public | XS |
| Submit DB-Engines registration form | `docs/community/REGISTRY_SUBMISSIONS.md` | zeptodb.io live | XS |
| Record demo GIF | `asciinema rec` → convert to GIF → uncomment in README | Docker image | XS |
| Post Show HN | `docs/community/LAUNCH_POSTS.md` | Docker image + website + Discord | XS |
| Post on Reddit (5 subreddits) | `docs/community/LAUNCH_POSTS.md` | 1-2 day gap after Show HN | XS |
| Uncomment README Discord badge | Top comment block in `README.md` | After Discord server creation | XS |

---

## P3 — High-Performance Connectivity

> ✅ Fully complete (Arrow Flight server). No remaining items.

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

> ✅ Already done: Vault-backed API Key Store, OIDC Discovery, SSO login flow, JWT Refresh, Server-side sessions, IdP group→role mapping

---

## P7 — Performance / Engine

| Task | Engine Impact | Effort |
|------|---------------|--------|
| **Composite index** | 🔴 Major | M |
| **MV query rewrite** | 🔴 Major | M |
| **Cost-based planner** | 🔴 Major | L |
| **JOINs/Window on virtual tables** | 🟠 Moderate | M |
| **SIMD-ify WindowJoin aggregate loop** | 🟠 Moderate | M |
| **JIT SIMD emit** | — | L |
| **DuckDB embedding** | — | M |
| **Limited DSL AOT compilation** | — | M |

> ✅ Already done: Tier A (INTERVAL, Prepared statements, Query result cache, SAMPLE, Scalar subqueries), FlatHashMap joins

---

## P8 — Cluster

> ✅ P8-Critical, P8-High, P8-Medium all complete.

### P8-RDMA — Transport Layer

| Task | Impact | Effort |
|------|--------|--------|
| **WAL replication RDMA PUT** | Remove ingest throughput bottleneck. TCP ~50μs → RDMA ~1-2μs | M |
| **Remote column scan RDMA GET** | Zero DataNode CPU overhead for scatter-gather queries | L |
| **Partition migration RDMA GET** | Zero service impact on source node during live rebalancing | M |
| **Failover re-replication RDMA GET** | Minimize replica replication overhead on node failure | M |
| **`remote_ingest_regions_` wire-up** | Actual connection of RDMA ingest path | S |

### P8-Feature — Distributed Feature Expansion

| Task | Why | Effort |
|------|-----|--------|
| **Live rebalancing** | Zero-downtime partition migration | L |
| **Tier C cold query offload** | Historical data → DuckDB on S3 | M |
| **PTP clock sync detection** | ASOF JOIN strict mode | S |
| **Global symbol registry** | Distributed string symbol routing | M |
| **Multi-node benchmark execution** | EKS guide ready, ~$12/run | S |

---

## P9 — Physical AI / Industry

| Task | Why | Effort |
|------|-----|--------|
| **MQTT ingestion** | IoT devices | S |
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
| HDB Compaction | Parquet merge | S |
| Snowflake/Delta Lake hybrid | — | M |
| Graph index (CSR) | Fund flow tracking | L |
| InfluxDB migration | InfluxQL → SQL | S |

---

## Summary

| Priority | Category | Remaining | Next Action |
|----------|----------|:---------:|-------------|
| **P1** | Demo-ready UI | ✅ 0 | Complete |
| **P2** | Website + Distribution | 11 + 8 manual | Website (zeptodb.io) is the gate |
| **P3** | Connectivity | ✅ 0 | Complete |
| **P4** | Tool Integration | 2 | ClickHouse protocol, JDBC/ODBC |
| **P5** | Data Pipelines | 4 | Kafka Connect, CDC |
| **P6** | Enterprise / Cloud | 3 | SAML, Geo-replication, Marketplace |
| **P7** | Engine Performance | 8 | Composite index, Cost-based planner |
| **P8** | Cluster | 10 | RDMA transport, Live rebalancing |
| **P9** | Physical AI / IoT | 3 | MQTT, OPC-UA, ROS2 |
| **P10** | Extensions | 9 | UDF, Edge mode |

**Total remaining: 50 items + 8 manual tasks**

**Critical path: P2 (Website + Distribution) → P4 (Tool Integration)**
