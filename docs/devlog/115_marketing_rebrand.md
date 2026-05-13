# Devlog 115 — Marketing site rebrand to general-purpose time-series DB

> Status: complete
>
> Date: 2026-05-13
>
> Scope: `web/src/app/(marketing)/**` + `docs/business/WEBSITE_PRD.md` + devlog / BACKLOG / COMPLETED pointers.

---

## Why

Until this devlog, the marketing pages at `web/src/app/(marketing)/` (`/home`, `/features`, `/pricing`) positioned ZeptoDB as an "ultra-low-latency in-memory database tailored for the financial and HFT markets." That no longer matches what the engine ships: OPC-UA, MQTT, Kafka, FIX, ITCH, and Binance connectors; sector profiles for Fab / Auto / Steel / Generic industrial deployments; SOC 2 / EMIR / MiFID II audit log; multi-tenant RBAC; HDB tiered storage to S3. The product is a general-purpose industry time-series DB with HFT-grade performance, serving:

- **Physical AI** — OPC-UA, robotics replay, sensor fusion (devlogs 101 → 110)
- **Finance / HFT** — the origin market, still first-class
- **Game** — player telemetry, anti-cheat, live-ops
- **IoT / Smart Factory** — MQTT + 10 kHz sensors + predictive maintenance
- **Real-Time Observability** — logs / metrics / traces unified under SQL

Source-of-truth positioning lives in `docs/business/product_positioning.md` ("Built for quants. Ready for everything."), `docs/design/physical_ai_market.md`, `docs/business/industry_gap_analysis.md`, and `docs/business/competitive_analysis.md`. The site now pulls directly from those documents instead of inventing new copy.

---

## What changed

### New marketing IA (5 pages)

| Route | Status | Purpose |
|-------|--------|---------|
| `/home` | Rewritten | Multi-industry hero + proof-metrics strip + industry cards + Why ZeptoDB + SQL/Python split-screen + CTAs |
| `/solutions` | **New** | 5 industry sections (pain → capability table → proof → killer line) + "+ more verticals" footer |
| `/features` | Rewritten | 4 capability groups (Ingest / Query / Deploy / Secure) replacing the legacy 3-card layout |
| `/performance` | **New** | Benchmark comparison (ZeptoDB vs kdb+ vs ClickHouse vs InfluxDB) + per-operation detail + methodology footer citing clang-19 / devlogs 097–098 |
| `/pricing` | Rewritten | Neutralized language — no longer finance-specific. "Cloud-hosted tier coming soon" teaser |

Section anchors on `/solutions`: `#physical-ai`, `#finance`, `#game`, `#iot`, `#observability`. The homepage industry cards deep-link into these.

### New marketing layout

New `src/app/(marketing)/layout.tsx` provides a compact cross-page nav bar (Home · Solutions · Features · Performance · Pricing). The console `ClientShell` / `Sidebar` / `TopBar` are untouched — the marketing group's layout nests inside them, so the existing AuthGuard behaviour is preserved.

### Updated tests (Vitest + React Testing Library)

| File | Status | What it verifies |
|------|--------|------------------|
| `src/__tests__/pricing.test.tsx` | Updated | New open-core header, "Book a Demo" CTA, neutralized production-domain copy, cloud-hosted teaser |
| `src/__tests__/home.test.tsx` | **New** | Multi-industry H1, 4-stat proof strip, 4 industry cards + "+ more", Why-ZeptoDB 3-column, primary CTAs |
| `src/__tests__/solutions.test.tsx` | **New** | 5 industry sections with correct anchor ids, all 5 headings, all 5 killer lines, "+ more verticals" footer |
| `src/__tests__/performance.test.tsx` | **New** | 5-column comparison header, 4 engine rows (ZeptoDB / kdb+ / ClickHouse / InfluxDB), reference numbers, clang-19 methodology footer |

### Vitest config cleanup (pre-existing bug fix)

