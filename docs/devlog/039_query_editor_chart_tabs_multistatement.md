# 039 — Query Editor: Chart View, Multi-Tab, Multi-Statement

Date: 2026-04-01

## Summary

Three query editor enhancements implemented in a single pass:

- **QE-5 (Result chart view)** — Toggle between table and chart (line/bar) for query results. Uses Recharts. User selects X column, Y columns (multi-select), and chart type. Caps at 500 rows for performance.
- **QE-1 (Multi-tab editor)** — Tab bar above the editor. Add/close/rename (double-click) tabs. Each tab has independent code and results. Tabs persist to localStorage (without results).
- **QE-9 (Multi-statement run)** — Splits SQL on `;` (respecting single-quoted strings). Executes statements sequentially. Each result shown as a sub-tab in the results area. Errors per-statement, not all-or-nothing.

## Files Changed

| File | Change |
|------|--------|
| `web/src/app/query/page.tsx` | Rewritten: tab state, multi-statement `run()`, result sub-tabs, chart/table toggle |
| `web/src/components/ResultChart.tsx` | New: line/bar chart with X/Y column selectors |

## Design Decisions

- **Statement splitting**: Simple `;` split with single-quote tracking. Does not handle `$$` dollar-quoting or `--` comments containing `;`. Sufficient for ZeptoDB's SQL dialect.
- **Tab persistence**: Only code and name saved to localStorage, not results (too large). Results cleared on reload.
- **Chart row limit**: 500 rows max to avoid browser performance issues with Recharts SVG rendering.
- **View mode**: Global toggle (not per-result-tab) — simpler UX, user typically wants same view for all results.

## Testing

- Existing `query.test.ts` (3 tests) passes — `loadHistory` export preserved.
- Build: `next build` passes with zero TypeScript errors.
- Pre-existing failure: `ui_theme.test.ts` color mismatch (unrelated).
