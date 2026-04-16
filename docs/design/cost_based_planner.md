# Cost-Based Query Planner

> Design doc for ZeptoDB's adaptive cost-based query planning system.
> Related code: `include/zeptodb/execution/`, `include/zeptodb/sql/executor.h`

---

## 1. Motivation

The current query executor (`QueryExecutor::exec_select`) uses hard-coded rules to choose execution paths:

| Decision | Current Approach | Problem |
|----------|-----------------|---------|
| JOIN build side | Always left→right | Small table should be build side |
| Index vs full scan | Rule: if index exists, use it | High-selectivity index scan is slower than seq scan |
| Parallel execution | row_count > 100K threshold | Ignores partition count, operator complexity |
| MV rewrite | Always apply if match found | MV scan may be costlier than filtered original |
| EXPLAIN output | Text-only plan | No cost estimates, no row count predictions |

A cost-based planner addresses these by estimating execution cost from table statistics and choosing the cheapest physical plan.

---

## 2. Adaptive Planning (2-Tier)

**Key insight:** Full cost-based optimization adds ~1-5μs overhead. For simple queries executing in ~10-50μs, this is 2-10% overhead — unacceptable for a low-latency time-series DB.

### Solution: Complexity-based routing

```
SQL → Parse → stmt cache hit? ──yes──→ Cached Plan → Execute
                    │ no
                    ▼
            needs_cost_planning(stmt)?
                /          \
              no            yes
              │              │
         Fast Path      Cost-Based Planning
      (current exec_select)  (Logical → Optimize → Physical)
              │              │
              ▼              ▼
            Execute        Execute
```

### Tier 1: Fast Path (rule-based, ~0 overhead)

Criteria — ALL must be true:
- Single table (no JOIN)
- No FROM subquery
- No CTE (WITH clause)
- No set operations (UNION/INTERSECT/EXCEPT)

These queries use the existing `exec_select` code path unchanged.

### Tier 2: Cost-Based Planning

Triggered when ANY is true:
- JOIN present
- FROM subquery
- CTE definitions
- Set operations

```cpp
bool needs_cost_planning(const SelectStmt& stmt) {
    if (stmt.join)                         return true;
    if (stmt.from_subquery)                return true;
    if (!stmt.cte_defs.empty())            return true;
    if (stmt.set_op != SetOp::NONE)        return true;
    return false;
}
```

Complexity check cost: ~10ns (field comparisons only).

### Plan Caching

Physical plans are cached alongside prepared statements in `stmt_cache_`:
- Cache key: `sql_hash(sql)`
- Invalidation: when statistics change significantly (row count 2x delta)
- Cache hit cost: ~50ns (hash lookup)

---

## 3. Architecture

### 3.1 Statistics Collector (Phase 1)

Per-partition, per-column statistics collected incrementally during ingestion.

```
TableStatistics
├── PartitionStats (per partition, updated on append/seal)
│   ├── row_count: size_t
│   ├── ts_min / ts_max: int64_t
│   └── ColumnStats (per column)
│       ├── min_value / max_value: int64_t
│       ├── null_count: size_t
│       └── distinct_count_approx: size_t  (HyperLogLog, 64 bytes)
│
└── TableStats (aggregated on demand)
    ├── total_rows: size_t
    ├── partition_count: size_t
    └── ColumnStats (merged min/max/distinct across partitions)
```

**File:** `include/zeptodb/execution/table_statistics.h`

Design decisions:
- **Incremental update:** min/max/count updated on every append — O(1) per row
- **HyperLogLog for distinct count:** ~2% error, 64 bytes per column — acceptable for cost estimation
- **No histogram in Phase 1:** Uniform distribution assumption with min/max range. Histograms added later for skewed data
- **Seal snapshot:** When partition is sealed, stats are frozen (immutable)
- **ANALYZE TABLE command:** Triggers full re-scan for accurate stats (optional, not required for basic operation)

### 3.2 Cost Model (Phase 2)

**File:** `include/zeptodb/execution/cost_model.h`

Cost unit: **abstract cost units** (not real nanoseconds, but proportional).

```
TotalCost = io_cost + cpu_cost

I/O Cost:
  seq_scan:         rows × BYTES_PER_ROW × SEQ_COST_FACTOR
  index_scan:       log2(rows) × INDEX_PROBE_COST + result_rows × RANDOM_COST
  partition_prune:  (1 - pruned_ratio) × base_scan_cost

CPU Cost:
  filter:           rows × FILTER_COST
  hash_build:       build_rows × HASH_BUILD_COST
  hash_probe:       probe_rows × HASH_PROBE_COST
  sort:             rows × log2(rows) × SORT_COST
  aggregate:        rows × AGG_COST
  simd_discount:    0.125 × scalar_cost  (8-wide SIMD lanes)
```

