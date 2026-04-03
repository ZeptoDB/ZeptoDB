# 030 — RingConsensus: PartitionRouter Distributed Synchronization

**Date**: 2026-03-27
**Status**: Completed
**P8-Critical**: PartitionRouter Distributed Synchronization

---

## Background

Each ClusterNode, QueryCoordinator, and ComputeNode holds an independent copy of PartitionRouter.
There is no mechanism to synchronize ring changes on node addition/removal, which can cause routing inconsistencies between nodes.

## Design Decision

**Epoch Broadcast** approach adopted (instead of Raft).

Reasons:
- Ring changes occur infrequently (only on node failure/addition)
- State is small (node list, a few KB)
- CoordinatorHA + FencingToken infrastructure already exists
- ZeptoDB target domains (HFT, IoT, Observability) are fine with eventual consistency
- Raft consensus latency could degrade tick-path performance

The interface is separated so it can be replaced with RaftConsensus in the future
if strong consistency is needed (e.g., banking/healthcare).

## Changed Files

### New
- `include/zeptodb/cluster/ring_consensus.h`
  - `RingConsensus` — abstract interface (propose_add, propose_remove, apply_update)
  - `RingSnapshot` — ring state serialization/deserialization
  - `EpochBroadcastConsensus` — epoch broadcast implementation

### Modified
- `include/zeptodb/cluster/rpc_protocol.h` — added `RING_UPDATE(13)`, `RING_ACK(14)`
- `include/zeptodb/cluster/tcp_rpc.h` — `RingUpdateCallback`, `set_ring_update_callback()`
- `src/cluster/tcp_rpc.cpp` — `RING_UPDATE` handler (apply → RING_ACK response)
- `include/zeptodb/cluster/cluster_node.h`
  - `ClusterConfig::is_coordinator` flag
  - `fencing_token_`, `consensus_` members
  - `join_cluster()` — automatic consensus initialization + RPC callback registration
  - `on_node_state_change()` — goes through consensus if coordinator, applies local change directly if follower
  - `set_consensus()` — external implementation injection (Raft-ready)

## Operation Flow

```
Coordinator:
  on_node_state_change(ACTIVE) → consensus_->propose_add(id)
    → router_.add_node(id)
    → token_.advance() (epoch bump)
    → broadcast RING_UPDATE to all peers
    → peers apply_update() (epoch validate → router rebuild)

Follower:
  TcpRpcServer receives RING_UPDATE
    → ring_update_callback_ → consensus_->apply_update()
    → FencingToken::validate(epoch) — rejects stale
    → router rebuild
    → RING_ACK response
```

## Replacement Path

```cpp
// Default (EpochBroadcast)
node.join_cluster(seeds);

// When introducing Raft
node.set_consensus(std::make_unique<RaftConsensus>(config));
node.join_cluster(seeds);
```

## Tests

All existing 796 tests passed (no regression).
