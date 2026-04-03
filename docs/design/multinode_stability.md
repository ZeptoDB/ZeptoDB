# ZeptoDB Multi-Node Operational Stability Design

**Date**: 2026-03-27
**Status**: P8-Critical/High completed, P8-Medium in progress

---

## Overview

This document summarizes the problems resolved and design decisions made to ensure production operational stability of ZeptoDB distributed clusters.
It covers all stability issues that can occur in a multi-node environment, including split-brain, data loss, resource exhaustion, concurrency bugs, and failure recovery.

### Architecture Context

```
┌─────────────────────────────────────────────────────────┐
│                    Control Plane                         │
│  CoordinatorHA (Active/Standby) + K8sLease + Fencing    │
│  QueryCoordinator (scatter-gather routing)               │
│  RingConsensus (epoch broadcast → all-node ring sync)    │
└────────────────────┬────────────────────────────────────┘
                     │ TcpRpc (HMAC auth)
     ┌───────────────┼───────────────┐
     ▼               ▼               ▼
┌─────────┐   ┌─────────┐   ┌─────────┐
│ DataNode │   │ DataNode │   │ DataNode │
│ Pipeline │   │ Pipeline │   │ Pipeline │
│ WAL+Rep  │   │ WAL+Rep  │   │ WAL+Rep  │
│ Health   │   │ Health   │   │ Health   │
└─────────┘   └─────────┘   └─────────┘
     ↕ UDP heartbeat + TCP probe ↕
```

---

## 1. Split-Brain / Data Loss Prevention (P8-Critical ✅)

### 1.1 PartitionRouter Distributed Synchronization

**Problem**: Each node holds an independent copy of PartitionRouter. No synchronization mechanism exists when the ring changes (node addition/removal), causing routing inconsistencies between nodes. The same symbol gets routed to different nodes, resulting in scattered data storage → inconsistent query results.

**Solution**: `RingConsensus` interface + `EpochBroadcastConsensus` implementation.

```
Coordinator: propose_add(node_id)
  → epoch++ → serialize(ring snapshot + epoch)
  → broadcast to all followers via TcpRpc RING_UPDATE
  → followers: apply_update() → epoch validation → ring replacement

Follower: RING_UPDATE received
  → epoch > local_epoch? → ring replacement + cache invalidation
  → epoch <= local_epoch? → ignore (stale)
```

- Only the Coordinator has ring modification authority (`is_coordinator=true`)
- Followers receive/apply ring updates via RPC callbacks
- Plugin architecture allowing replacement with a Raft implementation (`RingConsensus` interface)

**Related code**: `include/zeptodb/cluster/ring_consensus.h`
**Devlog**: `docs/devlog/030_ring_consensus.md`

### 1.2 CoordinatorHA — K8sLease-Based Split-Brain Prevention

**Problem**: Two Active coordinators can exist simultaneously. During a network partition, the Standby may incorrectly determine the Active has failed and promote itself → two coordinators performing writes concurrently.

**Solution**: K8sLease + FencingToken dual defense.

```
Standby: Active ping failure (2 seconds)
  → try_promote()
  → require_lease=true? → K8sLease::try_acquire() required
  → success: FencingToken::advance() → epoch bump
  → peer_rpc_.set_epoch(new_epoch)
  → transition to ACTIVE

Stale Active (epoch=5) recovers:
  → write attempt (TICK_INGEST, epoch=5)
  → DataNode: FencingToken::validate(5) → last_seen=6 → REJECTED
```

- `require_lease=false` (default): ping-based failover (testing/development)
- `require_lease=true` (production): K8sLease acquisition required
- Automatic demote on lease loss (ACTIVE → STANDBY)
- Current configuration: Active 1 + Standby 1

**Related code**: `include/zeptodb/cluster/coordinator_ha.h`, `src/cluster/coordinator_ha.cpp`
**Devlog**: `docs/devlog/031_coordinator_ha_lease.md`

