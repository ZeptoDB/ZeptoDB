# ZeptoDB Backlog

> Completed features: [`COMPLETED.md`](COMPLETED.md) | 830 tests passing
>
> Recent: ✅ Arrow Flight server | ✅ String column (dictionary-encoded) | ✅ Cluster stability (P8) complete

---

## P1 — "A Product We Can Demo"

| Task | Why | Effort |
|------|-----|--------|
| **Web UI / Admin Console** | curl demos have zero impact. A visual UI is essential for investor/customer meetings | M |
| ↳ Query editor + result table | SQL input → table/chart output | |
| ↳ Cluster status dashboard | Node count, partitions, ingestion rate | |
| ↳ Table schema browser | View CREATE TABLE results + `/tables/[name]` detail page | ✅ |

Without a Web UI, demos are impossible. `web/` folder, React+Vite, static serve on `localhost:8123`.

### Query Editor Enhancements (`web/src/app/query/page.tsx`)

> ✅ Fully complete: SQL autocomplete, keyboard shortcuts, run selected text, query history (search & pin), export CSV/JSON, resizable editor, schema sidebar, ZeptoDB function autocomplete, result chart view, multi-tab editor, multi-statement run, dark/light theme, column sorting, column filtering, saved queries, syntax error marker, execution cancel, exec time sparkline, EXPLAIN visualization

---

## P2 — "A Product People Can Find"

### Website & Docs

| Task | Why | Effort |
|------|-----|--------|
| **Website (zeptodb.io)** | A GitHub README alone lacks credibility. PRD: `docs/business/WEBSITE_PRD.md` | S |
| ↳ Landing page | Benchmark numbers are the key selling point | |
| ↳ Features / Performance / Use Cases | Feature details, benchmark comparisons, industry-specific examples | |
| ↳ Pricing / Blog / About | OSS vs Enterprise, devlog migration, company introduction | |
| ~~↳ Docs site (docs.zeptodb.io)~~ | ~~mkdocs already exists, just needs deployment~~ | ✅ |
| ↳ Docs site deployment automation | GitHub Actions → Cloudflare Pages / GitHub Pages CI/CD | XS |
| ~~↳ Docs nav update~~ | ~~Add 40+ missing pages (devlog 024-040, Flight API, multinode_stability, etc.)~~ | ✅ |
| ~~↳ Performance comparison page~~ | ~~vs kdb+/ClickHouse/TimescaleDB benchmark charts. Data already in `docs/bench/`~~ | ✅ |
| ↳ Use Cases page | HFT, Quant, Crypto, IoT industry-specific examples. Keywords for search traffic | S |
| ↳ Blog (devlog migration) | Migrate 040 existing devlogs → tech blog. SEO long-tail traffic | S |

### Getting Started & Onboarding

| Task | Why | Effort |
|------|-----|--------|
| ~~**Quick Start guide**~~ | ~~First query within 5 minutes. `docker run` → INSERT → SELECT → Python. Reduce bounce rate~~ | ✅ |
| ~~**Interactive Playground**~~ | ~~Run SQL in the browser (WASM or server sandbox). Try without installing → maximize conversion~~ | ✅ |
| ~~**Example dataset bundle**~~ | ~~Built-in sample stock/sensor data (`--demo` flag). Starting with an empty DB leaves users unsure what to do~~ | ✅ |
| **YouTube / Loom demo video** | 2-minute demo video. Embed in README + landing page. 3x conversion vs text | S |

### Package Distribution

| Task | Why | Effort |
|------|-----|--------|
| **Docker Hub official image** | Start with a single line: `docker pull zeptodb/zeptodb`. Currently only a Dockerfile exists, not registered in any registry | S |
| ↳ Multi-arch (amd64 + arm64) | Graviton build already verified. Covers M1 Mac users | |
| **PyPI package (`pip install zeptodb`)** | The first path Python quants try. Arrow Flight client wrapper | S |
| **Homebrew Formula** | macOS developer accessibility. `brew install zeptodb` | S |
| **GitHub Releases + binaries** | Download without building. Linux amd64/arm64 tarball, `.deb`, `.rpm` | S |

### SEO & Community