Time-series specific parameters:
- **Partition pruning:** WHERE timestamp range eliminates partitions at O(1) — massive I/O reduction
- **Sorted column (s#):** Binary search O(log n) vs full scan O(n)
- **Symbol affinity:** Same-symbol data co-located in partition — cache locality bonus
- **Columnar scan:** Only requested columns loaded — proportional to SELECT list width

Key cost estimation functions:

```cpp
struct CostEstimate {
    double io_cost   = 0.0;
    double cpu_cost  = 0.0;
    size_t est_rows  = 0;      // estimated output rows
    double total() const { return io_cost + cpu_cost; }
};

CostEstimate estimate_scan(const TableStats& stats, const WhereClause* where);
CostEstimate estimate_index_scan(const TableStats& stats, const std::string& col, double selectivity);
CostEstimate estimate_hash_join(const CostEstimate& build, const CostEstimate& probe);
CostEstimate estimate_sort(size_t rows, size_t key_count);
CostEstimate estimate_aggregate(size_t rows, size_t group_count);
```

Selectivity estimation:
- Equality: `1.0 / distinct_count`
- Range (BETWEEN): `(hi - lo) / (max - min)` (uniform assumption)
- IN list: `list_size / distinct_count`
- AND: `sel_a × sel_b` (independence assumption)
- OR: `sel_a + sel_b - sel_a × sel_b`

### 3.3 Logical Plan (Phase 3)

AST → tree of logical operators. Separates "what" from "how".

```
LogicalScan → LogicalFilter → LogicalProject → LogicalAggregate
                                                      ↓
                                              LogicalSort → LogicalLimit
```

Rule-based optimizations (before cost-based):
1. Predicate pushdown
2. Projection pushdown
3. Partition pruning
4. MV rewrite (existing `mv_rewriter.h`)

### 3.4 Physical Plan (Phase 4)

Logical → Physical with cost-based alternative selection:

| Logical | Physical Alternatives |
|---------|----------------------|
| Scan | SeqScan, IndexScan, SortedRangeScan, PartedScan |
| Join | HashJoin(left-build), HashJoin(right-build), AsofJoin |
| Aggregate | HashAggregate, SortAggregate |
| Sort | FullSort, TopNSort (when LIMIT present) |

Enumeration strategy:
- 2-table JOIN: compare both build sides → pick cheaper
- 3+ tables: greedy (smallest intermediate first)
- Scan method: selectivity ≤ 15% → index scan, else → seq scan

---

## 4. Implementation Phases

| Phase | Scope | Deliverables | Effort |
|-------|-------|-------------|--------|
| **1** | Statistics | `TableStatistics`, `ColumnStats`, `PartitionStats`, incremental collection, `ANALYZE TABLE` | M |
| **2** | Cost Model | `CostModel`, selectivity estimation, `CostEstimate`, EXPLAIN cost output | S |
| **3** | Logical Plan | `LogicalPlan` nodes, rule-based optimizer, predicate/projection pushdown | M |
| **4** | Physical Plan | `PhysicalPlan` nodes, cost-based enumeration, 2-tier adaptive routing | L |
| **5** | Integration | `exec_select` integration, `needs_cost_planning()` routing, observation-only | S |
| **6** | EXPLAIN v2 | Cost estimates, row estimates, indented tree output for complex queries | S |
| **7** | Wiring | HASH_JOIN build side wired to PhysicalPlan, TOPN_SORT/INDEX_SCAN already effective | S |

**Phase 1-7 are all implemented.** Statistics and cost model provide the foundation. Logical/physical plans are built for complex queries (JOIN, CTE, subquery, set-op) and used for EXPLAIN output and execution decisions. Simple queries continue to use the existing fast path unchanged. The HASH_JOIN build side decision is wired from PhysicalPlan to `exec_hash_join` (INNER/FULL joins). TOPN_SORT is effectively wired via `apply_order_by` partial sort. INDEX_SCAN is effectively wired via `collect_and_intersect` automatic index usage.

---

## 5. File Map

| File | Contents |
|------|----------|
| `include/zeptodb/execution/table_statistics.h` | ColumnStats, PartitionStats, TableStatistics |
| `src/execution/table_statistics.cpp` | Incremental update, HLL, merge logic |
| `include/zeptodb/execution/cost_model.h` | CostEstimate, CostModel, selectivity functions |
| `src/execution/cost_model.cpp` | Cost calculation implementations |
| `include/zeptodb/execution/query_planner.h` | LogicalNode, LogicalPlan, PhysicalNode, PhysicalPlanner |
| `src/execution/query_planner.cpp` | Logical/physical plan build, optimize, EXPLAIN formatting |
| `tests/unit/test_table_statistics.cpp` | Statistics collection tests |
| `tests/unit/test_cost_model.cpp` | Cost estimation tests |
| `tests/unit/test_query_planner.cpp` | Logical/physical plan, predicate pushdown, EXPLAIN tests |

---

## 6. Integration Points

- **Ingestion path:** `Partition::add_column` / append → update ColumnStats incrementally
- **Partition seal:** Freeze stats snapshot
- **QueryExecutor:** `needs_cost_planning()` routing, EXPLAIN cost display
- **MV rewriter:** Cost comparison (MV scan vs original) in Phase 3+
- **Prepared statements:** Plan cache alongside parse cache

---

## 7. Risks & Mitigations

| Risk | Mitigation |
|------|-----------|
| Statistics stale after bulk insert | Auto-refresh on seal; ANALYZE for manual refresh |
| Cost model inaccuracy | Conservative defaults; adaptive tuning from actual execution times (Phase 5+) |
| Planning overhead on simple queries | 2-tier adaptive routing — simple queries skip planning entirely |
| Regression in existing queries | Phase 1-2 are observation-only; execution path unchanged until Phase 4 |
