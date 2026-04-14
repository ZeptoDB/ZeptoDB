# Phase C: Distributed Memory & Cluster Architecture

> Cloud-Native horizontal scaling, swappable Transport abstraction (RDMA → CXL), lightweight Control Plane without Kubernetes

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────┐
│         Zepto Control Plane (single binary)       │
│                                                   │
│  ┌──────────┐ ┌───────────┐ ┌────────────────┐  │
│  │ Fleet    │ │ Metadata  │ │ Health         │  │
│  │ Manager  │ │ Store     │ │ Monitor        │  │
│  │(EC2 Fleet│ │(DynamoDB) │ │(Heartbeat +    │  │
│  │ API)     │ │           │ │ Failover)      │  │
│  └──────────┘ └───────────┘ └────────────────┘  │
│  ┌──────────┐ ┌───────────┐                      │
│  │ Partition│ │ Metrics   │                      │
│  │ Router   │ │ Exporter  │                      │
│  │(Consist. │ │(Prometheus│                      │
│  │ Hashing) │ │ format)   │                      │
│  └──────────┘ └───────────┘                      │
└───────────────────┬─────────────────────────────┘
                    │ Management (gRPC / REST)
                    │
   ┌────────────────┼────────────────────┐
   │                │                    │
┌──┴───┐  ┌────────┴─────┐  ┌──────────┴──┐
│ Node1│←→│    Node2     │←→│    Node3    │  Data Plane
│ APEX │  │    APEX      │  │    APEX     │  (EFA/RDMA direct)
│ DB   │  │    DB        │  │    DB       │
└──────┘  └──────────────┘  └─────────────┘
   ↑              ↑                ↑
   └──── Placement Group (CLUSTER) ────┘
         Same AZ, same rack → lowest latency
```

---

## 2. Transport Abstraction Layer

### 2-A. Interface Design (swappable modules)

```cpp
// Compile-time dispatch — zero virtual call overhead
template <typename Impl>
class TransportBackend {
public:
    // Register memory region on a remote node
    RemoteRegion register_memory(void* addr, size_t size);

    // One-sided RDMA write (no remote CPU involvement)
    void remote_write(const void* local, RemoteRegion remote, size_t offset, size_t size);

    // One-sided RDMA read
    void remote_read(RemoteRegion remote, size_t offset, void* local, size_t size);

    // Memory fence (ordering guarantee)
    void fence();

    // Connect/disconnect nodes
    ConnectionId connect(const NodeAddress& addr);
    void disconnect(ConnectionId conn);
};
```

### 2-B. Backend Implementations

| Backend | Purpose | Latency |
|---|---|---|
| `UCXBackend` | Production — RDMA/AWS EFA/InfiniBand | ~1-15μs |
| `CXLBackend` | Next-gen — CXL 3.0 memory semantics | ~150-300ns |
| `SharedMemBackend` | Dev/test — single-machine POSIX shm | ~100ns |
| `TCPBackend` | Fallback — environments without RDMA | ~50-100μs |

### 2-C. Scope of Change for CXL Migration

```cpp
// Current (RDMA)
using ProductionTransport = TransportBackend<UCXBackend>;

// Future (CXL 3.0) — change only this one line
using ProductionTransport = TransportBackend<CXLBackend>;
```

With CXL, `remote_write/read` internally becomes a simple `memcpy`.
Hardware guarantees cache coherency, so `fence()` only needs `std::atomic_thread_fence`.

---

## 3. Control Plane Design

### 3-A. Fleet Manager (EC2 Fleet API)

```cpp
struct FleetConfig {
    // EFA-capable instances only
    std::vector<std::string> instance_types = {"r7i.8xlarge", "r8g.8xlarge"};

    // Placement Group — same rack placement
    std::string placement_group = "zepto-cluster";
    PlacementStrategy strategy = PlacementStrategy::CLUSTER;

    // Warm Pool — pre-warmed standby instances
    size_t warm_pool_size = 2;

