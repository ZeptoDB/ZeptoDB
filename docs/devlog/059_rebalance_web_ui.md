# Devlog 059: Rebalance Progress in Web UI

Date: 2026-04-13

## Summary

Added a rebalance progress panel to the cluster dashboard page. Displays live status from `/admin/rebalance/status` with auto-refresh every 2 seconds.

## What was added

- `fetchRebalanceStatus()` API function in `web/src/lib/api.ts`
- `RebalanceStatus` component in cluster page showing:
  - State chip (RUNNING/PAUSED/CANCELLING/IDLE) with color coding
  - Progress bar (completed + failed / total moves)
  - Completed/total count, failed count (if any), current symbol
  - Blue border highlight when rebalance is active
  - Auto-hides when IDLE with no moves (nothing to show)

## Files Changed

| File | Change |
|------|--------|
| `web/src/lib/api.ts` | Added `fetchRebalanceStatus()` |
| `web/src/app/cluster/page.tsx` | Added `RebalanceInfo` interface, `RebalanceStatus` component, query hook |
| `docs/BACKLOG.md` | Marked rebalance Web UI as done |
| `docs/COMPLETED.md` | Added entry |
| `docs/devlog/059_rebalance_web_ui.md` | This devlog |
