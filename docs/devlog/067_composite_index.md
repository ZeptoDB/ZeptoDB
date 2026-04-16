# Devlog 067: Composite Index (Index Intersection)

**Date:** 2026-04-15
**Status:** ✅ Complete
**Design Doc:** `docs/design/composite_index.md`

---

## Summary

Replaced the single-winner waterfall index selection pattern in the executor with a collect-and-intersect approach that combines multiple s#/g#/p# indexes per partition scan.

## Problem

The executor used a waterfall pattern where only ONE index was used per partition:
```
timestamp range → s# sorted → g#/p# equality → full scan
```
Multi-predicate queries like `WHERE timestamp BETWEEN ... AND exchange = 3 AND price BETWEEN ...` would use only the timestamp index and linearly scan the rest.

## Solution

### New: `IndexResult` accumulator struct
- Tracks `[range_begin, range_end)` and optional `row_set`
- `intersect_range()`: narrows range via `[max(b1,b2), min(e1,e2))`
- `intersect_set()`: filters g# row sets against current range/set
- `materialize()`: produces final row indices

### New: `collect_and_intersect()` method
Single entry point that replaces all waterfall patterns:
1. Timestamp range (if available)
2. ALL s# sorted column ranges (`extract_all_sorted_ranges`)
3. ALL g#/p# equality predicates (`extract_all_index_eqs`)
4. Intersect all results
5. `eval_remaining_where()` for non-indexed predicates

### Files Modified

| File | Change |
|------|--------|
| `include/zeptodb/sql/executor.h` | Added `IndexResult`, `SortedRangePred`, `IndexEqPred`, `extract_all_sorted_ranges`, `extract_all_index_eqs`, `eval_remaining_where`, `collect_and_intersect` |
| `src/sql/executor.cpp` | Implemented new methods; replaced waterfall in `exec_simple_select`, `exec_agg`, `exec_group_agg` (symbol-group, single-column, multi-column paths) |
| `docs/api/SQL_REFERENCE.md` | Added Composite Index Intersection subsection |

### Preserved

- Existing `extract_time_range`, `extract_sorted_col_range`, `extract_index_eq` functions (used by other code paths)
- All 1024 existing tests pass (zero regression)
- Parallel execution paths unchanged (they benefit when calling serial per-partition logic)

## Test Results

- 1024 tests passed, 0 failed
- SortedColumnQueryTest: all 5 pass (including BETWEEN, GE/LE, EQ, OutOfRange, RowsScanned)