    // Capacity Reservation
    CapacityMode capacity_mode = CapacityMode::ON_DEMAND_RESERVED;
};

class FleetManager {
    // Immediately add a node (from warm pool → seconds)
    NodeId launch_node();

    // Graceful shutdown (migrate partitions → terminate)
    void drain_and_terminate(NodeId id);

    // Maintain warm pool (booted, ZeptoDB ready state)
    void maintain_warm_pool();

    // Current cluster state
    ClusterTopology topology() const;
};
```

### 3-B. Metadata Store (DynamoDB)

```
Table: zepto-cluster-metadata

PK: "partition#{symbol_id}#{hour_epoch}"
SK: "assignment"
Attributes:
  - node_id: "node-abc123"
  - state: ACTIVE | MIGRATING | SEALED
  - arena_usage_pct: 45.2
  - created_at: 1711065600

Table: zepto-cluster-nodes

PK: "node#{node_id}"
Attributes:
  - address: "10.0.1.5:9000"
  - state: JOINING | ACTIVE | SUSPECT | DEAD | LEAVING
  - last_heartbeat: 1711065612
  - instance_type: "r7i.8xlarge"
  - partitions_count: 42
```

Why DynamoDB?
- Serverless → zero operational overhead
- Single-digit ms latency (metadata access is cold path)
- Automatic replication + high availability

### 3-C. Health Monitor (Heartbeat + Failover)

```cpp
struct HealthConfig {
    uint32_t heartbeat_interval_ms = 1000;   // every 1 second
    uint32_t suspect_timeout_ms = 3000;      // 3s no response → SUSPECT
    uint32_t dead_timeout_ms = 10000;        // 10s → DEAD
    uint32_t failover_grace_ms = 5000;       // partition migration grace period
};

// State transitions:
// ACTIVE → (3s no response) → SUSPECT → (7s more) → DEAD → failover triggered
// SUSPECT + heartbeat resumes → ACTIVE
```

Failover procedure:
1. Node declared DEAD
2. Query partition list for that node (DynamoDB)
3. Reassign partitions to next node in Consistent Hash Ring
4. Activate Warm Pool node to recover data (load from HDB)

### 3-D. Partition Router (Consistent Hashing)

```cpp
class PartitionRouter {
    // Symbol → Node routing (O(1) local hash table)
    NodeId route(SymbolId symbol) const;

    // Add node — move minimum partitions
    MigrationPlan add_node(NodeId new_node);

    // Remove node — partitions go clockwise to next node
    MigrationPlan remove_node(NodeId failed_node);

    // Virtual nodes for even distribution
    // 1 physical node = 128 virtual nodes → even data distribution
    static constexpr size_t VIRTUAL_NODES_PER_PHYSICAL = 128;
};
```

### 3-E. Ring Consensus (Distributed Synchronization)

Each node holds an independent copy of the PartitionRouter. When the ring changes (node add/remove),
all nodes' routers must be synchronized to prevent routing inconsistencies.

**Interface**: `RingConsensus` (abstract class)

```cpp
class RingConsensus {
    virtual bool propose_add(NodeId node) = 0;     // Coordinator: add node
    virtual bool propose_remove(NodeId node) = 0;   // Coordinator: remove node
    virtual bool apply_update(const uint8_t* data, size_t len) = 0;  // Follower: receive and apply
    virtual uint64_t current_epoch() const = 0;
};
```

**Default implementation**: `EpochBroadcastConsensus` (eventual consistency)

- On ring change, Coordinator calls `FencingToken::advance()` → epoch bump
- Broadcasts `RING_UPDATE` RPC to all peers (serialized RingSnapshot)
- Followers apply only when epoch ≥ last_seen (reject stale updates)
- Existing dual-write (`migrating_`) prevents data loss during transitions

**Future extension**: Replace with `RaftConsensus` implementation for strong consistency.
Can be injected at runtime via `ClusterNode::set_consensus()`.

```
Coordinator (epoch=5)
  ├─ add_node(Node4)
  ├─ epoch = 6 (advance)
  └─ broadcast RING_UPDATE{epoch=6, nodes=[1,2,3,4]}
       → Node1 ✓ (6 ≥ 5, apply)
       → Node2 ✓ (6 ≥ 5, apply)
       → Node3 ✓ (6 ≥ 5, apply)
       → Node4 ✓ (6 ≥ 0, apply)
