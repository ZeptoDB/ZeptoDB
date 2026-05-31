# ZeptoDB Product Website — PRD

> Last updated: 2026-05-13 (devlog 115 — marketing rebrand to general-purpose time-series DB)

## 1. Overview

ZeptoDB product marketing website. A separate surface from the technical documentation (mkdocs at `docs.zeptodb.com`), positioning ZeptoDB as a **general-purpose industry time-series in-memory DB** with HFT-class performance.

- **Public URL**: `zeptodb.com` (product) / `docs.zeptodb.com` (technical docs)
- **Current implementation**: lives inside the main repo under `web/src/app/(marketing)/` as a set of Next.js 14 App Router pages. The standalone `~/zeptodb-web/` Astro project originally proposed in the PRD was not built; everything is now in-repo and ships with the same build pipeline as the console.

## 2. Target Audience

Positioning aligned with `docs/business/product_positioning.md` ("Built for quants. Ready for everything.") and `docs/design/physical_ai_market.md`.

| Priority | Audience | Pain Point | Message |
|----------|----------|------------|---------|
| Primary | HFT / Quant engineers | kdb+ license cost, q-language learning curve | kdb+-class performance + standard SQL + open source |
| Primary | Physical AI / Factory platform teams | InfluxDB / ROS bag can't handle 10 kHz sensors; sensor fusion is ad hoc | OPC-UA + MQTT connectors, ASOF JOIN, sector profiles (Fab / Auto / Steel / Generic) |
| Secondary | Data engineers | ClickHouse real-time limitations, InfluxQL lock-in | Real-time + analytics unified, ASOF JOIN, ClickHouse-compatible HTTP API |
| Secondary | Game backend teams | Firehose player telemetry, anti-cheat latency, live-ops A/B gap | Kafka consumer, window functions, xbar bucketing |
| Tertiary | CTO / VP Eng | Vendor lock-in, multi-workload sprawl, enterprise procurement | Open source, self-hosting, SOC 2 / EMIR / MiFID II audit log |

## 3. Page Structure (shipped)

Current Information Architecture (devlog 115):

```
zeptodb.com/
├── /home          # Hero + proof metrics + industry cards + Why ZeptoDB + SQL/Python split-screen + CTAs
├── /solutions     # Industry deep-dives — Physical AI, Finance, Game, IoT, Observability + "+ more verticals"
├── /features      # Four capability groups — Ingest, Query, Deploy, Secure
├── /performance   # Benchmark comparison table (ZeptoDB vs kdb+ vs ClickHouse vs InfluxDB) + per-op detail + methodology
├── /pricing       # Open Source (free) + Enterprise (contact) + "cloud-hosted tier coming soon"
└── /docs → redirect  # docs.zeptodb.com (mkdocs technical docs)
```

Deferred pages (not in the shipped 5):
- `/blog` — devlog migration (backlogged; devlogs live at `docs/devlog/` for now)
- `/about` — company / contact / GitHub landing (GitHub + Discord covered from hero CTAs in the meantime)

### 3.1 `/home`

- **H1**: "The Time-Series Database for Physical AI, IoT, and Real-Time Analytics"
- **Sub**: "From factory-floor sensors to autonomous fleets to live game servers to trading desks — one database for every high-velocity time-series workload. Microsecond latency, standard SQL, open source."
- **Proof metrics strip** (4 stats): `5.52M` ticks/sec · `272µs` 1M-row filter · `6 native feeds` (OPC-UA · MQTT · Kafka · FIX · ITCH · Binance) · `kdb+-class` performance without the license
- **Industry cards** (4 featured + 1 "more"): Physical AI (→ `/solutions#physical-ai`), Finance / HFT (→ `/solutions#finance`), Game (→ `/solutions#game`), IoT / Smart Factory (→ `/solutions#iot`), + more (→ `/solutions`)
- **Why ZeptoDB** 3-column: µs latency · Research → Production → Compliance · Open source, standard SQL, Python zero-copy
- **Code snippet split**: SQL (ASOF JOIN + xbar OHLCV) on the left, Python (from_polars + zero-copy) on the right
- **Primary CTAs**: Get Started (GitHub Quick Start), View Solutions, Join Discord, GitHub Star

