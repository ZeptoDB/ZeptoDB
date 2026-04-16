# Composite Index Design

> Multi-column index for ZeptoDB — turns multi-predicate WHERE scans from O(n) to O(log n + k).

**Status:** Design  
**Priority:** P7 (Engine Performance) — 🔴 Major impact  
**Effort:** M  
**Author:** AI Orchestrator  
**Date:** 2026-04-15

---

## 1. Problem

The current executor applies index optimizations in a **waterfall** (single-winner) pattern:

```
extract_time_range  →  extract_sorted_col_range  →  extract_index_eq  →  full scan
        ↓ miss                  ↓ miss                    ↓ miss              ↓
     try next              try next                  try next          O(n) scan
```

Only **one** index is used per partition scan. A query like:

```sql
SELECT * FROM trades
WHERE exchange = 3 AND price BETWEEN 15000 AND 16000
AND timestamp BETWEEN t1 AND t2
```

Uses the timestamp `s#` range to narrow rows, but then **linearly scans** the remaining rows for `exchange = 3` and `price BETWEEN ...`, even though `exchange` has a `p#` index and `price` has a `g#` index.

### Impact

For a partition with 1M rows where timestamp narrows to 100K rows:
- Current: O(log n) timestamp + **O(100K)** linear scan for remaining predicates
- With composite: O(log n) timestamp → O(1) exchange → **O(k)** where k = matching rows

---

## 2. Design Goals

1. **Combine existing indexes** — intersect results from s#, g#, p# across multiple columns
2. **Zero regression** — single-index queries must not slow down
3. **No new DDL** — reuse existing `ALTER TABLE ... SET ATTRIBUTE` per column
4. **Automatic** — executor detects multi-column index opportunities without user hints
5. **Minimal code change** — modify the executor's eval_where path, not the storage layer

---

## 3. Approach: Index Intersection

Instead of a new composite data structure, **intersect row sets** from multiple existing indexes.

### 3.1 Why Intersection Over a New Structure

| Option | Pros | Cons |
|--------|------|------|
| New B-tree on (col1, col2, ...) | Single lookup | New storage structure, DDL changes, maintenance on insert, column-order dependent |
| **Intersect existing indexes** | Zero storage overhead, reuses s#/g#/p#, order-independent | Intersection cost, less optimal than purpose-built composite |

For ZeptoDB's workload (time-series, append-only, partition-scoped), intersection is the right trade-off:
- Partitions are small (1 hour of 1 symbol) — intersection sets are bounded
- Existing indexes already cover the common predicates
- No write-path changes needed (indexes are built on `SET ATTRIBUTE`, not on insert)

### 3.2 Intersection Strategy

```
Step 1: Extract ALL usable index predicates from WHERE clause
        → List of (column, index_type, predicate_value)

Step 2: Execute each index lookup independently
        → s# sorted_range()  → [begin, end) row range
        → g# grouped_lookup() → vector<uint32_t> row indices
        → p# parted_range()  → [begin, end) row range

Step 3: Intersect results (smallest-first)
        → Produce final candidate row set

Step 4: Evaluate remaining (non-indexed) predicates on candidates only
```

### 3.3 Intersection Algorithm

Two cases based on result types:

