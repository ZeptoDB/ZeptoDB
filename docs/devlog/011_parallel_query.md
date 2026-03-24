# devlog #011 — Parallel Query Engine: QueryScheduler Abstraction + Multi-Node Expansion Path

**Date:** 2026-03-22
**Work:** Parallel query execution engine + QueryScheduler DI pattern

---

## Background and Goals

> "The goal is not to implement multi-node right now,
>  but to set up a structure where nodes can be added later
>  without modifying QueryExecutor code."

The existing `QueryExecutor` had only a serial path. Multi-core aggregation is clearly needed, but to avoid a complete rewrite when expanding to distributed execution later, we need to abstract **where execution happens**.

---

## Design: scatter/gather + DI Pattern

### QueryScheduler Interface

```
QueryExecutor
    |
    +-> QueryScheduler (abstract)
            ├── LocalQueryScheduler    <- implemented today
            └── DistributedQueryScheduler <- stub (UCX-based, future)
```

```cpp
class QueryScheduler {
    virtual vector<PartialAggResult> scatter(const vector<QueryFragment>&) = 0;
    virtual PartialAggResult         gather(vector<PartialAggResult>&&)    = 0;
    virtual size_t      worker_count()   const = 0;
    virtual string      scheduler_type() const = 0;
};
```

- **scatter**: distribute work units (Fragments) to workers
- **gather**: merge partial results
- QueryExecutor only sees the interface and doesn't know where execution happens

### Dependency Injection

```cpp
// Default: LocalQueryScheduler auto-created
QueryExecutor ex(pipeline);

// Testing/distributed: inject external scheduler
QueryExecutor ex(pipeline, std::make_unique<MockScheduler>());
```

`pool_raw_` raw pointer pattern: internal parallel paths (`exec_agg_parallel` etc.)
must call `QueryExecutor` private methods (`eval_where`, `get_col_data`),
so a `WorkerPool*` non-owning pointer is used instead of full scatter/gather separation.
Falls back to serial when non-local scheduler is injected (`nullptr`).

---

## Implementation Files

| File | Role |
|------|------|
| `include/zeptodb/execution/query_scheduler.h` | QueryFragment + PartialAggResult + interface |
| `include/zeptodb/execution/local_scheduler.h` | LocalQueryScheduler declaration |
| `src/execution/local_scheduler.cpp` | scatter/gather/execute_fragment implementation |
| `include/zeptodb/execution/distributed_scheduler.h` | Distributed scheduler stub |
| `src/execution/distributed_scheduler.cpp` | stub |
| `include/zeptodb/execution/parallel_scan.h` | ParallelScanExecutor (partition chunk distribution) |
| `src/execution/parallel_scan.cpp` | make_partition_chunks / select_mode |
| `src/sql/executor.cpp` | parallel path + DI constructor |

### PartialAggResult: Recursive Type Problem

To distribute GROUP BY aggregation, partial results need a `group_key -> partial_agg` map.
The map's value type is `PartialAggResult` itself, causing an incomplete type compile error.

```cpp
// Error: std::unordered_map<int64_t, PartialAggResult> (incomplete type)
// Fix:
std::unordered_map<int64_t, std::shared_ptr<PartialAggResult>> group_partials;
```

---

## Parallelization Strategy (ParallelScanExecutor::select_mode)

```
total_rows < threshold(100K)      -> SERIAL   (thread overhead > benefit)
num_partitions >= num_threads     -> PARTITION (distribute by partition)
otherwise                         -> CHUNKED   (split rows within single partition)
```

If `num_threads <= 1`, always SERIAL. Without checking this condition at
`exec_agg`/`exec_group_agg` entry, `exec_group_agg` -> `exec_group_agg_parallel` -> SERIAL -> `exec_group_agg` infinite recursion occurs.

**Bug fix**: Added `pool_raw_->num_threads() > 1` to parallel entry condition.

---

## Benchmark Results (1M rows, 2 symbols)

| Query | 1T | 2T | 4T | 8T |
|-------|-----|-----|-----|-----|
| GROUP BY symbol sum(volume) | 0.862ms | 0.460ms (1.87x) | 0.398ms (2.16x) | 0.248ms (3.48x) |
| sum(volume) WHERE symbol=1 | 0.006ms | — | — | — |
| count(*) | 0.004ms | — | — | — |

- Single partition queries (WHERE symbol=X) auto-fall through to serial path because `partitions.size() < 2`
- Meaningful multi-core acceleration confirmed for GROUP BY (all partitions)
- Direct scatter/gather API call: avg 0.033ms/round (10-round average)

---

## Tests (27 added)

- Parts 1-4: WorkerPool basics, ParallelScan utils, parallel aggregation correctness, multi-thread count correctness
- Part 5: Serial fallback (small data)
- Part 6: QueryScheduler DI tests (MockScheduler injection, type check, direct scatter/gather calls)

---

## Single-Node to Multi-Node Expansion Path

```
Current (single node)
  QueryExecutor
    +-> LocalQueryScheduler
           +-> WorkerPool (jthread, N cores)

Future (multi-node, UCX)
  QueryExecutor           <- no code changes
    +-> DistributedQueryScheduler
           ├-> UCX transport (node A)
           ├-> UCX transport (node B)
           └-> ...
           scatter: serialize PartialAggResult as Fragment to each node via UCX
           gather:  merge each node result via PartialAggResult::merge()
```

`PartialAggResult::serialize()` / `deserialize()` stubs will be filled when implementing the distributed scheduler.
UCX backend (`src/cluster/ucx_backend.cpp`) is already implemented from Phase C.

---

## Next Steps

- [ ] Implement DistributedQueryScheduler on UCX transport
- [ ] PartialAggResult FlatBuffers serialization
- [ ] Activate CHUNKED mode (split rows of single large partition)
- [ ] Parallelize exec_simple_select (currently only aggregation is parallelized)
