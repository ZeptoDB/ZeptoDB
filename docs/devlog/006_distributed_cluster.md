# 006 — Phase C: Distributed Memory & Cluster Architecture

> Date: 2026-03-22
> Phase: C-1 (Transport Abstraction) + C-2 (Cluster Core)
> Status: Complete — C++ 40 tests PASS, Python 31 tests PASS

---

## Overview

After completing Phase E (LLVM JIT), B (Vectorized Engine), A (Storage), and D (Python Binding),
Phase C takes APEX-DB to distributed systems.

The goal is simple:
**Scale APEX-DB horizontally across multiple nodes with zero virtual call overhead on the hot path.**

---

## Design Principles

Core principles established in the Phase C design document:

1. **No indirect calls on hot path** — CRTP template dispatch, inlined
2. **Transport swap = 1-line change** — painless RDMA -> CXL migration
3. **Full testing with SharedMem** — local development without RDMA hardware
4. **Consistent Hashing** — minimal partition movement on node changes

---

## Phase C-1: Transport Abstraction

### CRTP-based TransportBackend

```cpp
// Compile-time dispatch — zero virtual call overhead
template <typename Impl>
class TransportBackend {
public:
    void remote_write(const void* src, const RemoteRegion& remote,
                      size_t offset, size_t bytes) {
        impl().do_remote_write(src, remote, offset, bytes); // inlined dispatch
    }
    Impl& impl() { return static_cast<Impl&>(*this); }
};
```

Both `SharedMemBackend` and `UCXBackend` inherit from this class.
Hot path operations (`remote_write`, `remote_read`, `fence`) are resolved at compile time — zero overhead.

### SharedMemBackend (POSIX shm_open + mmap)

```
Node1                          Node2
 |  register_memory(buf)        |
 |  -> shm_open("/apex_1_ptr")  |
 |  -> mmap(MAP_SHARED)         |
 |  -> RemoteRegion{addr, rkey} |
 |                              |
 |  remote_write(src, region)   |
 |  -> memcpy(shm_ptr, src)     |  (reflected instantly via shared mapping)
 |                              |
 |  remote_read(region, dst)    |
 |  -> memcpy(dst, shm_ptr)     |
```

`remote_write/read` is implemented as `memcpy` internally,
but provides the same interface as one-sided RDMA operations in production.
In test environments, `fence()` uses `std::atomic_thread_fence(seq_cst)`.

### UCXBackend (Production)

When UCX headers are found, `APEX_HAS_UCX=1` flag is set automatically:

```cmake
if(APEX_USE_UCX AND UCX_FOUND)
    target_compile_definitions(apex_cluster PUBLIC APEX_HAS_UCX=1)
    target_link_libraries(apex_cluster PUBLIC PkgConfig::UCX)
endif()
```

Falls back to stub implementation to prevent compilation failure when UCX is absent.
Current environment (EC2) has UCX 1.12 installed — actual compilation is possible.

---

## Phase C-2: Cluster Core

### PartitionRouter — Consistent Hashing

The most critical component. Strategy: 1 physical node = 128 virtual nodes:

```
Hash ring (uint64 -> NodeId):
─────────────────────────────────────────
  0           ...          UINT64_MAX
  ├──●──●──●──●──●──●──●──●──●──●──●──●
  │  N1 N2 N1 N3 N2 N1 N3 N1 N2 N3 N2 N1
  └─ (each node evenly distributed via 128 virtual nodes)

Symbol -> hash -> upper_bound(ring) -> NodeId
                O(log n) lookup
```

Cache (`symbol -> NodeId`) makes hot path effectively O(1).

**Minimal Migration verification:**
- On node add: only the range covered by new node's vnodes is moved
- On node remove: only the removed node's range moves to the next node clockwise
- Zero node-to-node movement for other nodes (core property of consistent hashing)

Benchmark results:
```
PartitionRouter route() (cached):    500M ops/s   2.0 ns/op  OK
PartitionRouter route() (uncached):   29M ops/s  34.6 ns/op
```

### HealthMonitor — UDP Heartbeat

```
State transitions:
JOINING --heartbeat--> ACTIVE
ACTIVE --3s timeout--> SUSPECT
SUSPECT --heartbeat--> ACTIVE (recovery)
SUSPECT --7s more---> DEAD -> failover triggered
```

UDP heartbeat packet (24 bytes):
```cpp
struct HeartbeatPacket {
    uint32_t magic = 0x41504558;  // 'APEX'
    NodeId   node_id;
    uint64_t seq_num;
    uint64_t timestamp_ns;
};
```

For testing, `inject_heartbeat()` / `simulate_timeout()` API provided without UDP socket.
On UDP socket bind failure (port conflict etc.), gracefully disables receive-only, continues sending.

### ClusterNode<Transport> — The Unifying Class

