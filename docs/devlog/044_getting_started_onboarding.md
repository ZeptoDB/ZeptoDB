# Devlog 040 — Getting Started & Onboarding Docs

**Date:** 2026-04-01

## What

Created the first 3 Getting Started & Onboarding documents from the P2 backlog:

1. **Quick Start Guide** (`docs/getting-started/QUICK_START.md`)
   - Docker one-liner → INSERT → SELECT → Python in 5 minutes
   - Covers HTTP (curl), Arrow Flight, Python in-process, and Web UI
   - Links to all reference docs as next steps

2. **Interactive Playground** (`docs/getting-started/INTERACTIVE_PLAYGROUND.md`)
   - Browser-based SQL editor with sandboxed server backend
   - Sandbox constraints: 5s timeout, 30 req/min, 10K row limit
   - 3 preloaded datasets (trades, quotes, sensors)
   - 5 curated example queries covering VWAP, OHLCV, ASOF JOIN, EMA, IoT
   - Self-hosted deployment via `--playground` flag or YAML config

3. **Example Dataset Bundle** (`docs/getting-started/EXAMPLE_DATASETS.md`)
   - `--demo` flag design: 350K rows across 3 tables (~14.4 MB)
   - trades (100K, 5 equity symbols), quotes (200K), sensors (50K, 10 devices)
   - Deterministic generation (seed=42) for reproducible demos
   - Starter queries printed to stdout on startup

## Why

Empty database = user churn. These 3 docs define the "discover → try → use" onboarding funnel:
- Quick Start: minimum friction to first query
- Playground: zero-install browser experience
- Demo data: no blank canvas problem

## Files Changed

- `docs/getting-started/QUICK_START.md` — new
- `docs/getting-started/INTERACTIVE_PLAYGROUND.md` — new
- `docs/getting-started/EXAMPLE_DATASETS.md` — new
- `docs/BACKLOG.md` — marked 3 items as ✅
- `docs/devlog/040_getting_started_onboarding.md` — this file

## Implementation Status

These are **design documents**. Implementation requires:
- `--demo` flag + data generator in `src/demo/`
- `--playground` proxy middleware
- Actual Docker Hub image push
