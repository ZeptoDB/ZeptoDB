# 037 — P8 Cluster Operational Stability Batch Implementation

**Date**: 2026-03-27
**Status**: Completed (All P8-Critical/High, P8-Medium 6/6)

---

## Background

Batch implementation of multinode operational stability items from the BACKLOG P8 section.
These are mandatory fixes before production deployment, covering split-brain, resource exhaustion,
concurrency bugs, failure recovery, snapshot consistency, and more.

## Changes

### P8-High: TcpRpcServer Resource Management (4 items)

**1. Thread Pool Conversion**
- `accept_loop()`'s `std::thread(...).detach()` → fixed-size worker pool + `std::queue<int>` task queue
- `set_thread_pool_size()` API (default `hardware_concurrency`)
- Files: `tcp_rpc.h`, `tcp_rpc.cpp`

**2. Payload Size Limit**
- `max_payload_size_` (default 64MB), validated in `handle_connection()` + auth handshake
- Connection closed + `ZEPTO_WARN` on exceeding limit
- Files: `tcp_rpc.h`, `tcp_rpc.cpp`

**3. Graceful Drain Mode**
- `stop()` 5 phases: listen close → queue drain → drain wait (default 30 seconds) → forced shutdown → worker join/detach
- `set_drain_timeout_ms()` API
- Files: `tcp_rpc.cpp`

**4. Concurrent Connection Limit**
- `max_connections_` (default 1024), immediately closed on exceeding limit
- Files: `tcp_rpc.h`, `tcp_rpc.cpp`

### P8-High: PartitionRouter Concurrency

- `ring_mutex_` (`std::shared_mutex`) built-in
- writer: `add_node`/`remove_node` → `unique_lock`
- reader: `route`/`route_replica`/`node_count`/`all_nodes`/`plan_*` → `shared_lock`
- copy constructor/assignment also protected by lock
- Files: `partition_router.h`

### P8-High: TcpRpcClient::ping() Connection Leak

- `connect_to_server()` + `close()` → `acquire()` + `release()` pool reuse
- Files: `tcp_rpc.cpp`

### P8-Medium: GossipNodeRegistry Data Race

- `bool running_` → `std::atomic<bool> running_{false}`
- Files: `node_registry.h`

### P8-Medium: K8sNodeRegistry Deadlock

- Removed `fire_event_unlocked()`
- In `register_node()`/`deregister_node()`/`start()`, release lock before calling `fire_event()`
- Files: `node_registry.h`

### P8-Medium: ClusterNode Node Rejoin

- Count based on whether RPC client creation succeeds when connecting to seed
- If `seed_connected == 0`, clean up router/transport and throw `std::runtime_error`
- Bootstrap (no seeds) is allowed normally
- Files: `cluster_node.h`

### P8-Medium: SnapshotCoordinator Consistency

- Single phase → 2PC (PREPARE → COMMIT/ABORT)
- Phase 1: Send `SNAPSHOT PREPARE <id>` to all nodes (pause ingestion)
- Phase 2: If all succeed, send `SNAPSHOT COMMIT <id>`; on failure, send `SNAPSHOT ABORT <id>`
- `take_snapshot_legacy()` for backward compatibility
- Files: `snapshot_coordinator.h`, `snapshot_coordinator.cpp`

### P8-Medium: K8sNodeRegistry Actual Implementation

- `poll_loop()` performs HTTP GET on K8s Endpoints API
- Auto-detects `KUBERNETES_SERVICE_HOST/PORT` environment variables
- Service account token authentication
- `parse_endpoints_json()`: extracts IP/port, stable NodeId via IP hash
- `reconcile()`: diffs against current node map → JOINED/LEFT events
- Non-K8s environments: manual `register_node()` fallback retained
- Files: `node_registry.h`

### P8-Medium: PartitionMigrator Atomicity (Phase A)

- `MoveState` state machine: PENDING → DUAL_WRITE → COPYING → COMMITTED/FAILED
- `MigrationCheckpoint`: tracks state of each move
- `resume_plan()`: retries only FAILED moves (max_retries=3)
- `execute_plan()` → returns `MigrationCheckpoint`
- Phase B (disk checkpoint) and Phase C (rollback) are not yet implemented
- Files: `partition_migrator.h`, `partition_migrator.cpp`

## Documentation

- `docs/design/multinode_stability.md` newly created (full stability design document)
- `docs/BACKLOG.md` completed items updated
- `.kiro/KIRO.md` document map updated with `multinode_stability.md`

## Tests

| Test | Verification |
|------|-------------|
| `TcpRpcServerPayloadLimit.RejectsOversizedPayload` | 128B limit, 256B rejected |
| `TcpRpcServerMaxConnections.RejectsWhenFull` | 2 limit, 3rd rejected |
| `TcpRpcServerThreadPool.ConcurrentRequestsWithSmallPool` | 2 workers, max concurrent ≤ 2 |
| `TcpRpcServerGracefulDrain.InFlightRequestCompletesBeforeStop` | Guarantees in-flight query completion |
| `TcpRpcServerGracefulDrain.ForceCloseAfterTimeout` | Forced termination after 100ms timeout |
| `PartitionRouterConcurrency.ConcurrentAddRemoveRoute` | 4 readers + 1 writer concurrent |
| `TcpRpcClientPing.UsesConnectionPool` | pool_idle_count=1 after ping |
| `GossipNodeRegistryAtomic.RunningFlagIsAtomic` | Atomic concurrent reads |
| `K8sNodeRegistryDeadlock.CallbackDuringRegisterDoesNotDeadlock` | Registry access within callback |
| `ClusterNodeSeedFailure.BootstrapWithNoSeedsSucceeds` | Bootstrap succeeds normally |
| `ClusterNodeSeedFailure.PartialSeedConnectionSucceeds` | Partial seed connection succeeds |
| `Snapshot.TwoPC_AbortOnPrepareFailure` | PREPARE failure → ABORT, COMMIT 0 times |
| `K8sNodeRegistryEndpoints.ParseEndpointsJson` | JSON parsing 3 nodes |
| `K8sNodeRegistryEndpoints.ReconcileDetectsJoinAndLeave` | JOINED/LEFT events |
| `PartitionMigratorStateMachine.ResumeRetiesFailedMoves` | 1st failure → retry succeeds |

All tests: 803+ passing, no regressions
