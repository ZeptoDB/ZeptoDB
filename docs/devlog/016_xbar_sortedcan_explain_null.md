# Devlog 016: XBAR Sorted-Scan, EXPLAIN Statement, NULL Standardization

**Date:** 2026-03-22
**Status:** Completed

---

## Summary

Three improvements completed in this session:

1. **XBAR Sorted-Scan** — eliminate hash lookups for monotonic GROUP BY XBAR queries
2. **EXPLAIN statement** — `EXPLAIN SELECT ...` returns a human-readable execution plan
3. **NULL standardization** — consistent INT64_MIN sentinel for empty aggregates across all paths

Fresh PGO6 profile collected after all changes. Final Xbar benchmark: **10.25ms** (down from 11.62ms).

---

## 1. XBAR Sorted-Scan

### Problem

`exec_group_agg` single-column path (added in devlog 015) uses `unordered_map<int64_t, uint32_t> key_to_slot`. For GROUP BY XBAR (time bars), timestamps within each partition are strictly monotonically increasing, so consecutive rows land in the same XBAR bucket ~99.7% of the time (e.g. 1M rows → 3,334 unique buckets).

Despite the flat GroupState layout, each row still required a full hash lookup.

### Fix

Added `cached_key`/`cached_slot` variables initialized before the partition loop:

```cpp
int64_t  cached_key  = INT64_MIN;
uint32_t cached_slot = 0;
```

In the hot row loop:

```cpp
if (__builtin_expect(kv != cached_key, 0)) {
    auto it = key_to_slot.find(kv);
    if (__builtin_expect(it == key_to_slot.end(), 0)) {
        it = key_to_slot.emplace(kv, next_slot++).first;
        flat_states.resize(flat_states.size() + ncols);
    }
    cached_key  = kv;
    cached_slot = it->second;
}
GroupState* states = flat_states.data() + cached_slot * ncols;
```

The `__builtin_expect(..., 0)` hints guide branch prediction for the common fast path (key unchanged).

This reduces hash operations from ~1M (one per row) to ~3,334 (one per bucket boundary) — effectively O(N_buckets) instead of O(N_rows).

The same cache was applied to the parallel path's per-thread `PartialGroupScalar` struct.

### Result

| Config | Xbar 1M rows |
|--------|-------------|
| Before flat GroupState (devlog 015) | 45.2ms |
| After flat GroupState (devlog 015) | 11.62ms |
| After sorted-scan + PGO6 | **10.25ms** |

---

## 2. EXPLAIN Statement

### Implementation

**Tokenizer** (`include/zeptodb/sql/tokenizer.h`, `src/sql/tokenizer.cpp`):
- Added `TokenType::EXPLAIN` keyword

**AST** (`include/zeptodb/sql/ast.h`):
- Added `bool explain = false` flag to `SelectStmt`

**Parser** (`src/sql/parser.cpp`):
- Detects `EXPLAIN` prefix before parsing WITH/SELECT; sets `stmt.explain = true`

**Executor** (`src/sql/executor.cpp`):
- `build_explain_plan(stmt, pipeline)` static function builds plan lines
- Called from `execute()` before any actual query runs (when `stmt.explain == true`)
- Returns `QueryResultSet` with `column_names = {"plan"}`, plan lines in `string_rows`

### Plan output format

```
SELECT count(*) FROM trades
→ Operation: Aggregate
→ Table: trades  Partitions: 2  EstimatedRows: 20000
→ GroupBy: (none)
→ Path: serial

EXPLAIN SELECT sum(volume) FROM trades GROUP BY XBAR(recv_ts, 300000000000) AS bar
→ Operation: GroupAggregate
→ Table: trades  Partitions: 2  EstimatedRows: 20000
→ GroupBy: XBAR(300000000000)
→ Path: serial
```

---

## 3. NULL Standardization

### Problem

Different executor paths were inconsistent about what to return for an empty aggregate:
- `exec_group_agg` paths (all GROUP BY variants): returned `INT64_MIN` ✓
- `exec_agg` (scalar aggregate, serial): returned `0` for MIN, MAX, AVG, VWAP ✗
- `exec_agg_parallel` (scalar aggregate, parallel): returned `0` for MIN, MAX, AVG ✗

ZeptoDB uses `INT64_MIN` as the universal NULL sentinel throughout the column store.

### Fix

Updated `exec_agg` (serial) and `exec_agg_parallel` result output:

| Aggregate | Empty before | Empty after |
|-----------|-------------|-------------|
| MIN (no rows) | 0 | INT64_MIN |
| MAX (no rows) | 0 | INT64_MIN |
| AVG (count=0) | 0 | INT64_MIN |
| VWAP (vol=0) | 0 | INT64_MIN |

COUNT, SUM, FIRST, LAST, XBAR: unchanged (0 is correct for these).

---

## 4. Fresh PGO6 Profile

After all code changes, collected a new PGO profile (replacing the stale PGO5):

```bash
# Instrumentation build
cmake . -DCMAKE_CXX_FLAGS="-O3 -march=native -fprofile-generate=/tmp/zepto_pgo6" ...
./tests/zepto_tests --gtest_filter="Benchmark.*:SqlExecutor*:FinancialFunction*:WindowJoin*"

# Optimization build
cmake . -DCMAKE_CXX_FLAGS="-O3 -march=native -flto -fprofile-use=/tmp/zepto_pgo6 -fprofile-correction" \
        -DCMAKE_EXE_LINKER_FLAGS="-flto" -DAPEX_USE_TCMALLOC=ON
```

---

## Final Benchmark Results (LTO + PGO6 + tcmalloc + hugepages)

| Benchmark | Time |
|-----------|------|
| Xbar GROUP BY 1M rows | **10.25ms** |
| EMA 1M rows | **2.22ms** |
| Window JOIN 100K×100K | **11.6ms** |

Test suite: **428/429 pass** (only pre-existing `ClusterNode.TwoNodeLocalCluster` flaky).

---

## Lessons Learned

- For time-series GROUP BY, the key observation is that partitions store timestamps in order. A single `cached_key` variable captures this monotonicity with zero overhead for the common case.
- `__builtin_expect(cond, 0)` on the "new group encountered" branch is important: the branch predictor sees this as rarely-taken, keeping the fast path prediction accurate.
- PGO profiles become stale after hot-path code changes — always re-collect after significant executor modifications.

---

## Next Steps

- [ ] RIGHT JOIN / FULL OUTER JOIN (SQL completeness)
- [ ] SUBSTR / string manipulation (requires string column type)
- [ ] Graviton (ARM/SVE) build test
