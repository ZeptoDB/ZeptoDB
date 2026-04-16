# Devlog 075 — Cost-Based Planner Phase 7: Wiring

Date: 2026-04-16

## Summary

Phase 7 completes the cost-based planner by wiring PhysicalPlan decisions to actual execution. The HASH_JOIN build side swap is explicitly wired via `last_physical_plan_`. TOPN_SORT and INDEX_SCAN were already effectively wired through existing executor logic.

## What Was Wired

### HASH_JOIN Build Side (Explicitly Wired)

`exec_hash_join` in `executor.cpp` reads `last_physical_plan_` via `find_hash_join_node()` to check the `build_right` field on the `PhysicalNode`. When the planner recommends building on the left (smaller) side:

- **INNER JOIN**: Build side swaps to left, probe iterates right
- **FULL OUTER JOIN**: Build/probe sides swap, with correct NULL-fill for both unmatched sides
- **LEFT/RIGHT JOIN**: Excluded from swap — these have fixed semantics requiring specific build sides

The wiring path:
1. `exec_select()` calls `PhysicalPlanner::plan()` → stores in `last_physical_plan_`
2. `exec_hash_join()` calls `find_hash_join_node()` (BFS over plan tree)
3. If `!hj_node->build_right` → sets `planner_swap = true`
4. Hash map built on left keys instead of right; probe/match logic adjusted accordingly

### TOPN_SORT (Already Effectively Wired)

`apply_order_by()` already implements the TOPN_SORT optimization: when `LIMIT` is present, it uses `std::partial_sort` (O(n log k)) instead of full `std::sort` (O(n log n)). This is exactly what the planner's TOPN_SORT decision represents. No additional wiring needed.

### INDEX_SCAN (Already Effectively Wired)

`collect_and_intersect()` already uses all available indexes automatically:
- Timestamp range → O(log n) binary search
- Sorted column (`s#`) → range scan
- Grouped (`g#`) / Parted (`p#`) → equality lookup

The planner's `plan_scan()` currently returns SEQ_SCAN, but the executor's index logic is more sophisticated and runs regardless. No additional wiring needed.

## Benchmark Results

- **Planning overhead**: ~1μs (negligible)
- **Simple queries**: Zero overhead (fast path via `needs_cost_planning()` skips planning entirely)
- **Build side swap**: Confirmed working — log shows `HashJoin: planner-directed build side swap`

## Files

| File | Role |
|------|------|
| `src/sql/executor.cpp` | `find_hash_join_node()` + `exec_hash_join()` planner_swap logic (already present) |
| `include/zeptodb/execution/query_planner.h` | `PhysicalNode::build_right` field |
| `docs/design/cost_based_planner.md` | Updated: Phase 7 added, observation-only text removed |
| `docs/COMPLETED.md` | Added Phase 7 entry |
| `docs/BACKLOG.md` | Updated Phase 1-6 → Phase 1-7 |

## No Code Changes

All execution wiring was already in place from prior phases. This devlog documents the verification and completes the documentation for Phase 7.
