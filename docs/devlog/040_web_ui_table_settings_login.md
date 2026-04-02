# 040 — Web UI: Table Detail Page, Settings Enhancement, Login Polish

Date: 2026-04-01

## Summary

Three Web UI improvements targeting investor demo readiness:

1. **`/tables/[name]` dynamic route** — Dedicated table detail page with schema, column stats (min/max), row count summary cards, and data preview. Tables list page now navigates to detail on click instead of inline expansion.

2. **Settings page enhancement** — Added server info section (engine version, build date, health status) alongside existing runtime properties. Uses `fetchVersion` and `fetchHealth` APIs.

3. **Login page polish** — Gradient accent bar, tagline chip ("Nanosecond Time-Series Database"), keyboard hint ("Press Enter to submit"), Quick Start Guide link, footer branding. Improved visual hierarchy.

## Files Changed

| File | Change |
|------|--------|
| `web/src/app/tables/[name]/page.tsx` | New — table detail page with schema, column stats, preview |
| `web/src/app/tables/page.tsx` | Simplified — removed inline detail, rows navigate to `/tables/[name]` |
| `web/src/app/settings/page.tsx` | Enhanced — added server info section (version, health), extracted `Row` component |
| `web/src/app/login/page.tsx` | Polished — branding, accent bar, tagline, keyboard hint, footer |

## Design Decisions

- Column stats (min/max) only fetched for numeric/timestamp columns to avoid string aggregation errors
- Table detail uses `DESCRIBE`, `count(*)`, and `LIMIT 50` preview — same SQL patterns as existing code
- Settings page reuses existing `fetchVersion`/`fetchHealth` API functions — no backend changes needed
- Login page SSO button remains disabled placeholder per original design