### 1.3 WAL Replication Guarantee

**Problem**: WalReplicator drops data when the queue overflows in default async mode, with no retries on failure. Unreplicated data is lost on node failure.

**Solution**: 3 replication modes + retries + backpressure.

| Mode | Behavior | Use Case |
|------|----------|----------|
| `ASYNC` | fire-and-forget, drops on queue overflow | Development/testing |
| `SYNC` | Waits for all replica ACKs | Maximum durability |
| `QUORUM` | Waits for majority ACK out of W=N | Recommended for production |

- Failure retry queue: `max_retries=3`, exponential backoff
- Backpressure: producer blocks on queue saturation (automatic ingest rate throttling)
- Backward compatible with existing async/sync modes

**Related code**: `include/zeptodb/ingestion/wal_replicator.h`
**Devlog**: `docs/devlog/032_wal_replicator_reliability.md`

### 1.4 Failover Data Recovery

**Problem**: On node failure, the node is immediately removed from the router, but there is no procedure for recovering unreplicated data. Data is silently lost when callbacks are not registered.

**Solution**: Re-replication included as a mandatory step in failover.

```
Node DEAD detected
  → FailoverManager::handle_failure()
  → 1. router.remove_node(dead_id)
  → 2. auto_re_replicate=true?
       → PartitionMigrator: replica → new_replica data replication
       → async/sync selectable
  → 3. on_failover callback invocation
```

- Automatic re-replication enabled via `auto_re_replicate` setting
- Built-in PartitionMigrator, async/sync selectable
- Graceful fallback when node is not registered (log warning and continue)

**Related code**: `include/zeptodb/cluster/failover_manager.h`
**Devlog**: `docs/devlog/033_failover_data_recovery.md`

### 1.5 Internal RPC Security

**Problem**: Cluster-internal TCP RPC is plaintext. No authentication/authorization. Anyone with network access can execute SQL and inject data.

**Solution**: Shared-secret HMAC authentication protocol.

```
Client → Server: AUTH_HANDSHAKE (nonce + HMAC-SHA256(shared_secret, nonce))
Server: HMAC verification → AUTH_OK / AUTH_REJECT
  → on failure: connection immediately rejected
  → on success: normal RPC messages allowed
```

- `RpcSecurityConfig::enabled=true` + `shared_secret` configuration
- Automatic handshake on every new connection (TcpRpcClient::acquire())
- mTLS configuration structure prepared (`cert_path`/`key_path`/`ca_cert_path`)

**Related code**: `include/zeptodb/cluster/rpc_security.h`
**Devlog**: `docs/devlog/034_rpc_security.md`

---

## 2. Failure Detection and Recovery (P8-High ✅)

### 2.1 HealthMonitor — DEAD Node Recovery

**Problem**: Nodes in DEAD state remain DEAD forever even after recovery. No rejoin path exists. Operators must manually reconfigure the cluster.

**Solution**: Added `REJOINING` state.

```
State transitions:
  DEAD → REJOINING (when heartbeat is received again)
  REJOINING → ACTIVE (when on_rejoin callback returns true)
  REJOINING → REJOINING (callback returns false → retry on next heartbeat)
```

- `on_rejoin()` callback controls data resynchronization
- ClusterNode automatically re-adds to router on REJOINING→ACTIVE transition

### 2.2 HealthMonitor — UDP Fault Tolerance

**Problem**: A single UDP packet loss can trigger a DEAD determination. Socket bind failures are silently ignored.

**Solution**: Triple-layer defense.

| Defense Layer | Mechanism |
|---------------|-----------|
| Consecutive miss check | `consecutive_misses_for_suspect=3` — SUSPECT only after 3 consecutive misses |
| TCP double-check | TCP probe (port 9101) verification before SUSPECT→DEAD transition |
| Bind failure detection | `fatal_on_bind_failure=true` — exception raised on UDP bind failure |

