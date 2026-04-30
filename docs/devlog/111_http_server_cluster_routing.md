# Devlog 111 — HTTP server cluster-aware INSERT routing (P8-I3-wire)

Date: 2026-04-30
Scope: `tools/zepto_http_server.cpp`, `include/zeptodb/cluster/coordinator_routing_adapter.h`, `include/zeptodb/cluster/query_coordinator.h`, `tests/unit/test_coordinator_routing_adapter.cpp`, `tools/zepto_data_node.cpp`

---

## Problem

Devlog 103 (Sprint 1 of the horizontal-scaling work) shipped the library-level fix for cluster-aware `INSERT`:

- `ClusterNodeBase` abstract class
- `QueryExecutor::set_cluster_node()` setter + a routing branch in `exec_insert`
- 4 passing unit tests in `test_distributed_insert.cpp`

But the production binary `zepto_http_server` **never called `set_cluster_node()`**. The TODO comment at line 297-304 acknowledged the gap: "wire this when the HTTP server is connected to cluster mode."

The consequence was confirmed by the 2026-04-28 EKS multinode benchmark (`docs/bench/results_multinode.md`):

- amd64 N=1/2/3 ingest: **379 / 381 / 341 ticks/sec** (flat — no scaling)
- arm64 N=1/2/3 ingest: **460 / 461 / 448 ticks/sec** (flat)
- Per-pod inspection: **all writes landed on whichever pod the K8s Service LB picked**, not distributed by `PartitionRouter`
- `SELECT count(*)` returns the count from the LB-picked pod only; pods that didn't receive writes show zero rows

The scaling story built over three sprints (devlogs 102/103/104) depended on this final wire-up.

## Fix

### 1. `CoordinatorRoutingAdapter` — non-template `ClusterNodeBase` implementation

Rather than build a full `ClusterNode<TcpBackend>` (which would duplicate the `ZeptoPipeline` owned by `zepto_http_server`), the adapter reuses the pieces that already exist in the binary:

```cpp
// include/zeptodb/cluster/coordinator_routing_adapter.h
class CoordinatorRoutingAdapter : public ClusterNodeBase {
    PartitionRouter* router_;              // coordinator->router()
    std::shared_mutex* router_mu_;         // coordinator->router_mutex()
    core::ZeptoPipeline* local_;           // the binary's own pipeline
    NodeId self_id_;
    const RpcClientMap* remote_;           // peer TcpRpcClients keyed by NodeId

    bool ingest_tick(TickMessage msg) override {
        NodeId owner;
        { std::shared_lock lk(*router_mu_); owner = router_->route(msg.table_id, msg.symbol_id); }
        if (owner == self_id_) return local_->ingest_tick(msg);
        auto it = remote_->find(owner);
        if (it == remote_->end()) return false;
        return it->second->ingest_tick(msg);
    }
};
```

Lifetime contract: the adapter owns nothing; all four pointer-held resources must outlive it. The caller (main()) enforces this by declaring them in the same scope and destroying them in reverse order on shutdown.

### 2. Router unification (the subtle part)

The previous code had **two separate `PartitionRouter` instances**: one owned by `QueryCoordinator` (for scatter-gather SELECTs) and a second `rebalance_router` local to the main frame (for the rebalance manager). If the adapter had been pointed at `coordinator->router()`, a `RebalanceManager::add_node()` call would have mutated the rebalance-local ring while the adapter kept reading the coordinator's stale copy — ring view would never converge.

**Decision: Hypothesis A — delete `rebalance_router`, pass `coordinator->router()` to `RebalanceManager`.**

`RebalanceManager` takes `PartitionRouter&` by reference and mutates it in place. Since `QueryCoordinator::add_local_node` / `add_remote_node` already register every node in `router()` (verified in `src/cluster/query_coordinator.cpp:35, 52`), the router has the correct initial state. After this change, the adapter, the coordinator's query router, and the rebalance manager all read/write the same `PartitionRouter` instance under the same `shared_mutex`.

### 3. Peer RPC server for non-HA cluster mode

HA mode already starts a `TcpRpcServer` for peer SQL forwarding. Non-HA cluster mode didn't. The adapter routing creates remote dispatches (one pod forwards a tick to another pod's owner); those arrive at the owner's `port + 100` and need a server to accept them. Added a symmetric `TcpRpcServer::start(port+100, sql_cb, tick_cb)` in the non-HA cluster branch. `tick_cb` lands directly in the local pipeline (we are the owner by definition at this point).

### 4. Wire-up in `zepto_http_server.cpp::main()`

