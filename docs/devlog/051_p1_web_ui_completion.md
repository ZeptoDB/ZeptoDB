# 051 — P1 Web UI Completion: Dashboard & Bug Fixes

Date: 2026-04-07

## Summary

Completed all remaining P1 ("A Product We Can Demo") items. The Web UI is now fully demo-ready with a comprehensive dashboard, cluster monitoring, and query editor.

## Changes

### Dashboard Enhancement (`/dashboard`)

Rewrote the dashboard page from a basic stats view to a full overview:

- 5 stat cards: Ticks Ingested, Ticks Stored, Queries Executed, Partitions, Ingest Latency
- Drop rate warning banner (yellow >0%, red >1%)
- Live ingestion rate line chart (2s interval, 30-point rolling window)
- Tables summary: table list with column count and row count, fetched via `SHOW TABLES` + `DESCRIBE` + `SELECT count(*)`
- Rows-per-table bar chart (color-coded)
- Secondary stats row: Total Rows Scanned, Ticks Dropped, Avg Query Cost

### Navigation Changes

- Dashboard moved to first position in sidebar (was second after Query)
- Dashboard now visible to `analyst` role (previously excluded)
- Default landing page (`/`) redirects to `/dashboard` instead of `/query`

### Bug Fix: Broken Template Literals in `api.ts` and `auth.tsx`

The `API` base path variable was introduced but string literals were not properly converted — they used `` `${API}/path" `` (backtick start, double-quote end), causing parse failures in production builds. Fixed all ~20 occurrences across both files.

### Bug Fix: API URL Consistency

- `fetchMetricsHistory` had a hardcoded `/api/` prefix in one branch — unified to use `${API}`
- `updateKey`, `revokeKey`, `killQuery`, `fetchAudit`, `fetchKeyUsage`, `deleteTenant` had hardcoded `/api/` — unified to `${API}`

### Test Updates

- `sidebar.test.ts`: Updated expected order (Dashboard first), analyst now sees Dashboard
- `api.test.ts`, `tables.test.ts`: Updated expected URL from `/api` to `/api/` (trailing slash from `${API}/` pattern)
- `ui_theme.test.ts`: Fixed pre-existing color mismatch (test expected old Quantum theme colors, actual theme uses Electric Indigo)

## Result

- Build: ✅ `next build` passes
- Tests: ✅ 54/54 passing (9 test files)
- P1 status: ✅ Complete