```

---

## 4. Data Plane Design

### 4-A. Distributed Arena (Global Memory Pool)

```cpp
template <typename Transport>
class DistributedArena {
    Transport transport_;
    LocalArena local_arena_;           // Local memory (existing ArenaAllocator)
    RemoteRegion registered_region_;    // Region registered with Transport

    // Local allocation (hot path — same as before)
    void* allocate_local(size_t size);

    // Allow remote nodes to directly write to this arena (RDMA one-sided)
    RemoteRegion expose();
};
```

### 4-B. Distributed Ingestion Flow

```
Client Tick → PartitionRouter.route(symbol)
                    │
            ┌───────┴───────┐
            │ Local node?    │
            ├── YES ────────→ Local Ring Buffer → Local RDB
            │
            └── NO ─────────→ Transport.remote_write()
                              → Remote node Ring Buffer (zero-copy)
```

### 4-C. Distributed Query Flow

```
Client Query(VWAP, symbol=AAPL)
    → PartitionRouter: "AAPL is on Node2"
    → Send query request to Node2 (gRPC)
    → Node2 executes locally (SIMD vectorized)
    → Return result

// When time range spans multiple partitions:
Client Query(VWAP, symbol=AAPL, range=24h)
    → Send partial queries per partition to each node in parallel
    → Collect partial results (partial VWAP: Σpv, Σv)
    → Final aggregation at client