### 3.2 `/solutions`

Five industry sections, each with an anchor id (`physical-ai`, `finance`, `game`, `iot`, `observability`) and the same shape:

1. Pain paragraph
2. Capability table (need → ZeptoDB feature)
3. Proof-point paragraph
4. Killer-line chip

Content pulled directly from `docs/business/product_positioning.md`, `docs/design/physical_ai_market.md`, and `docs/business/industry_gap_analysis.md` — no reinvented copy. "+ more verticals" footer covers Crypto / DeFi, Autonomous Vehicles / Robotics (ROS 2 connector in-tree; live scalar smoke verification on P9 backlog), and Logistics.

### 3.3 `/features`

Four capability groups (replacing the legacy generic 3-card layout):

1. **Ingest** — Kafka, MQTT, OPC-UA, FIX, NASDAQ ITCH, Binance; 5.52M ticks/sec; WAL + quorum replication; backpressure retry
2. **Query** — Standard SQL over ClickHouse-compatible HTTP API (port 8123); ASOF / Hash / Window / UNION JOINs; EMA / DELTA / LAG / LEAD / RANK window functions; xbar / OHLCV / VWAP; JIT + Highway SIMD; cost-based planner
3. **Deploy** — Helm chart, rolling upgrade, PDB; multi-node cluster with auto failover; HDB tiered storage (Hot → Warm → Cold → S3); Prometheus `/metrics`; zero-downtime rebalancing
4. **Secure** — TLS / HTTPS, API Key + JWT / OIDC, RBAC 5 roles, Audit Log (SOC 2 / EMIR / MiFID II), Vault-backed keys, rate limiting, multi-tenancy with per-tenant namespaces

### 3.4 `/performance`

Benchmark comparison table — 4 engine rows (ZeptoDB, kdb+, ClickHouse, InfluxDB) × 5 columns (Ingestion, 1M-row filter p50, ASOF JOIN, License cost, Deployment). Numbers pulled from `docs/bench/` and `docs/business/competitive_analysis.md`.

Plus a per-operation detail card for ZeptoDB (Filter / VWAP / GROUP BY / EMA / xbar / SQL parse / Python column access / Indexed lookup / HDB flush / Partition routing).

Methodology footer explicitly cites:
- clang-19 builds (cross-arch compiler unification per devlogs 097 / 098)
- `docs/bench/results_multinode.md` for the Round 1–3 horizontal-scaling story
- `docs/business/competitive_analysis.md` for the full comparison matrix

### 3.5 `/pricing`

Two tiers, **neutralized** from the old finance-specific copy:

- **Open Source (Free)** — "Full engine, standard SQL, Python DSL, Kafka / MQTT / OPC-UA connectors. Self-hosted. Apache-2.0-compatible license." CTA: Get Started → GitHub Quick Start
- **Enterprise (Contact Us)** — "Advanced RBAC + multi-tenancy, SOC 2 / MiFID II audit log, cluster HA, priority support, licensed connectors. For production deployments across finance, factory floors, game backends, and Physical AI platforms." CTA: Book a Demo → `mailto:sales@zeptodb.com`

Small footer note: "Cloud-hosted tier coming soon."

## 4. Technical Requirements

### 4.1 Tech Stack (shipped)