`web/vitest.config.ts` now excludes `e2e/**`. The `e2e/web-ui.spec.ts` Playwright spec was being picked up by Vitest and failing with "Playwright Test did not expect test.describe() to be called here". This predates devlog 115 but was making `pnpm test` red. One-line fix; Playwright runs via `pnpm exec playwright test` as designed.

### Documentation updates (all in this devlog)

- `docs/business/WEBSITE_PRD.md` — IA updated from 7 pages (Landing / Features / Performance / Use Cases / Pricing / Blog / About) to the 5 pages that actually shipped; tech stack switched from Astro + Tailwind + Cloudflare Pages (aspirational) to Next.js 14 App Router + MUI + static export (shipped); target-audience table gained Physical AI and Game rows.
- `docs/BACKLOG.md` — devlog pointer bumped from 114 to 115; noted that the multi-industry messaging foundation is live so the "YouTube / Loom demo video" P2 item is now straightforward (still open).
- `docs/COMPLETED.md` — new "Marketing / Website" section at the bottom with the rebrand entry.
- `.kiro/KIRO.md` — stale devlog pointer bumped (`111 → 115`; next `116`).
- `.kiro/context/orchestrator.md` — same pointer refresh.

---

## Technical notes

- **Client / server components.** Every marketing page uses `"use client"`. Without that pragma, Next.js 16's server-component build rejects passing `component={Link}` through MUI's `<Button>` props ("Functions cannot be passed directly to Client Components…"). With the pragma, the pages still statically export under `output: "export"` — "use client" means hydrated, not dynamic.
- **Stack constraints honoured.** No new deps. Icons from `@mui/icons-material` (already in tree): `SmartToyIcon` (Physical AI), `ShowChartIcon` (Finance), `SportsEsportsIcon` (Game), `FactoryIcon` (IoT), `VisibilityIcon` (Observability), plus `InputIcon` / `QueryStatsIcon` / `CloudQueueIcon` / `LockIcon` for the four Features capability groups. Code snippets are rendered via `<Box component="pre">` with the existing JetBrains Mono font from `app/layout.tsx` — no Prism, no Shiki, no new runtime.
- **Theme-aware.** All colour usage is theme-token based (`color="primary"`, `color="secondary"`, `color="text.secondary"`, `borderColor: "divider"`, `bgcolor: (theme) => `${theme.palette.primary.main}10``). Both dark (default) and light modes render correctly under the existing `ThemeModeContext` toggle.
- **External vs internal links.** `next/link` for internal nav; `component="a"` for Discord, GitHub, GitHub `#-quick-start`, and the `mailto:sales@zeptodb.com?subject=…` Enterprise CTA.

---

## Build & verify

```bash
cd web
pnpm test                      # 12 files, 79 tests, all green
pnpm build                     # 19 static pages incl. /home /solutions /features /performance /pricing
pnpm exec eslint \
  "src/app/(marketing)/**/*.tsx" \
  "src/__tests__/{home,solutions,performance,pricing}.test.tsx"
                               # zero issues in new/modified files
```

Pre-existing lint issues in `admin/`, `cluster/`, `query/`, `tables/`, `tenants/`, `PaginatedTable.tsx`, `Providers.tsx`, and `theme.ts` are unchanged by this devlog and are out of scope.

---

## Files touched

**Web (marketing rebrand):**
- `web/src/app/(marketing)/layout.tsx` (new)
- `web/src/app/(marketing)/home/page.tsx` (rewritten)
- `web/src/app/(marketing)/solutions/page.tsx` (new)
- `web/src/app/(marketing)/features/page.tsx` (rewritten)
- `web/src/app/(marketing)/performance/page.tsx` (new)
- `web/src/app/(marketing)/pricing/page.tsx` (rewritten)

**Web (tests):**
- `web/src/__tests__/home.test.tsx` (new)
- `web/src/__tests__/solutions.test.tsx` (new)
- `web/src/__tests__/performance.test.tsx` (new)
- `web/src/__tests__/pricing.test.tsx` (updated)
- `web/vitest.config.ts` (exclude `e2e/**`)

