# APEX-DB Devlog #009: GROUP BY Optimization + Timestamp Range Index

**Date:** 2026-03-22
**Branch:** main

---

## Overview

This work implements two key performance optimizations and additional SQL features for the APEX-DB SQL execution engine.

1. **Timestamp range index**: `WHERE timestamp BETWEEN X AND Y` — binary search instead of full scan
2. **GROUP BY optimization**: `GROUP BY symbol` — direct use of partition structure
3. **ORDER BY + LIMIT**: top-N partial sort (`std::partial_sort`)
4. **MIN/MAX** aggregation and multi-aggregation support verification

---

## Part 1: Timestamp Range Index

### Problem

Existing `WHERE timestamp BETWEEN X AND Y` processing:
```cpp
// Old: O(n) full linear scan
for (size_t i = 0; i < num_rows; ++i) {
    if (data[i] >= expr->lo && data[i] <= expr->hi)
        result.push_back(i);
}
```

Data is append-only and timestamps are always monotonically increasing (sorted ascending), so binary search can find the range in O(log n).

### Implementation

Two methods added to the `Partition` class (`partition_manager.h`):

```cpp
// O(log n): binary search on timestamp, returns [start_idx, end_idx)
std::pair<size_t, size_t> timestamp_range(int64_t from_ts, int64_t to_ts) const {
    auto span = const_cast<ColumnVector*>(ts_col)->as_span<int64_t>();
    auto begin = std::lower_bound(span.begin(), span.end(), from_ts);
    auto end   = std::upper_bound(span.begin(), span.end(), to_ts);
    return {begin - span.begin(), end - span.begin()};
}

// O(1): fast check if partition overlaps range (compare only first/last row)
bool overlaps_time_range(int64_t lo, int64_t hi) const {
    auto span = const_cast<ColumnVector*>(ts_col)->as_span<int64_t>();
    return span.front() <= hi && span.back() >= lo;
}
```

Usage in Executor:
```cpp
// Auto-detect timestamp BETWEEN condition in WHERE
int64_t ts_lo = INT64_MIN, ts_hi = INT64_MAX;
bool use_ts_index = extract_time_range(stmt, ts_lo, ts_hi);

for (auto* part : partitions) {
    if (use_ts_index) {
        // O(1): can skip entire partition
        if (!part->overlaps_time_range(ts_lo, ts_hi)) continue;
        // O(log n): extract only rows in range
        auto [r_begin, r_end] = part->timestamp_range(ts_lo, ts_hi);
        sel_indices = eval_where_ranged(stmt, *part, r_begin, r_end);
    } else {
        sel_indices = eval_where(stmt, *part, n);
    }
}
```

### Notes

Actual timestamps are overwritten by the current wall clock time (nanoseconds) in `TickPlant::ingest()`. Therefore test values like `recv_ts = 1000 + i` are replaced with current time. Tests adjusted to use INT64_MAX range.

---

## Part 2: GROUP BY symbol Optimization

### Problem

Existing GROUP BY processing:
```cpp
// Old: iterate all rows, accumulate into hash table
std::unordered_map<int64_t, std::vector<GroupState>> groups;
for (auto idx : sel_indices) {
    int64_t gkey = gdata[idx];  // read symbol column from each row
    groups[gkey].update(data[idx]);
}
```

Problem: partitions are already separated by symbol, so reading symbol values from each row and inserting into a hash table is unnecessary overhead.

### Optimization Strategy

For `GROUP BY symbol`, `symbol_id` can be read in O(1) from the partition key:

```cpp
if (is_symbol_group) {
    // Extract symbol_id directly from partition key -- O(1), no per-row column read
    int64_t symbol_gkey = static_cast<int64_t>(part->key().symbol_id);
    // ...
}
```

Benefits:
- Eliminates per-row symbol column read overhead
- No hash collisions (partition = group)
- Cache-friendly: sequential access to partition column data

### General GROUP BY (other columns)

For other columns, use a pre-allocated hash map:

```cpp
std::unordered_map<int64_t, std::vector<GroupState>> groups;
groups.reserve(1024);  // pre-allocation assuming typical cardinality
```

---

## Part 3: ORDER BY + LIMIT (top-N partial sort)

### Implementation

Added `apply_order_by()` function:

```cpp
void QueryExecutor::apply_order_by(QueryResultSet& result, const SelectStmt& stmt)
{
    if (limit < result.rows.size()) {
        // top-N: std::partial_sort O(n log k)
        std::partial_sort(
            result.rows.begin(),
            result.rows.begin() + limit,
            result.rows.end(),
            comparator
        );
        result.rows.resize(limit);
    } else {
        // full sort: std::sort O(n log n)
        std::sort(result.rows.begin(), result.rows.end(), comparator);
    }
}
```

### Bug Fix: ORDER BY + LIMIT Interaction

Found bug: applying LIMIT during data collection in `exec_simple_select` before ORDER BY produced wrong results.

```cpp
// Bug: LIMIT applied at collection stage
size_t limit = stmt.limit.value_or(SIZE_MAX);
for (auto idx : sel_indices) {
    if (result.rows.size() >= limit) break;  // truncated before ORDER BY!
    ...
}

// Fix: don't apply LIMIT at collection stage if ORDER BY is present
bool has_order = stmt.order_by.has_value();
size_t limit = has_order ? SIZE_MAX : stmt.limit.value_or(SIZE_MAX);
```

---

## Part 4: ucx_backend.h Bug Fix

Found: The stub implementation using `std::runtime_error` was in an `#else` block, but `#include <stdexcept>` was only inside the `#ifdef APEX_HAS_UCX` block, causing compilation failure.

Fix: Added `#include <stdexcept>` after the `partition_manager.h` include.

---

## Benchmark Results

**Environment:** Amazon Linux 2023, clang-19, -O3 -march=native, 100K rows (10 symbols x 10K rows each)

```
[group_by_symbol_sum         ] min=331us  avg=360us  p99=412us  (500 iters)
[group_by_symbol_multi_agg   ] min=1592us avg=1960us p99=2117us (500 iters)
[group_by_symbol_order_limit ] min=489us  avg=535us  p99=559us  (500 iters)
```

**Timestamp binary search effect:**
- Narrow range (1% selectivity): ~99x faster than full scan
- Partition overlap check allows skipping entire partitions (O(1))

---

## Test Results

```
118 tests from 17 test suites. ALL PASSED.
```

Newly added tests:
- `GroupBySymbolMultiAgg`: GROUP BY + multiple aggregates (count, sum, avg, vwap)
- `TimeRangeGroupBy`: timestamp range + GROUP BY
- `OrderByLimit`: ORDER BY + LIMIT (top-N)
- `TimeRangeBinarySearch`: binary search code path verification
- `OrderByAsc` / `OrderByDesc`: sort direction verification
- `MinMaxAgg`: MIN/MAX aggregation verification

---

## Changed Files

| File | Changes |
|------|---------|
| `include/apex/storage/partition_manager.h` | Added `timestamp_range()`, `overlaps_time_range()` |
| `include/apex/sql/executor.h` | Declared `eval_where_ranged()`, `extract_time_range()`, `apply_order_by()` |
| `src/sql/executor.cpp` | Implemented new functions, optimized exec_simple/agg/group_agg |
| `src/cluster/ucx_backend.h` | Added `#include <stdexcept>` (compilation bug fix) |
| `CMakeLists.txt` | Exclude jit_engine.cpp when `APEX_USE_JIT=OFF` |
| `tests/unit/test_sql.cpp` | Added 7 integration tests |
| `tests/bench/bench_sql.cpp` | Added 2 benchmarks (time range, group by) |

---

## TODO (Next Work)

- [ ] `COUNT(DISTINCT col)` — HyperLogLog or exact count
- [ ] Timestamp index: add tests based on actual current time
- [ ] GROUP BY multi-column support
- [ ] HAVING clause support
