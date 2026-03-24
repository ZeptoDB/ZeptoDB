# Index Attribute Benchmark Results (g# / p#)
# Run date: 2026-03-24
# Environment: Intel Xeon 6975P-C, 8 vCPU, 30GB RAM, Clang 19 Release -O3 -march=native

---

## Summary

| Index Type | Latency (1M rows) | vs Full Scan | Complexity |
|---|---|---|---|
| No index (full scan) | **904μs** | baseline | O(n) |
| **g# (grouped/hash)** | **3.3μs** | **274x faster** | O(1) lookup |
| **p# (parted)** | **3.4μs** | **269x faster** | O(1) range |
| s# (sorted) | ~10μs | ~90x faster | O(log n) binary search |

---

## Test Setup

- 1M rows, single symbol (single partition, no partition pruning)
- Query: `SELECT count(*) FROM trades WHERE symbol = 1 AND price = 15500`
- price column: 1000 distinct values (15000–15999), uniform distribution
- g# index: hash map from price → row indices
- p# index: contiguous range per distinct price value
- 500 iterations, warmup 3 runs

## Detailed Results

```
[filter_eq_no_index_1M] min=838.28μs avg=903.74μs p99=969.94μs max=986.65μs
[filter_eq_g#_index_1M] min=  3.20μs avg=  3.31μs p99=  3.59μs max=  3.73μs
[filter_eq_p#_index_1M] min=  3.21μs avg=  3.36μs p99=  3.72μs max= 14.99μs
```

## Analysis

- **g# (hash index)**: O(1) hash lookup returns exactly the matching row indices. No scanning at all. 3.3μs includes SQL parse (~1.5μs) + hash lookup + count aggregation.
- **p# (parted index)**: O(1) lookup returns [begin, end) range for the value. Same performance as g# because the range is tight (1000 rows out of 1M).
- **Full scan**: Must evaluate WHERE condition on all 1M rows even though only ~1000 match.

## When to Use Each

| Attribute | Best For | Data Requirement |
|---|---|---|
| **s#** (sorted) | Range queries (BETWEEN, >, <) | Column must be monotonically sorted |
| **g#** (grouped) | Equality queries (= X) on high-cardinality columns | Any data order |
| **p#** (parted) | Equality queries on low-cardinality clustered columns | Same values must be contiguous |

## SQL Usage

```sql
-- Set attributes
ALTER TABLE trades SET ATTRIBUTE price GROUPED;    -- g# hash index
ALTER TABLE trades SET ATTRIBUTE exchange PARTED;   -- p# parted index
ALTER TABLE trades SET ATTRIBUTE timestamp SORTED;  -- s# sorted index
```