**Related code**: `include/zeptodb/cluster/health_monitor.h`
**Devlog**: `docs/devlog/035_health_monitor_resilience.md`

---

## 3. RPC Server Resource Management (P8-High ✅)

Resolved 4 resource management issues in TcpRpcServer.

### 3.1 Thread Pool

**Problem**: `std::thread(...).detach()` per connection — unlimited thread count, OOM risk.

**Solution**: Fixed-size worker thread pool + task queue.

```
accept_loop(): accept() → conn_queue_.push(fd) → notify_one()
worker_loop(): queue_cv_.wait() → pop(fd) → handle_connection(fd) → close(fd)
```

- `set_thread_pool_size(n)` — default `hardware_concurrency`, minimum 4
- Queue wait on pool saturation (natural backpressure)
- All workers joinable on `stop()` (detach removed)

### 3.2 Payload Size Limit

**Problem**: `std::vector` allocation without `payload_len` validation — OOM attack possible with a 4GB payload header.

**Solution**: `max_payload_size_` validation (default 64MB).

- Validation in both `handle_connection()` main loop and auth handshake
- Connection immediately closed on exceeding limit + `ZEPTO_WARN` log
- Runtime configurable via `set_max_payload_size(bytes)` API

### 3.3 Graceful Drain

**Problem**: `stop()` abandons active connections after 1 second. In-flight query results are lost.

**Solution**: 5-stage graceful shutdown.

```
stop():
  ① close listen socket (reject new connections)
  ② drain pending queue (close waiting fds)
  ③ wait for in-flight requests to complete for drain_timeout_ms (default 30 seconds)
  ④ force shutdown(SHUT_RDWR) on timeout
  ⑤ join workers (normal) or detach (forced)
```

- Configurable via `set_drain_timeout_ms(ms)` API
- Protects in-flight queries during rolling upgrades

### 3.4 Concurrent Connection Limit

**Problem**: `active_conns_` only counts connections with no upper bound.

**Solution**: `max_connections_` setting (default 1024).

- Immediately `close(cfd)` + warning log on exceeding limit in `accept_loop()`
- Runtime configurable via `set_max_connections(n)` API

**Related code**: `include/zeptodb/cluster/tcp_rpc.h`, `src/cluster/tcp_rpc.cpp`

---

## 4. Routing and Connection Stability (P8-High ✅)

### 4.1 PartitionRouter Concurrency

**Problem**: No internal lock on `ring_`/`node_set_`. FailoverManager calls `remove_node()` without a lock → data race, TOCTOU bug.

**Solution**: Built-in `ring_mutex_` (`std::shared_mutex`).

| Operation | Lock Type |
|-----------|-----------|
| `add_node`, `remove_node` | `unique_lock` (writer) |
| `route`, `route_replica`, `node_count`, `all_nodes`, `plan_*` | `shared_lock` (reader) |
| copy constructor, `operator=` | Both sides lock-protected |

- Safe concurrent access without external `router_mutex_`
- Cache (`cache_mutex_`) retains its existing separate mutex

### 4.2 TcpRpcClient::ping() Connection Leak

**Problem**: `ping()` creates `connect_to_server()` + `close()` every time — new TCP connection created/destroyed every 500ms. Kernel TIME_WAIT sockets accumulate.

**Solution**: Connection pool reuse via `acquire()`/`release()`.

```
// Before (new connection every time)
int fd = connect_to_server();
... ping ...
close(fd);

// After (pool reuse)
int fd = acquire();      // take from pool or create new
... ping ...
release(fd, ok);         // return to pool on success, close on failure
```

**Related code**: `src/cluster/tcp_rpc.cpp`

---

## 5. Node Registry Stability (P8-Medium ✅ partial)

### 5.1 GossipNodeRegistry Data Race

**Problem**: `running_` is a `bool` — UB on concurrent read/write from multiple threads.