```

---

## 5. Scaling Scenarios

### Scale Out (Add Node)
```
1. FleetManager: activate node from warm pool (seconds)
2. New node → register as JOINING in Control Plane
3. PartitionRouter: add to consistent hash
4. Generate MigrationPlan → list of partitions to migrate
5. Source node → RDMA-transfer partition data to new node
6. Update DynamoDB metadata
7. New node ACTIVE → start receiving traffic
```

### Scale In (Remove Node)
```
1. Mark target node as LEAVING
2. Migrate partitions → clockwise next node in consistent hash
3. Terminate after migration confirmed
4. FleetManager: replenish warm pool
```

### Failure Recovery
```
1. Heartbeat failure → SUSPECT (3s) → DEAD (10s)
2. Partitions of failed node → reassigned to next node
3. RDB data loss → recover from HDB (S3/NVMe)
4. WAL replay to restore latest data
5. Activate warm pool node
```

---

## 6. Tech Stack

| Component | Technology |
|---|---|
| Transport (current) | UCX → RDMA/AWS EFA |
| Transport (future) | CXL 3.0 (module swap) |
| Metadata | DynamoDB (serverless) |
| Node management | EC2 Fleet API + Warm Pool |
| Network placement | Placement Group (CLUSTER) |
| Inter-node RPC | gRPC (management) / RDMA (data) |
| HDB Cold Storage | S3 |
| Monitoring | Prometheus exporter → CloudWatch/Grafana |
| Configuration | S3 JSON or DynamoDB |

---

## 7. Implementation Order

### Phase C-1: Transport Abstraction
- `TransportBackend` interface
- `SharedMemBackend` (for testing)
- `UCXBackend` (production)
- Extend existing ArenaAllocator → DistributedArena

### Phase C-2: Cluster Core
- `PartitionRouter` (consistent hashing)
- `HealthMonitor` (heartbeat)
- `ClusterNode` (node process)
- Local 2-node test (SharedMem)

### Phase C-3: AWS Integration
- `FleetManager` (EC2 Fleet API)
- DynamoDB metadata
- Placement Group configuration
- EFA real-world testing

### Phase C-3 MVP: QueryCoordinator + TCP RPC ✅ Completed (2026-03-22)
- `QueryCoordinator` — two-tier routing:
  - Tier A: `WHERE symbol = N` (integer) → consistent-hash direct route to owning node
  - Tier A: `WHERE symbol = 'AAPL'` (string) → currently falls through to Tier B (scatter-gather); each node resolves string via local dictionary
  - Tier B: scatter-gather to all nodes → partial aggregation merge
- `TcpRpcServer` / `TcpRpcClient` — POSIX socket transport
  - 24-byte `RpcHeader` (magic, type, request_id, payload_len, epoch)
  - Binary `QueryResultSet` wire format (error, column names/types, packed int64 rows)
  - Connection pooling (acquire/release, MSG_PEEK liveness, max 4 idle)
- `partial_agg.h` — merge strategies:
  - `SCALAR_AGG`: SQL-AST-driven per-column merge (SUM/COUNT=add, MIN=min, MAX=max, AVG=SUM/COUNT rewrite)
  - `MERGE_GROUP_BY`: re-aggregate same key buckets across nodes (xbar time bars)
  - `CONCAT`: plain rows or GROUP BY with symbol affinity (no key overlap)
  - Strategy detected from SQL AST (not column names — executor returns raw names)
- 25 tests: RpcProtocol (5), PartialAgg (11), TcpRpc (4), QueryCoordinator (5)

### Phase C-3.5: Cluster Integrity ✅ Completed (2026-03-23)

**Problem:** Multiple components maintained independent state that could desync,
and split-brain scenarios had incomplete protection.

**Changes:**

1. **Unified PartitionRouter** — `QueryCoordinator::set_shared_router()` accepts
   an external `PartitionRouter*` + `shared_mutex*`. `ClusterNode::connect_coordinator()`
   injects its router into the coordinator. Eliminates dual-router desync.
   - Fallback: standalone QueryCoordinator uses its own internal router (backward compat)

2. **FencingToken in RPC protocol** — `RpcHeader` extended from 16 → 24 bytes with
   `uint64_t epoch` field. `TcpRpcServer::set_fencing_token()` enables write validation.
   - `TICK_INGEST` / `WAL_REPLICATE` with `epoch < last_seen` → rejected (status=0)
   - `epoch=0` → bypasses fencing (backward compatible with legacy clients)
   - Split-brain defense: stale coordinator's writes are rejected after new coordinator
     advances the epoch via `FencingToken::advance()`

3. **CoordinatorHA auto re-registration** — on standby→active promotion, all nodes
   in `registered_nodes_` are automatically replayed into the coordinator as remote
   endpoints. No manual callback wiring needed for basic functionality.

4. **ComputeNode merge logic** — `ComputeNode::execute()` now delegates to an internal
   `QueryCoordinator` instead of naive concat. Correctly handles GROUP BY, scalar
   aggregates, AVG rewrite, and ASOF JOIN routing.
   - Bug fix: `SELECT *` was misclassified as `SCALAR_AGG` (treated `is_star` as aggregate)

**Split-brain defense status:**

| Scenario | Defense | Status |
|----------|---------|--------|
| Stale coordinator TICK_INGEST | RpcHeader.epoch + FencingToken.validate() | ✅ Tested |
| Stale coordinator WAL_REPLICATE | Same | ✅ Tested |
| K8s Lease takeover detection | K8sLease.force_holder() → on_lost callback | ✅ Tested |
| FencingToken monotonic gate | epoch < last_seen → reject | ✅ Tested |
| Legacy client (epoch=0) | Bypasses fencing (backward compat) | ⚠️ By design |
| SQL_QUERY from stale coordinator | No fencing (reads are safe) | ⚠️ By design |

**Tests added:** 9 new tests (SharedRouter ×2, FencingRpc ×2, CoordinatorHA ×1, SplitBrain ×4)

**Related code:**
- `include/zeptodb/cluster/query_coordinator.h` — shared router API
- `include/zeptodb/cluster/rpc_protocol.h` — 24-byte RpcHeader
- `include/zeptodb/cluster/tcp_rpc.h` — fencing token + epoch on client/server
- `include/zeptodb/cluster/cluster_node.h` — `connect_coordinator()`
- `src/cluster/coordinator_ha.cpp` — auto re-registration on promote

### Phase C-3.6: Distributed Query Correctness ✅ Partial (2026-03-23)

**Completed:**
- VWAP distributed decomposition: `VWAP(p,v)` → `SUM(p*v), SUM(v)` rewrite → `SUM_PV/SUM_V` reconstruction
- ORDER BY + LIMIT post-merge: coordinator sorts merged results and truncates after all merge strategies

**Remaining query-level gaps:**

| Gap | Status | Notes |
|-----|--------|-------|
| HAVING distributed | TODO | Apply after GROUP BY merge at coordinator |
| DISTINCT distributed | TODO | Dedup at coordinator after concat |
| Window functions distributed | TODO | Requires full dataset; fetch-and-compute |
| FIRST/LAST distributed | TODO | Timestamp comparison across nodes |
| COUNT(DISTINCT) distributed | TODO | HyperLogLog or exact dedup |
| Subquery/CTE distributed | TODO | Cross-node CTE reference |
| Multi-column ORDER BY | TODO | Extend sort to composite key |

**Remaining infrastructure gaps:**

| Gap | Status | Notes |
|-----|--------|-------|
| Cancel propagation | TODO | Coordinator timeout → cancel RPC to nodes |
| Partial failure policy | TODO | Some nodes fail: partial result vs error |
| In-flight query safety | TODO | Node add/remove during scatter → race |
| Distributed query timeout | TODO | Remote-side timeout enforcement |

**Completed infrastructure gaps:**

| Gap | Resolved In | Notes |
|-----|-------------|-------|
| Dual-write during migration | devlog 055 | Wired into `ClusterNode::ingest_tick()` via `migration_target()` |

**Remaining precision gaps:**

| Gap | Status | Notes |
|-----|--------|-------|
| AVG int64 truncation | TODO | Float AVG loses precision via integer division |
| VWAP int64 overflow | TODO | SUM(price*volume) can overflow for large datasets |

### Phase C-4: Distributed Query
- UCX scatter-gather (replace TCP with RDMA for production)
- Consistent hashing for live node add/remove
- Replication factor 2 with automatic failover
- Multi-node benchmarks

---

## 8. Core Design Principles

1. **No indirect calls in hot path** — template dispatch, inline
2. **Control Plane != Data Plane** — management can be slow, data must be μs
3. **Transport swap = 1 line change** — painless RDMA → CXL migration
4. **No Kubernetes** — Fleet API + DynamoDB is sufficient
5. **Warm Pool** — add nodes in seconds, no boot wait
6. **Consistent Hashing** — minimum partition movement on node changes

---

## 9. Live Rebalancing (RebalanceManager)

Implemented in `include/zeptodb/cluster/rebalance_manager.h` + `src/cluster/rebalance_manager.cpp`.
Full implementation details: [`docs/devlog/055_live_rebalancing.md`](../devlog/055_live_rebalancing.md)

### Overview

```mermaid
graph TB
    subgraph Coordinator
        RM[RebalanceManager]
        PR[PartitionRouter]
        PM[PartitionMigrator]
        RC[RingConsensus]
    end

    subgraph Source Node
        SrcPipe[Ingest Pipeline]
        SrcStore[ColumnStore]
    end

    subgraph Destination Node
        DstPipe[Ingest Pipeline]
        DstStore[ColumnStore]
    end

    RM -->|1. plan_add/remove| PR
    RM -->|2. execute moves| PM
    PM -->|3. begin_migration| PR
    PM -->|4. SELECT data| SrcStore
    PM -->|5. replicate_wal| DstPipe
    PM -->|6. end_migration| PR
    RM -->|7. broadcast| RC
