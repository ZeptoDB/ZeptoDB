# Devlog 073: Cost-Based Query Planner — Phase 1 & 2

**Date:** 2026-04-16
**Status:** Complete
**Design Doc:** `docs/design/cost_based_planner.md`

---

## Summary

Implement the foundation for ZeptoDB's cost-based query planner:
- **Phase 1:** Statistics collector (`TableStatistics`) — per-partition, per-column stats with incremental updates
- **Phase 2:** Cost model (`CostModel`) — selectivity estimation and cost calculation for scan/join/aggregate operators

These phases are observation-only — they do not change query execution paths. They provide:
1. Accurate row count estimates for EXPLAIN
2. Foundation for future cost-based plan selection (Phase 3-4)
3. Selectivity-based index vs full scan decisions (future)

## Key Design Decision: Adaptive 2-Tier Planning

Not all queries go through cost-based planning. Simple single-table queries use the existing fast path (zero overhead). Cost-based planning activates only for JOINs, CTEs, subqueries, and set operations.

```
needs_cost_planning(stmt) → true only if JOIN / CTE / subquery / set-op present
```

## Files Changed

| File | Change |
|------|--------|
| `include/zeptodb/execution/table_statistics.h` | NEW — ColumnStats, PartitionStats, TableStatistics |
| `src/execution/table_statistics.cpp` | NEW — incremental update, HLL distinct, merge |
| `include/zeptodb/execution/cost_model.h` | NEW — CostEstimate, CostModel, selectivity |
| `src/execution/cost_model.cpp` | NEW — cost calculation implementations |
| `tests/unit/test_table_statistics.cpp` | NEW — statistics collection tests |
| `tests/unit/test_cost_model.cpp` | NEW — cost estimation tests |
| `docs/design/cost_based_planner.md` | NEW — full design doc |

## Statistics Collected

Per column: min, max, null_count, distinct_count (HyperLogLog ~2% error, 128 bytes).
Per partition: row_count, ts_min, ts_max.
Updated incrementally on append. Frozen on partition seal.

## Cost Model Parameters

Selectivity estimation: equality (1/NDV), range ((hi-lo)/(max-min)), IN (list_size/NDV).
Cost factors: sequential scan, index probe, hash build/probe, sort, aggregate, SIMD discount.

## Test Coverage

- ColumnStats: incremental min/max/count update, distinct approximation
- PartitionStats: multi-column stats, seal freeze
- TableStatistics: cross-partition merge, ANALYZE refresh
- CostModel: selectivity for EQ/range/IN/AND/OR, scan cost comparison, join cost

## Implementation Results

- **27 new tests** (12 TableStatistics + 15 CostModel), all passing
- **1080 total tests**, zero regressions
- HyperLogLog: 64 registers, FNV-1a hash, ~30 lines of code
- Cost model: 6 cost functions + 5 selectivity estimators
- Observation-only — no changes to existing query execution paths

### Build Integration

- `src/execution/table_statistics.cpp` and `src/execution/cost_model.cpp` added to `zepto_execution` library in root `CMakeLists.txt`
- `tests/unit/test_table_statistics.cpp` and `tests/unit/test_cost_model.cpp` added to `tests/CMakeLists.txt`

### Key Implementation Details

- **HyperLogLog:** 64 registers (m=64), FNV-1a 64-bit hash, alpha=0.709, small range correction for low cardinality. ~2-15% error acceptable for cost estimation.
- **ColumnStats::update():** O(1) — min/max comparison + HLL register update + estimate recalc
- **TableStatistics:** Thread-safe with `std::mutex`. `update_partition()` scans all columns. `aggregate()` merges all partition stats.
- **CostModel:** Static methods, no state. Cost constants: SEQ_COST=1.0, RANDOM_COST=4.0, INDEX_PROBE=0.5, HASH_BUILD=1.5, HASH_PROBE=0.5, SORT_COST=2.0, AGG_COST=1.0
- **Index scan crossover:** ~15-25% selectivity (index scan preferred below, seq scan above)

## Post-Review Fixes

Reviewer found 2 critical + 3 warning issues. All fixed:

1. **CRITICAL: Use-after-move UB** — `update_partition` logged `ps.row_count` after `std::move(ps)`. Fixed: capture `rows` before move.
2. **CRITICAL: `record_append` row_count semantics** — was incrementing `row_count` per column-append, not per row. Fixed: added `record_row()` method; `record_append` no longer touches `row_count`.
3. **WARNING: `estimate_aggregate` est_rows** — was returning input `rows` instead of `groups`. Fixed: `est_rows = groups`.
4. **WARNING: Integer overflow in `selectivity_range`** — cast to `double` before subtraction to avoid UB on large int64 ranges.
5. **WARNING: Doc ↔ Code HLL size** — design doc said 128 bytes, code uses 64 bytes (64 registers). Fixed docs.

## Test Results

- New tests: 27/27 pass (12 TableStatistics + 15 CostModel)
- Full suite: 1080/1080 pass, 0 regressions