**Solution**: `std::atomic<bool> running_{false}`.

### 5.2 K8sNodeRegistry Deadlock

**Problem**: `fire_event_unlocked()` calls callbacks while holding the `mu_` lock. Deadlock occurs if the callback calls `active_nodes()` or similar.

**Solution**: Release lock before invoking callbacks.

```
// Before (deadlock)
void register_node(addr) {
    lock_guard lock(mu_);
    nodes_[id] = ...;
    fire_event_unlocked(id, JOINED);  // callback while holding mu_!
}

// After (safe)
void register_node(addr) {
    bool is_new;
    { lock_guard lock(mu_); is_new = ...; nodes_[id] = ...; }
    if (is_new) fire_event(id, JOINED);  // callback after lock release
}
```

### 5.3 ClusterNode Node Rejoin Validation

**Problem**: Node is treated as "joined" even when all seed connections fail. No peer map synchronization.

**Solution**: Seed connection success count + exception on total failure.

- `seed_connected` count based on Transport connect + RPC client creation success
- `seed_connected == 0` (when seeds is non-empty): clean up router/transport then throw `std::runtime_error`
- Bootstrap (no seeds): allowed normally (first node)

**Related code**: `include/zeptodb/cluster/node_registry.h`, `include/zeptodb/cluster/cluster_node.h`

---

## 6. Incomplete Items (P8-Medium remaining)

| Task | Current Problem | Required Action | Effort |
|------|----------------|-----------------|--------|
| **SnapshotCoordinator Consistency** | Parallel execution without global barrier, point-in-time inconsistency | 2PC (prepare → commit), pause ingestion or LSN cutoff | M |
| **K8sNodeRegistry Actual Implementation** | poll_loop() is an empty loop | Implement K8s Endpoints API watch | M |
| **PartitionMigrator Atomicity** | Partial move on mid-failure, cannot resume | State machine + checkpoint + rollback | L |

---

## 7. Configuration Summary

Recommended settings for production deployment:

```cpp
// Coordinator HA
CoordinatorHAConfig ha_cfg;
ha_cfg.require_lease = true;           // K8sLease required
ha_cfg.failover_after_ms = 2000;       // failover after 2 seconds

// WAL Replication
WalReplicatorConfig wal_cfg;
wal_cfg.mode = ReplicationMode::QUORUM; // majority ACK
wal_cfg.max_retries = 3;

// RPC Server
server.set_max_payload_size(64 * 1024 * 1024);  // 64MB
server.set_max_connections(1024);
server.set_thread_pool_size(0);                   // auto (hardware_concurrency)
server.set_drain_timeout_ms(30000);               // 30 seconds

// RPC Security
RpcSecurityConfig sec;
sec.enabled = true;
sec.shared_secret = "<cluster-secret>";

// Health Monitor
HealthConfig health;
health.consecutive_misses_for_suspect = 3;
health.fatal_on_bind_failure = true;
// TCP probe auto-enabled (port 9101)
```

---

## 8. Test Coverage

| Area | Test Count | Key Scenarios |
|------|------------|---------------|
| RPC server resources | 4 | payload limit, connection limit, thread pool concurrency, graceful drain |
| Split-brain prevention | 2 | stale epoch tick/WAL rejection (FencingRpc) |
| PartitionRouter concurrency | 1 | 4 reader + 1 writer concurrent add/remove/route |
| ping() pool reuse | 1 | pool_idle_count check after consecutive pings |
| GossipNodeRegistry | 1 | atomic running_ concurrent reads |
| K8sNodeRegistry deadlock | 1 | no deadlock on registry access within callback |
| ClusterNode seed validation | 2 | exception on total seed failure, bootstrap success |
| CoordinatorHA | 7 | promote/demote, lease integration, dual promote prevention |
| WAL replication | 3 | quorum write, retries, backpressure |
| Failover | 2 | auto re-replication, graceful fallback |

Total tests: 803+ passing