**Docs:**
- `docs/business/WEBSITE_PRD.md`
- `docs/BACKLOG.md`
- `docs/COMPLETED.md`
- `docs/devlog/115_marketing_rebrand.md` (this file)
- `.kiro/KIRO.md`
- `.kiro/context/orchestrator.md`

**Explicitly not touched** (per task scope):
- Console routes: `/dashboard`, `/query`, `/tables`, `/cluster`, `/tenants`, `/admin`, `/settings`, `/login`
- Console components: `Sidebar.tsx`, `TopBar.tsx`, `ClientShell.tsx`, `Providers.tsx`, `AuthGuard.tsx`, `SchemaSidebar.tsx`, `ResultChart.tsx`, `PaginatedTable.tsx`, `ExplainView.tsx`, `ExecSparkline.tsx`, `ResizeDivider.tsx`, `UpgradeCard.tsx`
- `lib/api.ts`, `lib/auth.tsx`, `lib/useLicense.ts`
- Backend C++ code
- mkdocs technical docs (`site/`, `mkdocs.yml`)

---

## What this unblocks

- **Demo video (P2 backlog)** — the new `/solutions` page is a script-ready vertical tour: one minute per industry, one real feature set per industry, one killer line per industry. Recording a 5-minute Loom is now mechanical.
- **Physical AI outreach** — `/solutions#physical-ai` is linkable from OPC-UA partnership conversations; Samsung / SK / TSMC / POSCO-class audiences land on copy that names their sector profile directly (devlog 105).
- **Benchmark credibility** — `/performance` cites clang-19, devlogs 097–098, and `docs/bench/results_multinode.md`. Any prospect asking "how do I reproduce this?" has a paper trail.
- **Pricing conversations** — `/pricing` no longer implies ZeptoDB is finance-only, so the Enterprise CTA now makes sense for factory-floor and game-backend prospects.

## Follow-up (out of scope for 115)

- Record the P2 demo video now that messaging is stable.
- Consider a dedicated marketing-only layout tree (separate from `ClientShell` + AuthGuard) so unauthenticated traffic can land on `/home` / `/solutions` / `/performance` / `/pricing`. Today the marketing pages still nest under `AuthGuard`, which is fine for internal review but would need changing before `zeptodb.com` deployment. That's a larger architectural change — deferred.
- Migrate `docs/business/WEBSITE_PRD.md` §6 (Astro blog migration) into a realistic "docs.zeptodb.com vs zeptodb.com" split plan.

---

## Post-review fixup

Follow-up pass after code review flagged one BLOCKING issue and four WARNINGs against the initial 115 landing. All fixes kept inside this same feature scope — no new devlog number since this is corrective work on the rebrand, not a new feature.

### BLOCKING — wrong ITCH parser latency (fabricated number)

Two marketing pages cited the ITCH parser at 350 ns. That number is FIX. ITCH = 250 ns. Four source docs agree (`docs/feeds/FEED_HANDLER_COMPLETE.md`, `docs/design/high_level_architecture.md`, `docs/feeds/PERFORMANCE_OPTIMIZATION.md`, `docs/business/BUSINESS_STRATEGY.md`).

Fixed in three places (per KIRO.md "verify both and unify to the correct version"):

- `web/src/app/(marketing)/features/page.tsx` — Ingest bullet now reads "NASDAQ ITCH (250 ns parser), FIX (350 ns parser)".
- `web/src/app/(marketing)/solutions/page.tsx` — Finance / HFT capability row now reads "ITCH parser 250 ns, FIX parser 350 ns".
- `docs/COMPLETED.md` — Feed Handlers line now reads "NASDAQ ITCH (250ns parsing), FIX (350ns parsing)" (pre-existing typo; fixed in the same pass rather than deferred).

### WARNING 1 — phantom test file referenced in WEBSITE_PRD