| Task | Why | Effort |
|------|-----|--------|
| ~~**SEO basics (sitemap, OG, meta)**~~ | ~~Essential for search engine indexing. mkdocs-material auto-generates these~~ | ✅ |
| ~~**GitHub README renewal**~~ | ~~Badges, architecture diagram, enhanced Quick Start section. GIF demo placeholder~~ | ✅ |
| ~~**Community infrastructure (CONTRIBUTING, CoC, Issue templates)**~~ | ~~Standard open-source file set. Includes FUNDING.yml~~ | ✅ |
| ~~**Registry submission content preparation**~~ | ~~Awesome list PR text, DB-Engines form data completed~~ | ✅ |
| ~~**Launch post drafts**~~ | ~~Show HN + Reddit 5 subreddit drafts, timing strategy~~ | ✅ |

#### Manual TODO (requires manual execution)

| Task | Guide Document | Prerequisites | Effort |
|------|-----------|----------|--------|
| **Create Discord server** | `docs/community/COMMUNITY_SETUP.md` | — | XS |
| **Enable GitHub Discussions** | repo Settings → Features → Discussions | — | XS |
| **Submit Awesome Time-Series DB PR** | `docs/community/REGISTRY_SUBMISSIONS.md` | GitHub repo public | XS |
| **Submit DB-Engines registration form** | `docs/community/REGISTRY_SUBMISSIONS.md` | zeptodb.io live | XS |
| **Record demo GIF** | `asciinema rec` → convert to GIF → uncomment in README | Docker image | XS |
| **Post Show HN** | `docs/community/LAUNCH_POSTS.md` | Docker image + website + Discord | XS |
| **Post on Reddit (5 subreddits)** | `docs/community/LAUNCH_POSTS.md` | 1-2 day gap after Show HN | XS |
| **Uncomment README Discord badge** | Top comment block in `README.md` | After Discord server creation | XS |

---

## P3 — High-Performance Connectivity

> ✅ Fully complete

| Task | Why | Status |
|------|-----|--------|
| ~~**Arrow Flight server**~~ | ~~gRPC-based Arrow batch streaming. Python `pyarrow.flight` zero-copy~~ | ✅ |

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
| **Vault-backed API Key Store** | Production secret management | ✅ Done |
| **Geo-replication** | Multi-region, global trading desks | L |

### SSO / Identity Enhancements

> Current state: JWT verification (HS256/RS256), JWKS auto-fetch, CLI flags, runtime reload complete.

| Task | Why | Effort |
|------|-----|--------|
| ~~**IdP group → role mapping**~~ | ~~For environments where adding a `zepto_role` custom claim to the IdP is not possible~~ | ✅ |
| ~~**OIDC Discovery**~~ | ~~Auto-detect JWKS URL, issuer, audience with a single `--oidc-issuer` flag~~ | ✅ |
| ~~**Web UI SSO login flow**~~ | ~~OAuth2 Authorization Code Flow~~ | ✅ |
| ~~**JWT Refresh Token**~~ | ~~Auto-renew on token expiration~~ | ✅ |
| ~~**Server-side sessions**~~ | ~~Issue session cookie after JWT login~~ | ✅ |
| **SAML 2.0 support** | For SAML-only environments such as banks/insurance | L |

---

## P7 — Performance / Engine

> ✅ Tier A complete: INTERVAL syntax, Prepared statements, Query result cache, SAMPLE clause, Scalar subqueries

| Task | Engine Impact | Effort |
|------|---------------|--------|
| **Composite index** | 🔴 Major | M |
| **MV query rewrite** | 🔴 Major | M |
| **Cost-based planner** | 🔴 Major | L |
| **JOINs/Window on virtual tables** | 🟠 Moderate | M |
| **~~Replace `std::unordered_map` in join operators with flat hash map~~** | ~~🔴 Major~~ | ~~S~~ | ✅ |
| **SIMD-ify WindowJoin aggregate loop** | 🟠 Moderate | M |
| **JIT SIMD emit** | — | L |
| **DuckDB embedding** | — | M |
| **Limited DSL AOT compilation** | — | M |

---

## P8 — Cluster

> ✅ P8-Critical, P8-High, P8-Medium all complete.

### P8-RDMA — Transport Layer Real Connection

| Task | Impact | Effort |
|------|------|--------|
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
| Snowflake/Delta Lake hybrid | | M |
| Graph index (CSR) | Fund flow tracking | L |
| InfluxDB migration | InfluxQL → SQL | S |

---

**Critical path: P1 (Web UI polish) → P2 (Website + Distribution) → P4 (Tool Integration)**

Docs site build complete (`~/zeptodb-site`). Remaining P2: product website (Astro) + deployment automation + package distribution.
