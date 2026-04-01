# Per-Layer In-Memory DB Disaggregation — Impact Analysis

> What happens if each ZeptoDB layer runs as an independent in-memory DB process?

*Created: 2026-04-01*

---

## 1. Current Architecture (Monolithic In-Process)

Today all layers share a single address space:

```
┌─────────────────────────────────────────┐
│              Single Process              │
│                                          │
│  Layer 1  Storage   (Arena, ColumnStore) │
│  Layer 2  Ingestion (RingBuffer, WAL)    │
│  Layer 3  Execution (SIMD, JIT)          │
│  Layer 4  SQL/DSL   (Parser, Executor)   │
│  Layer 5  Server    (HTTP, Auth, Metrics)│
│  Layer 0  Cluster   (Transport, Router)  │
└─────────────────────────────────────────┘
```

Key property: all layers access the same Arena memory via raw pointers.
Zero-copy from ingestion → storage → execution → client response.

---

## 2. Proposed Model: Each Layer as a Separate In-Memory DB Process

```
┌──────────┐  IPC  ┌──────────┐  IPC  ┌──────────┐  IPC  ┌──────────┐
│ Ingestion│ ────→ │ Storage  │ ────→ │ Execution│ ────→ │ SQL/HTTP │
│ Process  │       │ Process  │       │ Process  │       │ Process  │
└──────────┘       └──────────┘       └──────────┘       └──────────┘
```

Each process owns its own memory arena and communicates via IPC
(shared memory, Unix sockets, or TCP).

---

## 3. Impact Analysis Per Layer

### 3.1 Layer 1 — Storage (Arena + ColumnStore + PartitionManager)