```cpp
template <typename Transport>
class ClusterNode {
    Transport       transport_;   // SharedMem or UCX
    PartitionRouter router_;      // Consistent hash ring
    HealthMonitor   health_;      // UDP heartbeat
    ApexPipeline    local_pipeline_; // Local APEX pipeline
};
```

On `join_cluster()`:
1. Transport initialization (shm_open or ucp_init)
2. Register self + seed nodes in PartitionRouter
3. Connect to seed node
4. Start HealthMonitor (UDP thread)
5. Start local pipeline (drain thread)

On `ingest_tick(msg)`:
```
msg.symbol_id -> PartitionRouter.route() -> owner
if owner == self: local_pipeline_.ingest_tick(msg)
else:             transport_.remote_write(&msg, remote_region, ...)
```

---

## Lessons Learned

### 1. CRTP constructor access control

Initially declared `TransportBackend()` as `protected`.
Attempting to create `ClusterNode<TransportBackend<SharedMemBackend>>` caused a compile error.
Declaring `TransportBackend<SharedMemBackend>` directly as a member cannot access the protected constructor.

Fix: Changed to `public`, and use `SharedMemBackend` directly as the Transport type in `ClusterNode<SharedMemBackend>`.

### 2. Stack overflow (16MB!)

```
sizeof(ClusterNode<SharedMemBackend>) = 8,390,528 bytes  // ~8MB!
sizeof(ApexPipeline)                  = 8,389,312 bytes  // due to TickPlant 65K slots
```

Declaring two ClusterNodes on the stack in a test caused 16MB -> segfault.
Exceeded the OS default stack size of 8MB. UCX's signal handler caught this, producing confusing crash logs that initially looked like a UCX bug.

```cpp
// Fix: heap allocation
auto node1 = std::make_unique<ShmNode>(cfg1);
auto node2 = std::make_unique<ShmNode>(cfg2);
```

Lesson: **Always heap-allocate large objects in HFT systems.**
(ApexPipeline is this large because of the RingBuffer size designed in Phase A)

### 3. HealthMonitor state transition bug

After `simulate_timeout(node, 11000)`, calling `check_states_now()` once doesn't cause
ACTIVE -> SUSPECT -> DEAD to happen in one shot.

```cpp
// Wrong expectation:
monitor.simulate_timeout(200, 11000);
monitor.check_states_now();
EXPECT_EQ(get_state(200), NodeState::DEAD);  // FAIL: still SUSPECT

// Correct expectation:
monitor.simulate_timeout(200, 11000);
monitor.check_states_now();  // ACTIVE -> SUSPECT (age >= 3s)
EXPECT_EQ(get_state(200), NodeState::SUSPECT);
monitor.check_states_now();  // SUSPECT -> DEAD (age >= 10s)
EXPECT_EQ(get_state(200), NodeState::DEAD);
```

The state machine advances one step per `check_states_now()` call — two calls needed for two transitions.

---

## Benchmark Results Summary

```
Environment: EC2 (Amazon Linux 2023, Clang-19, Release -O3 -march=native)

SharedMem write+fence (64B):    73.9M ops/s   13.5 ns/op
SharedMem read (64B):            ~inf           ~0 ns/op
SharedMem bulk write (4KB):     14.9M ops/s   66.9 ns/op  = 61 GB/s
PartitionRouter (cached):      500.4M ops/s    2.0 ns/op
PartitionRouter (uncached):     28.9M ops/s   34.6 ns/op
Single-node ingest:              5.1M ops/s  195.7 ns/op
```

**SharedMem 13.5ns** — extremely fast compared to RDMA hardware (~1-15us).
In production, switching to UCXBackend adds network latency.

**PartitionRouter 2ns** — fully cached state. Uncached 34.6ns is also fast enough.

---

## Next Steps: Phase C-3 & C-4

- **C-3: AWS Integration** — EC2 Fleet API, DynamoDB metadata, EFA real tests
- **C-4: Distributed Query** — scatter-gather, partial VWAP aggregation, multi-node benchmarks

`ClusterNode::remote_query_vwap()` is currently a stub.
Will be implemented with gRPC or RDMA one-sided in C-3.

---

## File List

```
include/apex/cluster/
├── transport.h          # CRTP TransportBackend interface
├── partition_router.h   # Consistent Hashing (virtual nodes 128)
├── health_monitor.h     # UDP Heartbeat + state transitions
└── cluster_node.h       # ClusterNode<Transport> integration class

src/cluster/
├── shm_backend.h/cpp    # POSIX shm_open + mmap implementation
└── ucx_backend.h/cpp    # UCX RDMA implementation (APEX_HAS_UCX conditional)

tests/unit/test_cluster.cpp   # 8 unit tests
tests/bench/bench_cluster.cpp # 7 benchmarks
```

---

*Completing APEX-DB in order: Phase E -> B -> A -> D -> C.*
*Now moving beyond single machine to the cluster.*
