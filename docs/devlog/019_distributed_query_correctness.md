# Devlog 019: Distributed Query Correctness — VWAP & ORDER BY

**Date:** 2026-03-23
**Phase:** Distributed Query Correctness

---

## Motivation

After the cluster integrity work (devlog 018), a review of distributed query
capabilities revealed 13 gaps across query correctness, infrastructure safety,
and numeric precision. This devlog covers the first two fixes.

---

## Changes

### 1. VWAP Distributed Decomposition

**Problem:** `VWAP(price, volume)` = `SUM(price*volume)/SUM(volume)`. When scattered
to multiple nodes, each node computes a local VWAP. Naively averaging local VWAPs
gives wrong results (e.g., 150 instead of correct 157).

**Solution:** Same pattern as AVG rewrite. In `build_avg_rewrite()`:
- `VWAP(price, vol)` → `SUM(price * vol)`, `SUM(vol)` in the scatter SQL
- Coordinator reconstructs: `SUM_PV / SUM_V`

Extended `AvgColInfo` with `is_vwap` flag for future float precision handling.

**Verification:**
- Node 1: 3 ticks, price=100, vol=10 → local SUM(p*v)=3000, SUM(v)=30
- Node 2: 2 ticks, price=200, vol=20 → local SUM(p*v)=8000, SUM(v)=40
- Correct: (3000+8000)/(30+40) = 11000/70 = 157
- Wrong (avg of local VWAPs): (100+200)/2 = 150

### 2. ORDER BY + LIMIT Post-Merge

**Problem:** Scatter-gather returned concatenated results with no ordering or
truncation. Each node applied ORDER BY/LIMIT locally, but the merged result
was unsorted and untruncated.

**Solution:** After all merge strategies (CONCAT, SCALAR_AGG, MERGE_GROUP_BY),
parse the original SQL for ORDER BY and LIMIT clauses, then:
1. `std::sort` on the sort column (first ORDER BY item)
2. `resize()` to LIMIT

This is applied in `execute_sql()` as a final post-processing step.

---

## Remaining Gaps (Full Inventory)

### Query Level
| Gap | Difficulty | Status |
|-----|-----------|--------|
| HAVING distributed | Medium | ✅ Strip from scatter, apply post-merge |
| DISTINCT distributed | Easy | ✅ `std::set` dedup at coordinator |
| Window functions distributed | Hard | ✅ Fetch-and-compute via temp pipeline |
| FIRST/LAST distributed | Medium | ✅ Fetch-and-compute + `store_tick_direct()` |
| COUNT(DISTINCT) distributed | Hard | ✅ Parser + executor + fetch-and-compute |
| Subquery/CTE distributed | Hard | ✅ Detect CTE/subquery → fetch-and-compute |
| Multi-column ORDER BY | Easy | ✅ Composite key sort in post-merge |

### Infrastructure Level
| Gap | Difficulty | Impact |
|-----|-----------|--------|
| Cancel propagation | Medium | Coordinator timeout → cancel RPC to nodes |
| Partial failure policy | Medium | Some nodes fail: partial result vs error |
| In-flight query safety | Hard | Node add/remove during scatter → race |
| Dual-write during migration | Hard | Data loss gap during partition move |
| Distributed query timeout | Medium | Remote-side timeout enforcement |

### Precision
| Gap | Difficulty | Impact |
|-----|-----------|--------|
| AVG int64 truncation | Medium | Float AVG loses precision |
| VWAP int64 overflow | Medium | SUM(price*volume) can overflow |

---

## Test Summary

| Test | Result |
|------|--------|
| QueryCoordinator.TwoNodeRemote_DistributedVwap | ✅ VWAP=157 (correct) |
| QueryCoordinator.TwoNodeRemote_OrderByLimit | ✅ DESC [500,400,300], ASC [100,200] |
| QueryCoordinator.TwoNodeRemote_DistributedHaving | ✅ Post-merge HAVING filter |
| QueryCoordinator.TwoNodeRemote_DistributedDistinct | ✅ Cross-node dedup (4 unique) |
| QueryCoordinator.TwoNodeRemote_DistributedWindowFunction | ✅ Cross-node LAG |
| QueryCoordinator.TwoNodeRemote_DistributedFirstLast | ✅ FIRST=100, LAST=500 |
| QueryCoordinator.TwoNodeRemote_DistributedCountDistinct | ✅ COUNT(DISTINCT)=4 |
| QueryCoordinator.TwoNodeRemote_DistributedCTE | ✅ CTE on full dataset |
| QueryCoordinator.TwoNodeRemote_MultiColumnOrderBy | ✅ Composite sort |

**Total tests: 607 passing (all suites)**

---

## Files Changed

| File | Change |
|------|--------|
| `src/cluster/query_coordinator.cpp` | VWAP rewrite in `build_avg_rewrite()`; ORDER BY + LIMIT post-merge |
| `tests/unit/test_coordinator.cpp` | 2 new tests |
| `BACKLOG.md` | Full distributed gap inventory (query/infra/precision) |

*Last updated: 2026-03-23*