| Aspect | Impact | Severity |
|--------|--------|----------|
| Memory isolation | Arena is no longer directly accessible by Execution layer; every query requires IPC or shared-memory mapping | 🔴 Critical |
| Partition lifecycle | `seal()` / `reclaim_arena()` must be coordinated across process boundaries; race conditions on state transitions | 🔴 Critical |
| HDB flush | FlushManager can remain co-located — minimal impact if Storage owns persistence | 🟢 Low |
| Snapshot/recovery | Recovery reads snapshot into Storage process; other layers must wait — adds startup sequencing complexity | 🟡 Medium |
| Attribute indexes (s#/g#/p#) | Index structures stay local to Storage; Execution must request range lookups via RPC instead of direct `sorted_range()` calls | 🟡 Medium |

**Latency delta:** Current `get_column()` = pointer dereference (~1ns). With IPC = 0.5–5μs per call. **500–5000× regression on column access.**

### 3.2 Layer 2 — Ingestion (RingBuffer + TickPlant + WAL)

| Aspect | Impact | Severity |
|--------|--------|----------|
| Ring buffer → Storage path | Currently lock-free atomic write into Arena. Separate process requires serialization across process boundary | 🔴 Critical |
| WAL writes | WAL is sequential I/O — can stay co-located with Ingestion process; no impact | 🟢 Low |
| Feed handlers (FIX/ITCH/Kafka) | Feed decode stays in Ingestion process; decoded ticks must be forwarded to Storage process | 🟡 Medium |
| Tick ordering guarantee | Cross-process FIFO requires either shared-memory ring buffer (POSIX shm) or kernel-mediated ordering — adds ~200ns–1μs per tick | 🟡 Medium |
| Backpressure | Ring buffer overflow detection must span two processes; needs a shared atomic counter or flow-control protocol | 🟡 Medium |

**Throughput delta:** Current 5.52M ticks/sec. Cross-process serialization overhead estimates:
- POSIX shm ring buffer: ~3–4M ticks/sec (30–40% drop)
- Unix socket: ~1–2M ticks/sec (60–80% drop)
- TCP loopback: ~0.5–1M ticks/sec (80–90% drop)

### 3.3 Layer 3 — Execution (SIMD + JIT + JOIN)

| Aspect | Impact | Severity |
|--------|--------|----------|
| Data access pattern | Vectorized engine reads contiguous column spans (`std::span<T>`). Separate process means data must be copied or mapped into Execution's address space | 🔴 Critical |
| SIMD efficiency | SIMD requires cache-line aligned contiguous memory. If data arrives via IPC copy, alignment must be re-guaranteed; if via shm mmap, TLB pressure increases | 🔴 Critical |
| JIT compilation | LLVM JIT operates on local memory — no direct impact on codegen. But JIT-compiled functions can no longer directly dereference Storage pointers | 🟡 Medium |
| Parallel scan | `ParallelScanExecutor` scatter/gather assumes shared address space. Cross-process scatter requires distributed task coordination | 🔴 Critical |
| JOIN operations | ASOF JOIN two-pointer scan requires simultaneous access to two tables. Cross-process = 2× IPC round-trips per probe | 🔴 Critical |
| Window functions | Sliding window (wj) needs sequential column access — same IPC penalty as filter | 🟡 Medium |

**Latency delta:**
- Filter 1M rows: 272μs → estimated 2–10ms (7–37× regression)
- VWAP 1M rows: 532μs → estimated 5–20ms (10–38× regression)
- ASOF JOIN: O(n) scan becomes O(n) IPC calls if not bulk-transferred

### 3.4 Layer 4 — SQL Parser + Query Planner

| Aspect | Impact | Severity |
|--------|--------|----------|
| Parse latency | Parser is CPU-only (no data access). Running separately has negligible impact on parse time (1.5–4.5μs) | 🟢 Low |
| Plan → Execution handoff | AST/physical plan must be serialized and sent to Execution process. Plan objects are small — ~100 bytes typical | 🟢 Low |
| Schema metadata | Parser needs table/column metadata for validation. Must fetch from Storage process or cache locally | 🟡 Medium |
| Query routing | `QueryScheduler` dispatch becomes cross-process RPC instead of function call | 🟡 Medium |

**Latency delta:** +10–50μs per query for plan serialization + RPC. Acceptable for most workloads.

### 3.5 Layer 5 — HTTP Server + Auth + Metrics

| Aspect | Impact | Severity |
|--------|--------|----------|
| Request routing | HTTP → SQL → Execution becomes a 3-hop RPC chain instead of in-process function calls | 🟡 Medium |
| Auth/RBAC | Security checks are stateless (JWT verify, API key lookup). Can run independently with no data dependency | 🟢 Low |
| Metrics collection | Prometheus metrics must aggregate across all processes. Requires a metrics aggregation layer or per-process `/metrics` endpoints | 🟡 Medium |
| Access logging | `util::Logger` access log stays local to HTTP process — no impact | 🟢 Low |

**Latency delta:** +50–200μs per request for the full RPC chain (HTTP → SQL → Execution → Storage → back).

### 3.6 Layer 0 — Cluster (Transport + Router + Health)

| Aspect | Impact | Severity |
|--------|--------|----------|
| Partition routing | `PartitionRouter` must know which Storage process owns which partition. Adds a local routing layer on top of cluster routing | 🟡 Medium |
| Replication | WAL replication from Ingestion → remote node. If Ingestion is separate, replication source changes but protocol stays the same | 🟢 Low |
| Health monitoring | Must monitor N processes per node instead of 1. Heartbeat complexity increases linearly | 🟡 Medium |
| Failover | Partial process failure (e.g., Execution crashes but Storage survives) creates new failure modes not present in monolithic model | 🔴 Critical |

---

## 4. Cross-Cutting Concerns

### 4.1 Zero-Copy Destruction

The single most impactful consequence. ZeptoDB's core performance advantage is zero-copy data flow:

```
Feed → RingBuffer → Arena → ColumnVector → SIMD Engine → pybind11 → numpy
         all raw pointer dereferences, same address space
```

Splitting into processes **breaks every `→` in this chain**. Each boundary requires either:
- **Shared memory mapping** (POSIX shm / mmap): preserves zero-copy but introduces TLB pressure, page fault risk, and complex lifecycle management
- **Data copy over IPC**: simple but destroys the latency advantage

### 4.2 Failure Mode Explosion

| Monolithic | Disaggregated |
|------------|---------------|
| 1 process: alive or dead | N processes: partial failure states |
| Restart = full recovery | Must handle: Storage up + Execution down, Ingestion up + Storage down, etc. |
| No IPC timeout | Every cross-process call needs timeout + retry + circuit breaker |

### 4.3 Operational Complexity

| Metric | Monolithic | Disaggregated |
|--------|-----------|---------------|
| Processes per node | 1 | 4–6 |
| Config files | 1 | 4–6 (+ IPC endpoint config) |
| Log streams | 1 (structured JSON) | 4–6 (need correlation IDs) |
| Deployment artifacts | 1 binary | 4–6 binaries |
| K8s pods per node | 1 | 4–6 (sidecar or multi-container) |
| Debugging | gdb attach 1 process | distributed tracing required |

### 4.4 Memory Overhead

Each process needs its own:
- Arena allocator (cannot share across process boundaries without shm)
- Thread pools
- LLVM JIT context (~50MB per process)
- librdkafka / libucx state

Estimated overhead: +200–500MB per node for process duplication.

---

## 5. Quantified Performance Impact Summary

| Metric | Current (Monolithic) | Disaggregated (shm) | Disaggregated (TCP) |
|--------|---------------------|---------------------|---------------------|
| Tick ingest | 5.52M/sec | ~3.5M/sec | ~0.8M/sec |
| Filter 1M rows | 272μs | ~1–3ms | ~5–15ms |
| VWAP 1M rows | 532μs | ~2–5ms | ~10–30ms |
| SQL parse | 1.5–4.5μs | 1.5–4.5μs (no change) | 1.5–4.5μs (no change) |
| Python zero-copy | 522ns | ~5–10μs (shm remap) | N/A (copy required) |
| E2E query latency | ~1ms | ~3–8ms | ~20–50ms |

---

## 6. When Disaggregation Makes Sense

Despite the costs, per-layer separation can be justified in specific scenarios:

| Scenario | Rationale |
|----------|-----------|
| Multi-tenant isolation | Storage process per tenant prevents noisy-neighbor memory pressure |
| Independent scaling | Scale Execution horizontally (more SIMD workers) without scaling Storage |
| Security boundary | Ingestion (untrusted feed data) isolated from Storage (sensitive positions) |
| Language heterogeneity | SQL/HTTP layer in Go/Rust while Execution stays C++ |
| Fault containment | JIT crash in Execution doesn't kill Storage — data survives |

### Recommended Hybrid Approach

Rather than full disaggregation, a **2-process model** preserves most benefits:

```
┌─────────────────────────────┐    shm    ┌──────────────────┐
│ Data Plane (hot path)       │ ────────→ │ Control Plane    │
│ Ingestion + Storage +       │           │ HTTP + Auth +    │
│ Execution (single process)  │           │ Metrics + SQL    │
└─────────────────────────────┘           └──────────────────┘
```

- Hot path stays zero-copy in one process
- Control plane (cold path) is isolated — crash doesn't lose data
- HTTP/Auth can be restarted independently
- Metrics aggregation is trivial (2 processes)

---

## 7. Conclusion

Full per-layer disaggregation **destroys ZeptoDB's core value proposition** (sub-microsecond zero-copy data flow). The 10–50× latency regression on query execution makes it unsuitable for HFT workloads.

If process isolation is required, the **2-process hybrid model** (§6) retains >90% of monolithic performance while gaining fault isolation and independent deployment of the control plane.

---

*Related docs:*
- `high_level_architecture.md` — layer overview
- `layer1_storage_memory.md` — Arena/ColumnStore design
- `layer3_execution_engine.md` — SIMD/JIT execution
- `phase_c_distributed.md` — cluster transport (already cross-process)
