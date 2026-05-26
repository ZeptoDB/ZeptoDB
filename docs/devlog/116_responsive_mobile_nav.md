# Devlog 116 — Responsive Mobile Navigation

**Date**: 2026-05-13
**Scope**: `web/` (UI only, no engine changes)

## Problem

The web UI was unusable on a 375 px viewport (iPhone SE):

1. **Marketing nav** (`/home`, `/solutions`, `/features`, `/performance`, `/pricing`)
   used a single horizontal `Stack` of `Button`s. With five items and `flexWrap: "wrap"`,
   the row clipped or wrapped messily on narrow screens and competed for vertical
   space with hero content.

2. **Console layout** rendered a `variant="permanent"` MUI `Drawer` 220 px wide
   alongside an `AppBar` whose `width` was `calc(100% - 220px)` and whose `ml` was
   220 px. On a 375 px viewport this left only 155 px of content width, the
   sidebar covered most of the screen, and the `AppBar` was offset off-screen on
   the left, producing horizontal overflow and an unscrollable layout.

Both surfaces were touched in devlog 052 (marketing) and devlog 045/051 (console)
but neither was sized for phones.

## Solution

Use the MUI `md` breakpoint (`theme.breakpoints.down("md")` ≈ < 900 px) as the
single switch between desktop and mobile layouts. Desktop appearance is
**unchanged** — every desktop selector resolves to the same CSS as before.

### #1 — Marketing hamburger drawer

`web/src/app/(marketing)/layout.tsx`:

* Above `md`: keep the existing horizontal `Stack` of text-button links
  (`Home`, `Solutions`, `Features`, `Performance`, `Pricing`).
* Below `md`: render a top row with the small ZeptoDB wordmark on the left
  (linking to `/home`) and a `MenuIcon` `IconButton` on the right. Tapping
  the icon opens an MUI `Drawer` (`anchor="right"`, width 240 px) holding
  the same links as a vertical `List`.
* The drawer closes on link click (`onClick={() => setDrawerOpen(false)}`)
  and on backdrop tap / ESC (MUI default `onClose`).
* Switch is driven by `useMediaQuery(theme.breakpoints.down("md"))`.

### #5 — Console Sidebar / TopBar / ClientShell

`web/src/components/Sidebar.tsx`:

* New optional props: `variant?: "permanent" | "temporary"`, `open?: boolean`,
  `onClose?: () => void`. Default `variant="permanent"` preserves the
  existing call sites and desktop appearance.
* When rendered as `temporary`, every nav action (`router.push`, logout,
  external Discord link) calls `onClose()` first so the drawer closes
  before navigation.
* `SIDEBAR_WIDTH = 220` export is unchanged.

`web/src/components/TopBar.tsx`:

* New optional `onMobileMenuToggle` prop.
* `AppBar` width / margin is now responsive:
  * `xs`: `width: 100%`, `ml: 0`
  * `md`: `width: calc(100% - SIDEBAR_WIDTH)`, `ml: SIDEBAR_WIDTH` (unchanged)
* On mobile, prepends a `MenuIcon` `IconButton` that fires
  `onMobileMenuToggle`.
* Role `Chip` is hidden on `xs` (`display: { xs: "none", sm: "flex" }`)
  so title + connection pill + theme-toggle still fit in 375 px.
* Toolbar horizontal padding tightens from `px: 3` to `px: 1.5` on `xs`.

`web/src/components/ClientShell.tsx`:

* New inner `ConsoleShell` component owns `mobileOpen` state and the
  `useMediaQuery(theme.breakpoints.down("md"))` switch.
* On mobile renders `<Sidebar variant="temporary" open={mobileOpen}
  onClose={...} />`; on desktop renders `<Sidebar />` unchanged.
* `TopBar` receives `onMobileMenuToggle={() => setMobileOpen(o => !o)}`.
* Main `<Box component="main">`:
  * `ml: { xs: 0, md: ${SIDEBAR_WIDTH}px }`
  * `width: { xs: "100%", md: calc(100% - ${SIDEBAR_WIDTH}px) }`
  * `p: { xs: 2, md: 4 }`

### Constraints honoured

* No change to desktop appearance (every `md` branch resolves to the
  pre-change CSS).
* `SIDEBAR_WIDTH` stays at 220.
* TypeScript strict, no `any`.
* No new dependencies. Uses MUI primitives already in use elsewhere
  (`useMediaQuery`, `useTheme`, `Drawer`, `IconButton`, `MenuIcon`).
* `getVisibleNav()` exported function untouched — `sidebar.test.ts` still
  passes verbatim.

## Files changed

```
web/src/app/(marketing)/layout.tsx          (rewritten)
web/src/components/Sidebar.tsx              (added optional variant/open/onClose props)
web/src/components/TopBar.tsx               (responsive width/ml + mobile hamburger)
web/src/components/ClientShell.tsx          (added ConsoleShell with mobile state)
web/e2e/web-ui.spec.ts                      (appended Mobile responsive describe block)
docs/design/web_ui_prd.md                   (added "Responsive layout" section)
docs/devlog/116_responsive_mobile_nav.md    (this file)
.kiro/KIRO.md                               (devlog "Current last:" bumped)
```

## Verification

* `cd web && pnpm build` — passes (Next 16 turbopack, 19 static routes).
* `cd web && pnpm test` (vitest) — 13 files, **83 tests pass**.
* `pnpm exec playwright test --list` — lists the two new mobile tests
  alongside the 18 existing tests.
* Full Playwright run requires both a Next dev server on `:3000` **and**
  the `zepto_http_server` backend on `:8123` (login flow). Neither was
  running in the implementation environment, so the appended Playwright
  block was syntactically validated via `--list` only.

## Test additions

`web/e2e/web-ui.spec.ts` gains a `Mobile responsive @ 375×667` describe
block (uses `test.use({ viewport: { width: 375, height: 667 } })`) with
two tests:

1. `marketing page shows hamburger and drawer opens with links` —
   logs in, navigates to `/home`, asserts the marketing hamburger
   inside `<main>` is visible, taps it, and verifies the right-anchored
   `Drawer` lists Solutions / Features / Performance / Pricing.
2. `console TopBar is full-width with no horizontal scroll` — logs in,
   goes to `/dashboard`, asserts `document.documentElement.scrollWidth
   <= clientWidth + 1`, asserts the `AppBar` `boundingBox().x` is ~0
   and width ≥ 370 px, and confirms the TopBar hamburger button is
   present.

Existing assertions are not modified.

## Follow-ups (out of scope for this change)

* Marketing pages currently sit under the same `AuthGuard`-wrapped
  `ClientShell` as the console, so `/home` etc. redirect to `/login`
  for anonymous visitors. That should be split (move marketing under
  its own root layout that bypasses `AuthGuard`). Tracked separately —
  this devlog only addresses responsive layout.