**Range ∩ Range** (s# or p# results): Simple overlap → `[max(begin1, begin2), min(end1, end2))`

**Range ∩ Set** (range result + g# row indices): Filter the set to keep only indices within range.

**Set ∩ Set** (multiple g# results): Sort smaller set, binary search from larger — O(m log m + n log m) where m < n. For small sets (< 1024), use linear merge on sorted vectors.

---

## 4. Executor Changes

### 4.1 Current Path (executor.cpp ~line 2540)

```cpp
// Waterfall: first match wins, rest ignored
if (use_ts_index) {
    // timestamp range only
} else if (extract_sorted_col_range(...)) {
    // one s# column only
} else if (extract_index_eq(...)) {
    // one g#/p# column only
} else {
    // full scan
}
```

### 4.2 New Path

Replace the waterfall with a **collect-and-intersect** approach:

```cpp
// Phase 1: Collect all applicable index results
IndexResult combined;  // holds [begin,end) range + optional row set

// Always start with timestamp range if available
if (extract_time_range(stmt, ts_lo, ts_hi)) {
    auto [rb, re] = part->timestamp_range(ts_lo, ts_hi);
    combined.set_range(rb, re);
}

// Layer on s# sorted column ranges
for (auto& [col, lo, hi] : extract_all_sorted_ranges(stmt, *part)) {
    auto [rb, re] = part->sorted_range(col, lo, hi);
    combined.intersect_range(rb, re);
}

// Layer on g#/p# equality predicates
for (auto& [col, val] : extract_all_index_eqs(stmt, *part)) {
    if (part->is_grouped(col)) {
        combined.intersect_set(part->grouped_lookup(col, val));
    } else if (part->is_parted(col)) {
        auto [rb, re] = part->parted_range(col, val);
        combined.intersect_range(rb, re);
    }
}

// Phase 2: Materialize candidate rows
sel_indices = combined.materialize();
rows_scanned += sel_indices.size();

// Phase 3: Evaluate remaining non-indexed predicates
sel_indices = eval_remaining_where(stmt, *part, sel_indices, indexed_columns);
```

### 4.3 IndexResult Helper

A lightweight struct that tracks the intersection state:

```cpp
struct IndexResult {
    size_t range_begin = 0;
    size_t range_end   = SIZE_MAX;  // unbounded initially
    std::vector<uint32_t> row_set;  // empty = use range only
    bool has_set = false;

    void set_range(size_t b, size_t e);
    void intersect_range(size_t b, size_t e);      // narrow the range
    void intersect_set(const std::vector<uint32_t>& s);  // intersect with row set
    std::vector<uint32_t> materialize() const;      // produce final row indices
};
```

### 4.4 New Extraction Functions

```cpp
// Returns ALL sorted-column range predicates (not just the first)
std::vector<SortedRangePred> extract_all_sorted_ranges(
    const SelectStmt& stmt, const Partition& part) const;

// Returns ALL equality predicates on indexed columns
std::vector<IndexEqPred> extract_all_index_eqs(
    const SelectStmt& stmt, const Partition& part) const;

// Evaluate WHERE excluding already-indexed predicates
std::vector<uint32_t> eval_remaining_where(
    const SelectStmt& stmt, const Partition& part,
    const std::vector<uint32_t>& candidates,
    const std::unordered_set<std::string>& indexed_cols) const;
```

---

## 5. Query Examples

### Example 1: Timestamp + Exchange (s# + p#)

```sql
SELECT * FROM trades
WHERE timestamp BETWEEN t1 AND t2 AND exchange = 3
```

Before: timestamp range → 100K rows → linear scan for exchange = 3
After: timestamp range [r1,r2) ∩ exchange parted_range [r3,r4) → max(r1,r3)..min(r2,r4) → **O(1) intersection**

### Example 2: Timestamp + Price Range (s# + s#)

```sql
SELECT * FROM trades
WHERE timestamp BETWEEN t1 AND t2 AND price BETWEEN 15000 AND 16000
```

Before: timestamp range only, price scanned linearly
After: timestamp range ∩ price sorted_range → two O(log n) lookups + range overlap

### Example 3: Exchange + OrderType (p# + g#)

```sql
SELECT * FROM trades
WHERE exchange = 3 AND order_type = 5
```

Before: picks one index (first match in waterfall)
After: exchange parted_range ∩ order_type grouped_lookup → range ∩ set intersection

---

## 6. Performance Analysis

| Scenario | Before | After | Speedup |
|----------|--------|-------|---------|
| ts range + p# eq | O(log n + R) | O(log n + 1) | R/1 where R = rows in ts range |
| ts range + g# eq | O(log n + R) | O(log n + k) | R/k where k = matching rows |
| s# range + s# range | O(log n + R₁) | O(log n + log n) | R₁/1 (range overlap) |
| g# eq + g# eq | O(k₁) | O(min(k₁,k₂) × log max(k₁,k₂)) | depends on selectivity |
| Single predicate | O(index) | O(index) | 1x (no regression) |

Where R = rows after first index, k = rows matching additional predicate.

---

## 7. Parallel Path

The parallel execution paths (`exec_simple_select_parallel`, `exec_group_agg_parallel`) call the same `eval_where` / `eval_where_ranged` internally. The composite index change is in the serial per-partition scan logic, so parallel paths benefit automatically.

---

## 8. Files to Modify

| File | Change |
|------|--------|
| `include/zeptodb/sql/executor.h` | Add `IndexResult`, `extract_all_sorted_ranges`, `extract_all_index_eqs`, `eval_remaining_where` |
| `src/sql/executor.cpp` | Replace waterfall in `exec_simple_select` + `exec_agg` + `exec_group_agg` with collect-and-intersect |
| `docs/api/SQL_REFERENCE.md` | Document composite index behavior in Index Attributes section |
| `docs/devlog/067_composite_index.md` | Implementation devlog |
| `docs/COMPLETED.md` | Add entry when done |
| `docs/BACKLOG.md` | Mark composite index as done |
| `tests/unit/test_composite_index.cpp` | New test file |

---

## 9. Test Plan

### Unit Tests

| Test | Description |
|------|-------------|
| `IndexResult_RangeIntersect` | Two ranges → correct overlap |
| `IndexResult_RangeSetIntersect` | Range + g# set → filtered set |
| `IndexResult_SetSetIntersect` | Two g# sets → correct intersection |
| `IndexResult_EmptyIntersect` | Non-overlapping ranges → empty result |
| `IndexResult_SingleIndex` | Single predicate → same as current behavior |
| `CompositeIndex_TimestampPlusPart` | s# + p# end-to-end query |
| `CompositeIndex_TimestampPlusGroup` | s# + g# end-to-end query |
| `CompositeIndex_TwoSorted` | s# + s# range intersection |
| `CompositeIndex_ThreeWay` | s# + p# + g# triple intersection |
| `CompositeIndex_NoRegression` | Single-index queries unchanged |
| `CompositeIndex_GroupByPath` | GROUP BY with composite index |
| `CompositeIndex_ParallelPath` | Parallel execution with composite index |

### Edge Cases

- Empty partition
- No indexed columns in WHERE → falls back to full scan
- All predicates indexed → no remaining WHERE eval needed
- Intersection produces empty set → zero rows returned
- Single-row partition
- g# lookup returns empty → early exit

---

## 10. Non-Goals (Future Work)

- **Persistent composite index structure** — a B-tree on (col1, col2) would be more efficient for very high-cardinality multi-column lookups, but adds write-path complexity. Defer to cost-based planner era.
- **CREATE INDEX DDL** — explicit composite index creation syntax. Not needed for intersection approach.
- **Index advisor** — automatic recommendation of which columns to index. Separate feature.
- **Bloom filter pre-check** — for very large g# sets, a bloom filter could skip intersection early. Add if profiling shows need.
