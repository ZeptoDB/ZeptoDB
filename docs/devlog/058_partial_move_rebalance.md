# Devlog 058: Partial-Move Rebalance API

Date: 2026-04-13

## Summary

Added `start_move_partitions()` to `RebalanceManager` — moves specific symbols between existing nodes without triggering a full node drain.

## Problem

The existing rebalance API only supports two operations:
- `start_add_node(NodeId)` — redistributes partitions to a new node
- `start_remove_node(NodeId)` — drains ALL partitions from a leaving node

Production operators need to move individual symbols for hot-symbol rebalancing, capacity planning, and maintenance without draining entire nodes.

## Solution

New method `start_move_partitions(vector<Move>)` accepts an explicit move list. Each move specifies `{symbol, from, to}`. Reuses the existing `start_plan()` → `run_loop()` execution path with `action_ = NONE` to skip ring topology broadcast.

HTTP API extended: `POST /admin/rebalance/start` now accepts `action: "move_partitions"` with a `moves` array.

## API

### C++
```cpp
std::vector<PartitionRouter::Move> moves = {{42, 1, 2}, {99, 2, 3}};
mgr.start_move_partitions(std::move(moves));
```

### HTTP
```bash
curl -X POST http://localhost:8123/admin/rebalance/start \
  -H 'Content-Type: application/json' \
  -d '{"action":"move_partitions","moves":[{"symbol":42,"from":1,"to":2}]}'
```

## Tests (6)

| Test | What it verifies |
|------|------------------|
| `PartialMovePartitions` | Single symbol move between existing nodes |
| `PartialMoveEmptyMoves` | Empty moves returns false |
| `PartialMoveMultipleSymbols` | 3 symbols moved in one call |
| `PartialMoveNoBroadcast` | No RingConsensus broadcast on partial move |
| `PartialMoveAlreadyRunning` | Rejected when rebalance already running |
| `HttpPartialMovePartitions` | HTTP endpoint with move_partitions action |

## Files Changed

| File | Change |
|------|--------|
| `include/zeptodb/cluster/rebalance_manager.h` | Added `start_move_partitions()` declaration |
| `src/cluster/rebalance_manager.cpp` | Added `start_move_partitions()` implementation |
| `src/server/http_server.cpp` | Added `move_partitions` action in `/admin/rebalance/start` |
| `tests/unit/test_rebalance.cpp` | 6 new tests |
| `docs/api/HTTP_REFERENCE.md` | Added `move_partitions` action docs |
| `docs/design/phase_c_distributed.md` | Added partial-move subsection |
| `docs/BACKLOG.md` | Marked partial-move as done |
| `docs/COMPLETED.md` | Added partial-move entry |
| `docs/devlog/058_partial_move_rebalance.md` | This devlog |
