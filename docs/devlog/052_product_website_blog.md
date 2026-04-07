# Devlog 052: Product Website & Blog (P2)

**Date:** 2026-04-07
**Status:** ✅ Complete

## Summary

Built the ZeptoDB product website using Astro Starlight. All pages from the
WEBSITE_PRD are implemented. Blog seeded with 4 technical posts migrated from
devlogs. Security and Integrations pages verified complete.

## What Was Built

### Site (`~/zeptodb-site/`)

| Page | File | Content |
|------|------|---------|
| Landing `/` | `index.mdx` | Hero, benchmark comparison table, use case cards, CTA |
| Features | `features.mdx` | Ingestion, Query, Storage, Client APIs, Security, Cluster |
| Benchmarks | `benchmarks/index.md` | Hardware specs, throughput/latency numbers |
| Use Cases (4) | `use-cases/` | Trading, IoT, Robotics, Autonomous Vehicles |
| Compare (4) | `compare/` | vs kdb+, ClickHouse, InfluxDB, TimescaleDB |
| Pricing | `pricing.mdx` | Community (Free) vs Enterprise |
| Blog (4 posts) | `blog/` | Intro, ASOF JOIN, Zero-Copy Python, Lock-Free Ingestion |
| About | `about.mdx` | Mission, tech philosophy, tech stack |
| Security | `security.mdx` | TLS, Auth, RBAC, Audit, Compliance matrix |
| Integrations | `integrations.mdx` | Feed handlers, clients, monitoring, cloud, roadmap |
| Community | `community.mdx` | GitHub, Discussions, Contributing, Roadmap |
| Contact | `contact.md` | Contact info |

### Blog Posts (devlog-based)

| Post | Source Devlog | Topic |
|------|-------------|-------|
| `introducing-zeptodb.mdx` | 001 + architecture | Project intro, comparison table |
| `how-asof-join-works.mdx` | 008 | Sorted merge algorithm, Window JOIN |
| `zero-copy-python.mdx` | 005 | pybind11, Lazy DSL, Polars benchmarks |
| `lock-free-ingestion.mdx` | 002 + 001 | MPMC ring buffer, Highway SIMD, JIT |

### Infrastructure

- Astro Starlight with custom Header (top nav + GitHub Stars)
- GitHub Actions: `build-deploy.yml` (push to main + repository_dispatch)
- `sync-docs.mjs`: syncs ZeptoDB `docs/` → site content on CI
- Build: 50 pages, 5.55s, Pagefind search index

## Files Changed

```
zeptodb-site/src/content/docs/blog/
  index.mdx                    # Updated: links to real posts
  introducing-zeptodb.mdx      # New
  how-asof-join-works.mdx      # New
  zero-copy-python.mdx         # New
  lock-free-ingestion.mdx      # New

docs/BACKLOG.md                # P2 Website items marked complete
docs/COMPLETED.md              # Website (P2) section added
docs/devlog/052_product_website_blog.md  # This file
```