`docs/business/WEBSITE_PRD.md §8` listed `web/src/__tests__/features.test.tsx` but that file was never created. Added it — 4 tests mirroring the pattern of `home.test.tsx` / `solutions.test.tsx` / `performance.test.tsx` / `pricing.test.tsx`: H1 "Platform Features" + tagline, the four capability-group H2s (Ingest / Query / Deploy / Secure), the corrected ITCH 250 ns / FIX 350 ns copy, and one representative bullet per group. No PRD change — the doc is now accurate once the file exists.

### WARNING 2 — stale devlog pointer in reviewer context

`.kiro/context/reviewer.md` line 33 still said "(sequential numbering, next: 083)". Bumped to 116 to match the `KIRO.md` / `orchestrator.md` pointers the previous pass updated.

### WARNING 3 — heading-level skips (accessibility)

Every marketing page jumped from `<h1>` directly to `<h5>` or `<h6>`, skipping h2–h4. MUI `variant="h*"` controls visual size only; semantic level needs `component="h*"`. Visual design is unchanged; only the DOM level is corrected.

- `pricing/page.tsx` — subtitle "Open core, enterprise-ready…" gets `component="h2"`; both tier titles ("Open Source", "Enterprise") get `component="h3"`.
- `performance/page.tsx` — the comparison-table card gains a new `<Typography variant="h4" component="h2">Benchmark comparison</Typography>`; "ZeptoDB in detail" is now `variant="h4" component="h2"`; "Test environment" footer is now `variant="h6" component="h2"`.
- `home/page.tsx` — industry card titles and Why-ZeptoDB card titles now render `component="h3"` under the existing `<h2>` section headings.
- `solutions/page.tsx` — "+ more verticals" footer gets `component="h2"` to align with its sibling industry sections.

`solutions.test.tsx` and `performance.test.tsx` updated to assert on the corrected heading levels (level 5 → 2 for "+ more verticals", level 6 → 2 for "Test environment").

### WARNING 4 — decorative icons lacked `aria-hidden`

MUI icons sitting next to descriptive text are decorative. Added `aria-hidden` to:

- `home/page.tsx` — hero `StorageIcon`, the 5 `INDUSTRIES` entry icons, and the 3 `WHY` entry icons.
- `solutions/page.tsx` — the 5 industry section icons.
- `features/page.tsx` — the 4 capability-group icons (Input, QueryStats, CloudQueue, Lock).
- `performance/page.tsx` — none present (no change).
- `pricing/page.tsx` — none present (no change).

### Bonus — unique button labels on home

Five `Learn more →` industry buttons on the home page all had identical text, so screen-reader users heard "Learn more" five times with no context. Added `aria-label={`Learn more about ${ind.title}`}` to each; visible text unchanged.

### Verify

```bash
cd web
pnpm test       # 13 files, 83 tests (79 + 4 new features.test.tsx) — all green
pnpm build      # 19 static pages — success
pnpm exec eslint "src/app/(marketing)/**/*.tsx" \
  "src/__tests__/{home,solutions,features,performance,pricing}.test.tsx"
                # zero issues in marketing diffs
grep -RHn "350 ns parser" 'web/src/app/(marketing)/'  # only FIX references
grep -RHn "250 ns parser" 'web/src/app/(marketing)/'  # only ITCH references
```

### Files touched (fixup only)

- `web/src/app/(marketing)/features/page.tsx`
- `web/src/app/(marketing)/solutions/page.tsx`
- `web/src/app/(marketing)/home/page.tsx`
- `web/src/app/(marketing)/performance/page.tsx`
- `web/src/app/(marketing)/pricing/page.tsx`
- `web/src/__tests__/features.test.tsx` (new)
- `web/src/__tests__/solutions.test.tsx` (heading-level assertion)
- `web/src/__tests__/performance.test.tsx` (heading-level assertion)
- `docs/COMPLETED.md` (ITCH/FIX latency correction)
- `.kiro/context/reviewer.md` (devlog pointer bump)
- `docs/devlog/115_marketing_rebrand.md` (this section)

Out of scope per reviewer guidance: AuthGuard refactor for marketing pages (deferred in §Follow-up above), rewriting the InfluxDB row on `/performance`, and any C++ / console-route / shared-component changes.
