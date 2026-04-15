# ZeptoDB Product Website — PRD

## 1. Overview

ZeptoDB product marketing website. A separate site from the technical documentation (mkdocs),
with the purpose of conveying product value and acquiring leads.

- **URL**: `zeptodb.com` (product) / `docs.zeptodb.com` (technical docs)
- **Repo**: `~/zeptodb-web/` (separate from main repo)

## 2. Target Audience

| Priority | Audience | Pain Point | Message |
|----------|----------|------------|---------|
| Primary | HFT/Quant Engineers | kdb+ license cost, q language entry barrier | kdb+ performance + standard SQL + open source |
| Secondary | Data Engineers | ClickHouse real-time limitations | Real-time + analytics unified, ASOF JOIN |
| Tertiary | CTO/VP Eng | Vendor lock-in, cost | Open source, self-hosting, enterprise security |

## 3. Page Structure

```
zeptodb.com/
├── /                    # Landing — Hero + core value + benchmark highlights
├── /features            # Feature details (SIMD, JIT, SQL, Python, Security)
├── /performance         # Benchmark comparison table (vs kdb+, ClickHouse)
├── /use-cases           # Industry use cases (HFT, Quant, IoT, Crypto)
├── /pricing             # OSS vs Enterprise vs Cloud (placeholder)
├── /blog                # Technical blog (leveraging existing devlog)
├── /about               # Company info, contact, GitHub
└── /docs → redirect     # docs.zeptodb.com (mkdocs)
```

### 3.1 Landing Page (/)

- Hero: One-line tagline + key metrics (5.52M ticks/sec, 272μs filter)
- "Why ZeptoDB" 3-column (kdb+ performance / standard SQL / open source)
- Benchmark highlight table
- Code snippets (SQL + Python DSL)
- CTA: GitHub Star, Get Started, Contact

### 3.2 Features (/features)

- Storage Engine (Arena, Column Store, HDB, Parquet)
- Ingestion (5.52M/sec, MPMC Ring Buffer, WAL)
- Execution (Highway SIMD, LLVM JIT, Parallel Scan)
- SQL (standard SQL, ASOF JOIN, Window Functions, xbar, EMA)
- Python (zero-copy, Polars-style DSL)
- Security (TLS, RBAC, JWT, Audit Log, SOC2/MiFID II)
- Cluster (Replication, Auto Failover, Coordinator HA)

### 3.3 Performance (/performance)

- Comparison table: ZeptoDB vs kdb+ vs ClickHouse
- Benchmark charts (bar chart)
- Test environment/methodology specified
- Leveraging existing `docs/bench/` data

### 3.4 Use Cases (/use-cases)

- HFT: Tick processing, ASOF JOIN, xbar OHLCV
- Quant Research: Backtesting, EMA, Python Jupyter
- Risk/Compliance: Audit logs, Grafana dashboards
- Crypto/DeFi: 24/7 multi-exchange, Kafka
- IoT/Smart Factory: 10KHz sensors, DELTA

### 3.5 Pricing (/pricing)

- Community (OSS): Free, self-hosting
- Enterprise: Paid support, SLA, dedicated features
- Cloud (Coming Soon): Managed service
- Contact Sales CTA

### 3.6 Blog (/blog)

- Migrate existing devlog (000~023)
- New technical blog posts
- RSS feed

## 4. Technical Requirements

### 4.1 Tech Stack

| Component | Choice | Rationale |
|-----------|--------|-----------|
| Framework | **Astro** | Content-focused SSG, zero JS by default, fast builds |
| UI | **Tailwind CSS** | Utility-based, built-in dark mode |
| Components | **React** (if needed) | Only interactive elements as islands |
| Content | **MDX** | Embed components in blog/content |
| Deploy | **Cloudflare Pages** | Free, global CDN, fast |
| Domain | `zeptodb.com` | Product / `docs.zeptodb.com` docs |

### 4.2 Non-functional

- **Performance**: Lighthouse 95+ (all categories)
- **SEO**: Meta tags, OG images, sitemap.xml, robots.txt
- **i18n**: en (default) / ko
- **Responsive**: Mobile / tablet / desktop
- **Dark mode**: System setting sync + manual toggle
- **Accessibility**: WCAG 2.1 AA

## 5. Execution Plan

### Phase 1: Foundation (Day 1-2)

1. Astro project initialization + Tailwind setup
2. Layout (Header/Nav/Footer) components
3. Dark mode + responsive basics
4. Cloudflare Pages deployment pipeline

### Phase 2: Core Pages (Day 3-5)

5. Landing page
6. Features page
7. Performance page (comparison table + charts)
8. Use Cases page

### Phase 3: Content + Finalization (Day 6-7)

9. Pricing page
10. Blog (devlog migration)
11. i18n (ko) implementation
12. SEO + OG images + sitemap

## 6. Content Sources (Leveraging Existing Assets)

| Source | Usage |
|--------|-------|
| `README.md` | Landing page key metrics, comparison table |
| `docs/bench/` | Performance page data |
| `docs/design/` | Features page architecture |
| `docs/business/` | Use Cases, Pricing reference |
| `docs/devlog/` | Blog content |

## 7. Success Metrics

- GitHub Star growth rate
- Documentation page traffic (Landing → Docs conversion rate)
- Contact/Demo request count
- Lighthouse score 95+
- Page load < 1 second (FCP)
