# Devlog 074: Cost-Based Query Planner — Phase 3-6

**Date:** 2026-04-16
**Design doc:** `docs/design/cost_based_planner.md`

---

## Summary

Implemented the remaining phases (3-6) of the cost-based query planner, building on the Phase 1-2 foundation (TableStatistics + CostModel from devlog 073).

## What was implemented

### Phase 3: Logical Plan (`LogicalNode`, `LogicalPlan`)
- `LogicalNode` — tree of logical operators: SCAN, FILTER, PROJECT, AGGREGATE, JOIN, SORT, LIMIT, WINDOW
- `LogicalPlan::build()` — converts `SelectStmt` AST into a logical operator tree
  - Simple SELECT: SCAN → FILTER → PROJECT → SORT → LIMIT
  - JOIN: two SCANs → JOIN → FILTER → PROJECT → SORT → LIMIT
  - GROUP BY: SCAN → FILTER → AGGREGATE → SORT → LIMIT
- `LogicalPlan::optimize()` — rule-based optimizations:
  - Predicate pushdown: moves FILTER below JOIN when it references only one side
  - Projection pushdown: propagates needed columns to SCAN nodes

### Phase 4: Physical Plan (`PhysicalNode`, `PhysicalPlanner`)
- `PhysicalNode` — tree of physical operators with cost estimates
- `PhysicalPlanner::plan()` — converts logical plan to physical plan using CostModel + TableStatistics
  - SCAN → SEQ_SCAN (default), INDEX_SCAN (selectivity ≤ 0.15), SORTED_RANGE_SCAN
  - JOIN → HASH_JOIN (smaller side as build), ASOF_JOIN, WINDOW_JOIN
  - AGGREGATE → HASH_AGGREGATE
  - SORT + LIMIT → TOPN_SORT, else FULL_SORT
- `PhysicalNode::describe()` — single-line description for EXPLAIN output

### Phase 5: exec_select Integration
- 2-tier adaptive routing via `needs_cost_planning()`:
  - Simple queries (no JOIN, no CTE, no subquery, no set-op) → existing fast path unchanged
  - Complex queries → build logical plan, optimize, generate physical plan
- Physical plan is observation-only — existing execution logic is completely unchanged
- `TableStatistics` updated for queried partitions before planning

### Phase 6: EXPLAIN Enhancement
- Complex queries: EXPLAIN outputs indented physical plan tree with cost estimates
- Simple queries: existing text-based EXPLAIN format preserved
- Tree format with `├──` / `└──` connectors, node type, cost, estimated rows

## Files changed

| File | Change |
|------|--------|
| `include/zeptodb/execution/query_planner.h` | Replaced placeholder with LogicalNode, LogicalPlan, PhysicalNode, PhysicalPlanner, format_explain_tree |
| `src/execution/query_planner.cpp` | Full implementation (build, optimize, plan, describe, format) |
| `include/zeptodb/sql/executor.h` | Added table_stats_, last_physical_plan_, needs_cost_planning(), query_planner.h include |
| `src/sql/executor.cpp` | Added planner integration in exec_select, enhanced EXPLAIN for complex queries |
| `tests/unit/test_query_planner.cpp` | 20 tests: logical build, predicate pushdown, physical plan, needs_cost_planning, EXPLAIN output |
| `tests/CMakeLists.txt` | Added test_query_planner.cpp |

## Test results

- 20 new tests, all passing
- 1100 total tests (1080 existing + 20 new), zero regressions

## Design decisions

1. **Observation-only**: Physical plan is built but not used for execution. This is the safe incremental approach — future phases can wire the plan to actual execution.
2. **2-tier routing**: Simple queries pay zero overhead (field comparisons only, ~10ns). Complex queries get full cost-based planning.
3. **Predicate pushdown**: Only pushes predicates with explicit table_alias to avoid ambiguity.
4. **Build side selection**: For HASH_JOIN, the smaller estimated side is chosen as build side.
5. **TOPN_SORT upgrade**: When LIMIT is present above a SORT, FULL_SORT is upgraded to TOPN_SORT.
