# Devlog 060: Rebalance History Endpoint

Date: 2026-04-13

## Summary

Added `GET /admin/rebalance/history` endpoint and Web UI history table. Records completed rebalance events in an in-memory ring buffer (max 50 entries).

## What was added

### Backend
- `RebalanceHistoryEntry` struct in `rebalance_manager.h`
- `history()` method on `RebalanceManager` returns all entries
- `run_loop()` records an entry at the end of each rebalance (action, node, moves, duration, cancelled)
- `GET /admin/rebalance/history` HTTP endpoint returns JSON array

### Web UI
- `fetchRebalanceHistory()` API function
- `RebalanceHistory` component: table with time, action, node, moves, failed, duration, result chip (OK/PARTIAL/CANCELLED)
- 5s auto-refresh, most recent first

## Tests (5)

| Test | What it verifies |
|------|------------------|
| `HistoryRecordedAfterAddNode` | Entry recorded with correct action, node, moves, duration |
| `HistoryRecordedAfterCancel` | Cancelled flag set to true |
| `HistoryMultipleEntries` | Multiple rebalances produce multiple entries |
| `HistoryPartialMoveAction` | Partial move records action=NONE, node_id=0 |
| `HttpRebalanceHistory` | HTTP endpoint returns correct JSON |

## Files Changed

| File | Change |
|------|--------|
| `include/zeptodb/cluster/rebalance_manager.h` | `RebalanceHistoryEntry` struct, `history()` method, `history_`/`run_start_`/`MAX_HISTORY` members |
| `src/cluster/rebalance_manager.cpp` | `history()` impl, record entry in `run_loop()`, set `run_start_` in `start_plan()` |
| `src/server/http_server.cpp` | `GET /admin/rebalance/history` endpoint |
| `web/src/lib/api.ts` | `fetchRebalanceHistory()` |
| `web/src/app/cluster/page.tsx` | `RebalanceHistoryEntry` interface, `RebalanceHistory` component, query hook |
| `tests/unit/test_rebalance.cpp` | 5 new tests |
| `docs/api/HTTP_REFERENCE.md` | History endpoint docs |
| `docs/BACKLOG.md` | Marked as done |
| `docs/COMPLETED.md` | Added entry |
| `docs/devlog/060_rebalance_history.md` | This devlog |