```cpp
if (!remote_nodes.empty() && coordinator) {
    // migrator ring (separate: holds host:port for physical data moves)
    rebalance_migrator = make_unique<PartitionMigrator>();
    rebalance_migrator->add_node(node_id, "127.0.0.1", port+100);
    for (rn : remote_nodes) rebalance_migrator->add_node(rn.id, rn.host, rn.port+100);

    // rebalance manager reads/writes coordinator->router(), not a separate copy
    rebalance_mgr = make_unique<RebalanceManager>(
        coordinator->router(), *rebalance_migrator);

    // peer RPC clients
    for (rn : remote_nodes) peer_rpc.emplace(rn.id,
        make_shared<TcpRpcClient>(rn.host, rn.port+100, 2000));

    // peer RPC server
    rpc_srv = make_unique<TcpRpcServer>();
    rpc_srv->start(port+100, sql_cb, tick_cb);

    // the actual wire-up
    routing_adapter = make_unique<CoordinatorRoutingAdapter>(
        &coordinator->router(), &coordinator->router_mutex(),
        &pipeline, node_id, &peer_rpc);
    executor.set_cluster_node(routing_adapter.get());
}
```

Startup logs in cluster mode now emit:
- `Rebalance manager: enabled (N nodes)`
- `Peer RPC server: port <N+100>`
- `Cluster routing: enabled (N remote nodes)`

## `zepto_data_node` — no wire-up needed

`zepto_data_node` is a leaf binary: it accepts SQL via `TcpRpcServer` only, never directly from clients, and every SQL it receives has already been routed by an upstream coordinator. Wiring `set_cluster_node()` here would introduce a double-route. A 3-line comment at the top of its `main()` documents the decision so future readers don't mistake the absence for oversight.

## Tests

`tests/unit/test_coordinator_routing_adapter.cpp` — 4 Google Tests, all pass:

| # | Test | Coverage |
|---|------|----------|
| 1 | `RoutesToLocalWhenOwnerIsSelf` | Single-node ring; adapter hits the local pipeline branch |
| 2 | `RoutesToRemoteWhenOwnerIsDifferent` | Two-node ring; adapter finds a remotely-owned symbol and forwards to stub `CountingRpcClient` |
| 3 | `DropsOnUnknownOwner` | Two-node ring where one node has no client entry; adapter returns false without side effects |
| 4 | `ConcurrentRouteLocksAreCorrect` | 8 threads × 10 K ingests on a 3-node ring; totals add up, no race on `shared_mutex`, all three branches exercised |

End-to-end coverage (HTTP → adapter → remote RPC → peer's `ingest_tick`) already exists in `tests/unit/test_distributed_insert.cpp::RoutesToRemoteOwner` which spins up a real 2-node cluster in-process.

## Build + verification

```
cd build && ninja -j$(nproc) zepto_http_server zepto_tests
./tests/zepto_tests --gtest_filter="CoordinatorRoutingAdapter*"   # 4/4 pass, 47 ms
./tests/zepto_tests                                               # 1288/1288 pass (was 1284)
```

Zero regressions. No new compiler warnings.

## What is NOT in this stage

- **EKS re-benchmark** — stage 3, separate from this commit. The null-result `docs/bench/results_multinode.md` will be updated with a "Round 2" section after the EKS bench runs.
- **Python cluster hook** (P8-I5) — unchanged; still an open BACKLOG item.
- **Stateless `zepto_ingest_node`** (P8-I3) — unchanged; still an open BACKLOG item. With P8-I3-wire landed, implementing the stateless binary is a one-liner (same `executor.set_cluster_node(...)` call pattern).

## Expected impact (to be verified in stage 3)

Based on the architecture (consistent-hash ring with ~0.84× per-pod scaling efficiency for small clusters due to RPC overhead + hash skew):

| Pods | Expected amd64 | Expected arm64 | vs today (flat) |
|---|---|---|---|
| 1 | ~380 /s | ~460 /s | same (local path unchanged) |
| 2 | ~700 /s | ~850 /s | **+84 %** |
| 3 | ~1000 /s | ~1250 /s | **+163 %** |

Per-pod ingest distribution should be ~33 % / ~33 % / ~33 % (±15 % hash skew) instead of 100 % / 0 % / 0 %.

## Files changed

- `tools/zepto_http_server.cpp` — TODO removed; cluster wire-up replaces the old rebalance block; +`<unordered_map>` include
- `include/zeptodb/cluster/coordinator_routing_adapter.h` — already landed (kept as-is)
- `include/zeptodb/cluster/query_coordinator.h` — `router()` + `router_mutex()` accessors already landed
- `tools/zepto_data_node.cpp` — 3-line comment on leaf-node decision
- `tests/unit/test_coordinator_routing_adapter.cpp` — 4 new tests
- `tests/CMakeLists.txt` — register new test file
- `docs/bench/results_multinode.md` — unchanged in this stage (stage 3 will add Round 2)
- `docs/BACKLOG.md` — mark P8-I3-wire ✅
- `docs/COMPLETED.md` — add Sprint closeout entry
