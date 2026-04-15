# Devlog 064: MV Query Rewrite

Date: 2026-04-14

## Summary

Implemented automatic query rewriting that redirects SELECT ... GROUP BY queries
to pre-aggregated materialized view lookups when an exact-matching MV exists.
This eliminates full partition scans entirely for matching queries — O(n) → O(1).

## What Changed

### New Files
- `include/zeptodb/sql/mv_rewriter.h` — header-only rewriter with `MVRewriter::try_rewrite()`
- `tests/unit/test_mv_rewrite.cpp` — 6 unit tests

### Modified Files
- `include/zeptodb/storage/materialized_view.h` — added `get_all_defs()` method
- `src/storage/materialized_view.cpp` — `get_all_defs()` implementation
- `src/sql/executor.cpp` — MV rewrite check inserted before `exec_select()`
- `tests/CMakeLists.txt` — added test_mv_rewrite.cpp

## How It Works

The rewriter sits between the query cache check and `exec_select()` in
`QueryExecutor::execute()`. For each SELECT with GROUP BY:

1. Guard checks: no WHERE, no JOIN, no HAVING, no DISTINCT, no subquery, no CTE
2. Map each SQL `AggFunc` to storage `MVAggType` (SUM, COUNT, MIN, MAX, FIRST, LAST)
3. Iterate all registered MVDefs via `get_all_defs()`
4. Match: source_table, group_by columns (order-insensitive), xbar_bucket, agg columns
5. On match: call `MaterializedViewManager::query()` and return directly

ORDER BY and LIMIT are still applied on the MV result.

## Matching Rules

| Condition | Required |
|-----------|----------|
| GROUP BY present | Yes |
| No WHERE clause | Yes |
| No JOIN | Yes |
| No HAVING | Yes |
| No DISTINCT | Yes |
| source_table matches | Yes |
| All agg columns match (type + source_col) | Yes |
| group_by columns match (set equality) | Yes |
| xbar_bucket matches | Yes |

Unmappable aggregations (AVG, VWAP, STDDEV, etc.) cause immediate fallback to
full scan — no partial MV coverage in this phase.

## Test Results

```
[  PASSED  ] 1004 tests.  (0 failures, 0 regressions)
```

6 new tests:
- ExactMatch — MV hit returns correct pre-aggregated data
- NoMatchWhere — WHERE present → falls through to full scan
- NoMatchAggMismatch — avg() vs sum() MV → no rewrite
- NoMVRegistered — no MV → normal execution
- MultipleAggColumns — multi-column MV match
- NoMatchTableMismatch — wrong table → no rewrite

## Future Work

- Partial column coverage (query uses subset of MV columns)
- WHERE pushdown for filtered MVs
- AVG rewrite via SUM/COUNT decomposition
- EXPLAIN output indicating MV rewrite was used