| Component | Shipped | Original PRD proposal |
|-----------|---------|-----------------------|
| Framework | **Next.js 14 / 16 App Router** (in-repo under `web/`) | Astro (separate repo) |
| UI | **Material UI (MUI) v7** with Emotion | Tailwind CSS |
| Icons | `@mui/icons-material` | Heroicons / Lucide |
| Theme | Shared console theme (`web/src/theme/theme.ts`) with dark/light toggle | Tailwind dark mode |
| Content | TSX pages under `web/src/app/(marketing)/**` | MDX |
| Tests | Vitest + React Testing Library + jsdom | — |
| Output | Static export (`next.config.ts` `output: "export"`) | Astro SSG |
| Deploy | Same pipeline as the console (`/ui` basePath) | Cloudflare Pages |

Rationale for the shift from Astro: keeping the marketing site in-repo and in-stack with the console avoids a second build pipeline, a second dependency tree, and a second CI surface. It also means every marketing page reuses the console's dark/light theme, fonts (Inter + JetBrains Mono), and MUI components — consistent visual identity for free.

### 4.2 Non-functional

- **Performance**: static export, no runtime server, MUI components hydrated client-side.
- **SEO**: per-page `metadata` exports (TODO — currently inherits root metadata; backlog item).
- **i18n**: English-only at time of writing (KIRO.md rule); `ko` deferred.
- **Responsive**: MUI `Box sx={{ gridTemplateColumns: { xs: "1fr", md: "repeat(3, 1fr)" } }}` style breakpoints throughout.
- **Dark mode**: inherited from console `ThemeProvider` via shared `(marketing)` layout nesting under `ClientShell`.
- **Accessibility**: all interactive controls are MUI `Button` with semantic `component={Link}` or `component="a"`; headings use semantic `h1`/`h2`/`h4`/`h5`/`h6`; tables use `<Table>` with `TableHead` / `TableBody`.

### 4.3 Navigation

Marketing cross-nav is a thin top strip rendered by `web/src/app/(marketing)/layout.tsx` (Home · Solutions · Features · Performance · Pricing). The console `ClientShell`, `Sidebar`, and `TopBar` are untouched; marketing pages inherit them today — an intentional current-state tradeoff (marketing pages still sit behind `AuthGuard` at the moment; removing that gate for public `zeptodb.com` hosting is a follow-up).

## 5. Execution Status

Phase 1 (foundation) — **done** via in-repo Next.js.
Phase 2 (core pages) — **done** as of devlog 115: `/home`, `/solutions`, `/features`, `/performance`, `/pricing`.
Phase 3 (blog + i18n) — **deferred**. Demo video (P2 backlog) comes first because the new `/solutions` page is already a script-ready vertical tour.

## 6. Content Sources

| Source | Consumed by |
|--------|-------------|
| `README.md` | `/home` key metrics, `/performance` per-op detail |
| `docs/bench/` | `/performance` comparison table (esp. `results_multinode.md`) |
| `docs/business/product_positioning.md` | `/home` + `/solutions` industry tables |
| `docs/business/competitive_analysis.md` | `/performance` vs kdb+ / ClickHouse / InfluxDB rows |
| `docs/business/industry_gap_analysis.md` | `/solutions` capability tables |
| `docs/design/physical_ai_market.md` | `/solutions#physical-ai` + `/solutions#iot` sector profiles |
| `docs/COMPLETED.md` | Proof-point language ("SLA-grade per devlog 110", "sector profiles per devlog 105") |

## 7. Success Metrics

- GitHub Star growth rate (vanity — but public and easy)
- Page-level clicks to `/solutions#physical-ai`, `#iot`, `#game` (proves the multi-industry pivot lands)
- Demo / sales enquiries via the Enterprise `mailto:` CTA
- Conversion from `/home` → `docs.zeptodb.com` Quick Start
- Lighthouse score 90+ (targeted, not yet measured on in-repo Next.js export)

## 8. Related

- Devlog: `docs/devlog/115_marketing_rebrand.md`
- Source: `web/src/app/(marketing)/**`
- Tests: `web/src/__tests__/{home,solutions,features,performance,pricing}.test.tsx`