```

### Dual-Write During Migration

`ClusterNode::ingest_tick()` checks `PartitionRouter::migration_target()` before normal routing. If a symbol is migrating, ticks are sent to both source and destination nodes (dual-write). This prevents data loss during partition moves.

```mermaid
sequenceDiagram
    participant Client
    participant CN as ClusterNode
    participant Router as PartitionRouter
    participant A as Node A (source)
    participant B as Node B (dest)

    Note over Router: migration active: sym → {A, B}
    Client->>CN: ingest_tick(sym)
    CN->>Router: migration_target(sym)
    Router-->>CN: {from=A, to=B}
    CN->>A: send_to_node(A, tick) ✅
    CN->>B: send_to_node(B, tick) ✅
```

### RebalanceManager State Machine

```mermaid
stateDiagram-v2
    [*] --> IDLE
    IDLE --> RUNNING : start_add_node() / start_remove_node()
    RUNNING --> PAUSED : pause()
    RUNNING --> CANCELLING : cancel()
    RUNNING --> IDLE : all moves done
    PAUSED --> RUNNING : resume()
    PAUSED --> CANCELLING : cancel()
    CANCELLING --> IDLE : worker exits
```

- `start_add_node(NodeId)` — plans migration via `router_.plan_add()`, executes in background thread
- `start_remove_node(NodeId)` — plans migration via `router_.plan_remove()`, executes in background thread
- Each move is delegated to `PartitionMigrator::execute_plan()` (handles dual-write begin/end, copy, retry)
- Supports pause/resume/cancel via `std::atomic<RebalanceState>` + condition variable
- Checkpoint support via `PartitionMigrator::set_checkpoint_path()`

### Per-Move State Machine

```mermaid
stateDiagram-v2
    [*] --> PENDING
    PENDING --> DUAL_WRITE : begin_migration()
    DUAL_WRITE --> COPYING : start transfer
    COPYING --> COMMITTED : success
    COPYING --> FAILED : error + rollback
    FAILED --> PENDING : retry (max 3)
