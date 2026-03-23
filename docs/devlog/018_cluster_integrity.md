# Devlog 018: Cluster Integrity & Split-Brain Defense

**Date:** 2026-03-23
**Phase:** C-3.5 (Cluster Integrity)

---

## Motivation

Code review of the multi-node architecture revealed four structural issues:

1. `ClusterNode` and `QueryCoordinator` each maintained independent `PartitionRouter` instances — no synchronization between them
2. `RpcHeader` had no fencing mechanism — a stale coordinator could write to data nodes after failover
3. `CoordinatorHA` standby promotion left the coordinator's endpoint list empty
4. `ComputeNode::execute()` used naive concat — incorrect results for GROUP BY and aggregate queries

---

## Changes

### 1. Unified PartitionRouter

`QueryCoordinator` now accepts an external router via `set_shared_router(PartitionRouter*, shared_mutex*)`. `ClusterNode` injects its router with `connect_coordinator(coord)`.

When no external router is set, the coordinator falls back to its own internal router (backward compatible).

Locking: `router_read_lock()` / `router_write_lock()` return the appropriate lock regardless of which router is active.

### 2. FencingToken in RPC Protocol

`RpcHeader` extended from 16 → 24 bytes:

```
Before: magic(4) + type(4) + request_id(4) + payload_len(4) = 16 bytes
After:  magic(4) + type(4) + request_id(4) + payload_len(4) + epoch(8) = 24 bytes
```

Server-side: `TcpRpcServer::set_fencing_token(FencingToken*)` — when set, TICK_INGEST and WAL_REPLICATE messages with `epoch < last_seen` are rejected.

Client-side: `TcpRpcClient::set_epoch(uint64_t)` — sets the epoch included in write messages.

Backward compatibility: `epoch=0` bypasses fencing entirely.

### 3. CoordinatorHA Auto Re-registration

On standby→active promotion, the monitor loop now iterates `registered_nodes_` and calls `coordinator_.add_remote_node()` for each. This ensures the promoted coordinator can immediately route queries.

### 4. ComputeNode Merge Logic

`ComputeNode` now contains an internal `QueryCoordinator`. `add_data_node()` registers with both the direct RPC map and the coordinator. `execute()` delegates to `coordinator_.execute_sql()`.

Bug fix discovered during this work: `SELECT *` was misclassified as `SCALAR_AGG` because the `is_star` flag caused the `all_agg` check to skip the column. Fixed by treating `agg == NONE` as non-aggregate regardless of `is_star`.

---

## Split-Brain Verification

Four simulation tests validate the end-to-end fencing flow:

1. **StaleCoordinatorWriteRejected** — Coordinator A (epoch=1) writes successfully. Coordinator B promoted (epoch=2) writes successfully. A tries again with epoch=1 → rejected. Data count stays at 2 (no corruption).

2. **StaleWalReplicationRejected** — New coordinator (epoch=3) replicates WAL. Old coordinator (epoch=1) tries WAL replication → rejected.

3. **K8sLeasePreventsDualLeader** — Lease holder A loses lease to B via `force_holder()`. A cannot re-acquire. FencingToken gate rejects A's stale epoch.

4. **LegacyClientBypass** — Client with epoch=0 bypasses fencing (backward compat confirmed).

### Remaining Gaps

- `epoch=0` bypass should be disabled in production (enforce non-zero epoch)
- SQL_QUERY (reads) have no fencing — by design (reads are safe)
- CoordinatorHA does not auto-wire FencingToken on promotion — caller must connect K8sLease → FencingToken → TcpRpcClient.set_epoch chain

---

## Test Summary

| Test | Description |
|------|-------------|
| SharedRouter.CoordinatorUsesClusterNodeRouter | Shared router visible from both sides |
| SharedRouter.CoordinatorFallsBackToOwnRouter | Standalone mode works |
| FencingRpc.StaleEpochTickRejected | Stale tick rejected, current accepted, legacy bypasses |
| FencingRpc.StaleEpochWalRejected | Stale WAL rejected, current accepted |
| CoordinatorHA.PromotionReRegistersNodes | Remote nodes available after promotion |
| SplitBrain.FencingPreventsStaleWrite | Unit-level FencingToken validation |
| SplitBrain.StaleCoordinatorWriteRejected | Full E2E split-brain scenario |
| SplitBrain.StaleWalReplicationRejected | WAL replication split-brain |
| SplitBrain.K8sLeasePreventsDualLeader | Lease + fencing integration |

**Total new tests: 9** | All cluster tests: 77 passing

---

## Files Changed

| File | Change |
|------|--------|
| `include/apex/cluster/query_coordinator.h` | `set_shared_router()`, `router()`, lock helpers |
| `src/cluster/query_coordinator.cpp` | Use `router()` + lock methods; fix `SELECT *` SCALAR_AGG bug |
| `include/apex/cluster/cluster_node.h` | `connect_coordinator()` method |
| `include/apex/cluster/rpc_protocol.h` | RpcHeader 16→24 bytes, epoch field |
| `include/apex/cluster/tcp_rpc.h` | `set_fencing_token()`, `set_epoch()` |
| `src/cluster/tcp_rpc.cpp` | Epoch validation in TICK_INGEST/WAL_REPLICATE handlers |
| `src/cluster/coordinator_ha.cpp` | Auto re-register nodes on promotion |
| `include/apex/cluster/compute_node.h` | Internal QueryCoordinator member |
| `src/cluster/compute_node.cpp` | Delegate execute() to coordinator |
| `tests/unit/test_coordinator.cpp` | 9 new tests |

---

## Next Steps

- Enforce non-zero epoch in production mode (`TcpRpcServer::set_require_epoch(true)`)
- Wire FencingToken into CoordinatorHA promotion path automatically
- Integration test: 3-node cluster with actual network partition simulation
