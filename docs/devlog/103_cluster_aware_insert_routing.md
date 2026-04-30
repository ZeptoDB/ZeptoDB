# Devlog 103 — Cluster-aware INSERT routing

**Date:** 2026-04-26
**Scope:** Correctness fix — HTTP/SQL/Python `INSERT` routing
**Status:** ✅ Done

## Problem

Before this change, `QueryExecutor::exec_insert()` wrote every SQL `INSERT`
tick directly to the local `ZeptoPipeline` on whichever pod received the
HTTP request:

```cpp
// src/sql/executor.cpp:740 (before)
pipeline_.ingest_tick(msg);
```

`QueryExecutor` held only a `ZeptoPipeline&` and had no awareness of cluster
topology. As a result, in a multi-pod deployment, writes were silently
mis-partitioned: the same symbol could land on different pods depending on
which one received the HTTP request, breaking the one-owner-per-partition
invariant that the query path relies on.

The correct path was already implemented — `ClusterNode::ingest_tick` routes
via `PartitionRouter` and forwards remote ticks over `TcpRpcClient::ingest_tick`
(`include/zeptodb/cluster/cluster_node.h:264-294`). It was unused by the
HTTP/SQL/Python INSERT path. Feed consumers (`KafkaConsumer`, `MqttConsumer`,
`OpcUaConsumer`) had already been wired via `set_routing()` but the
HTTP/Python write paths had not.

### Measured impact

With the bug, horizontal ingest did not scale past a single pod: the shared
pipeline saturated at ~4M tps regardless of replica count. With correct
routing (simulated via independent pipelines), 2 pods scaled to ~2.9× — the
expected near-linear speed-up minus cross-pod network cost.

## Fix

### 1. Non-template base class

`ClusterNode<Transport>` is a class template, so non-cluster headers
(`sql/executor.h`, `transpiler/python_binding.cpp`) could not hold a pointer
to it without being templated themselves. We extracted a minimal abstract
base:

```cpp
// include/zeptodb/cluster/cluster_node_base.h (new)
class ClusterNodeBase {
public:
    virtual ~ClusterNodeBase() = default;
    virtual bool ingest_tick(zeptodb::ingestion::TickMessage msg) = 0;
};
```

`ClusterNode<Transport>` now inherits from `ClusterNodeBase` and marks its
existing `ingest_tick` with `override`. The body is unchanged.

### 2. `QueryExecutor::set_cluster_node()`

Added a single setter and a single pointer member. Kept every existing
constructor signature unchanged to avoid combinatorial overload explosion.

```cpp
void set_cluster_node(zeptodb::cluster::ClusterNodeBase* node) {
    cluster_node_ = node;
}
```

### 3. Branch in `exec_insert`

The hot line at `src/sql/executor.cpp:740`:

```cpp
// Before
pipeline_.ingest_tick(msg);

// After
if (cluster_node_) {
    cluster_node_->ingest_tick(msg);   // route to partition owner
} else {
    pipeline_.ingest_tick(msg);        // single-node fallback
}
```

`UPDATE` and `DELETE` do not go through `ingest_tick`; they modify rows
in-place via `as_span()`. No change needed there.

### 4. Python bindings

`src/transpiler/python_binding.cpp` had three call sites calling
`pipeline_->ingest_tick(msg)` (single ingest, int batch, float batch).
All three now call a private helper `ingest_routed_()` that performs the
same conditional dispatch. Exposing the `set_cluster_node` hook to Python
requires pybind11 plumbing beyond scope; the C++-side plumbing is in place
so whoever wires cluster mode to the Python layer can enable it with a
one-line binding. Tracked as a small follow-up.

### 5. `zepto_http_server`

At the time of devlog 103, this did not construct a `ClusterNode<Transport>`
and the `TODO` comment at `tools/zepto_http_server.cpp:297-304` held the
future wire-up site. **Landed in devlog 111 (2026-04-30) via a
non-template `CoordinatorRoutingAdapter`** that reuses the existing
`QueryCoordinator`'s `PartitionRouter` + a peer `TcpRpcClient` pool —
no duplicate pipeline, no full `ClusterNode<T>` construction. See
`docs/devlog/111_http_server_cluster_routing.md` and the Round 2 EKS
verification in `docs/bench/results_multinode.md`.

## Backward compatibility

`cluster_node_` defaults to `nullptr`. Every existing call site and every
existing test that constructs a `QueryExecutor` without calling
`set_cluster_node()` behaves identically to before this change (verified:
full suite 1262 → 1266, no regressions; 4 new tests).

## Tests

New file `tests/unit/test_distributed_insert.cpp`:

| Scenario | Covered |
|---|---|
| (a) INSERT routes to remote owner (2-node TCP RPC round trip) | ✅ `RoutesToRemoteOwner` |
| (b) INSERT lands locally when this node owns the symbol | ✅ `OwnerIsSelf_IngestsLocally` |
| (c) Backward compat: no `set_cluster_node()` → direct to pipeline | ✅ `NoClusterNode_FallsBackToPipeline` |
| (d) Explicit `set_cluster_node(nullptr)` reverts to fallback | ✅ `NullAfterSet_RevertsToPipeline` |

Scenario (a) uses a real two-node `ClusterNode<SharedMemBackend>` pair with
`enable_remote_ingest=true`; the tick is dispatched by node 1 over
`TcpRpcClient::ingest_tick` to node 2's `TcpRpcServer`, which delivers it to
node 2's local `ZeptoPipeline`. We verify `ticks_ingested` goes up on
node 2 and stays flat on node 1.

Full suite: **1266 passed / 1266** (was 1262 / 1262 pre-change).

## Files changed

| File | Change |
|---|---|
| `include/zeptodb/cluster/cluster_node_base.h` | **new** — non-template routing base |
| `include/zeptodb/cluster/cluster_node.h` | `: public ClusterNodeBase` + `override` on `ingest_tick` |
| `include/zeptodb/sql/executor.h` | forward decl + `cluster_node_` member + `set_cluster_node()` |
| `src/sql/executor.cpp` | 1-line branch in `exec_insert`; `#include` of base header |
| `src/transpiler/python_binding.cpp` | `ingest_routed_()` helper + 3 call sites rewrapped + member |
| `tools/zepto_http_server.cpp` | TODO comment at the future wire-up point (replaced with the actual wire-up in devlog 111) |
| `tests/unit/test_distributed_insert.cpp` | **new** — 4 correctness tests |
| `tests/CMakeLists.txt` | register new test file |

Diff is intentionally small (≈80 lines production code, ≈220 lines tests +
docs). No existing signatures changed; `ingest_tick` got an `override`
decorator only.

## Related

- devlog 102 — Phase 1 ingest scale (single-pod, drain-thread lift). This
  devlog closes the Phase 2 correctness gap that was blocking horizontal
  scale-out.
- BACKLOG P8-I3 — Stateless `zepto_ingest_node` binary. With this fix, that
  work becomes a matter of wiring `ClusterNode` into a new binary; the
  ingest routing is already correct.