```

### 9.1 Load-Based Auto-Trigger (RebalancePolicy)

`RebalancePolicy` enables automatic rebalancing when cluster load becomes imbalanced.

```cpp
struct RebalancePolicy {
    bool     enabled = false;
    double   imbalance_ratio = 2.0;    // trigger if max/min partition count > 2x
    uint32_t check_interval_sec = 60;
    uint32_t cooldown_sec = 300;       // min time between auto-rebalances
};
```

A background `policy_thread_` periodically calls a `LoadProvider` callback to get per-node partition counts. If `max_count / min_count > imbalance_ratio` and the cooldown has elapsed, it calls `start_remove_node()` on the most overloaded node.

```mermaid
graph TD
    A[Sleep check_interval_sec] --> B{IDLE?}
    B -->|no| A
    B -->|yes| C{Cooldown elapsed?}
    C -->|no| A
    C -->|yes| D[load_provider → per-node counts]
    D --> E{max/min > ratio?}
    E -->|no| A
    E -->|yes| F[start_remove_node on max node]
    F --> A
```

### 9.2 Admin HTTP API

Five REST endpoints for rebalance control, registered in `HttpServer::setup_admin_routes()`:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/admin/rebalance/status` | GET | JSON status snapshot |
| `/admin/rebalance/start` | POST | Start add_node or remove_node |
| `/admin/rebalance/pause` | POST | Pause current rebalance |
| `/admin/rebalance/resume` | POST | Resume paused rebalance |
| `/admin/rebalance/cancel` | POST | Cancel current rebalance |

All require admin role. Returns 503 if `RebalanceManager` is not configured.
Full API reference: [`docs/api/HTTP_REFERENCE.md`](../api/HTTP_REFERENCE.md#admin-rebalance)

### Partial-Move API

In addition to full add/remove node operations, `start_move_partitions()` accepts an explicit list of `{symbol, from, to}` moves. This enables:
- Hot-symbol rebalancing without draining an entire node
- Manual capacity planning and partition placement
- Surgical moves for maintenance windows

Partial moves reuse the same `start_plan()` → `run_loop()` execution path. The key difference: `action_` is set to `NONE`, so no `RingConsensus` broadcast occurs after completion (the ring topology doesn't change — only data placement does).
