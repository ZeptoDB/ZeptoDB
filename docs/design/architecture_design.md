# Next-Generation HFT In-Memory DB: Core Engine and Architecture Design

> Based on the strategic goals in `initial_doc.md` (finance/HFT specialization, memory disaggregation, ultra-low latency, C++), this document presents a concrete architecture design to overcome existing KDB+ limitations and unify Research and Production.

---

## 1. Core Engine Architecture

We inherit KDB+'s philosophy of 'extreme simplicity' and 'data locality', but break through the physical limitations of the OS and single-node using modern cloud and hardware technologies.

### 1-A. Storage & Memory Manager (Global Memory Pooling)
- **Limitation to overcome:** Existing DBs (like kdb+) depend on OS virtual memory (`mmap`), resulting in catastrophic tail latency from page faults.
- **Design approach:**
  - **Memory Disaggregation:** Using CXL 3.0 and RDMA (AWS EFA/SRD), combine local RAM and RAM from multiple remote servers into a single **logical shared memory space (Global Shared Memory Pool)** (Kernel Bypass).
  - **Columnar Layout:** Adopt a pure columnar storage structure perfectly aligned (padded) to CPU cache line and SIMD register width. Minimizes cache misses.

### 1-B. Ingestion & Network Layer (Ultra-Low Latency Data Collection)
- **Design approach:**
  - **Zero-Copy RDMA Write:** Exchange market data (Ticks) are written directly to distributed memory space without CPU intervention the moment they arrive at the NIC (Network Interface Card).
  - **Lock-Free Queue:** Implement a Multi-Producer Multi-Consumer (MPMC) Ring Buffer in C++20/Rust for context-switch-free ultra-low latency ingestion architecture.

### 1-C. Query & Execution Engine (Vectorization and Computation Acceleration)
- **Design approach:**
  - **Vectorized Execution:** Pipeline queries to machine code via C++ template metaprogramming. Internalize SIMD instructions like ARM SVE (AWS Graviton4) or x86 AVX-512.
  - **Hardware Offloading:** Heavy mathematical operations like VWAP, Options Greeks calculation, and real-time FDS are **offloaded to CXL-connected FPGA/GPU accelerators**, freeing the main CPU.

### 1-D. Multi-Model Data Structures
- **Time-Series:** Timestamp-based Append-Only column arrays for fast write throughput.
- **Graph:** Sparse matrix compression (CSR) for fund flow and relationship tracking. Hybrid connection that blocks pointer-chasing latency by using only pointer offsets within time-series column indices.

---

## 2. Specialized Vision: Research-to-Production Integration Bridge Architecture

> Completely eliminates the industry's greatest bottleneck: **"wasted effort translating Python (quant research) → C++ (production deployment)"**. The core competitive advantage is drastically shortening Time-to-Market with a single technology stack.

### 2-A. Lazy Evaluation-Based DSL
- Quant researchers write Python code using the intuitive Pandas/Polars-style API (`db.filter(price > 100).rolling(1m).vwap()`).
- The Python engine does not compute immediately — it serves only as an interface, internally building an **Abstract Syntax Tree (AST, execution plan)** for data operations.

### 2-B. Runtime JIT Compilation (Using LLVM Core)
- At runtime, when the Python AST passes to the internal C++ core, the embedded **LLVM runtime JIT compiler** instantly translates and optimizes this logic into ultra-fast C++ machine code (hot-spot branch elimination, etc.).
- At this stage, SIMD and hardware offloading instructions — which Python fundamentally cannot support — are injected.

### 2-C. Perfect Zero-Copy Integration (Using pybind11) — ✅ Implemented

**Design goal:** Eliminate serialization/deserialization costs when the Python
research environment reads and analyzes C++ data by aligning memory layouts
100% (Apache Arrow style). Switch only memory pointer access rights without
copying data values.

**Actual implementation (`zepto_py` package — completed 2026-03-22):**

```
zepto_py/
├── dataframe.py    — from_pandas(), from_polars(), from_arrow(), from_polars_arrow()
│                     All paths use vectorized ingest_batch() — no Python row iteration
├── arrow.py        — ArrowSession: to_arrow(), to_duckdb(), to_polars_zero_copy()
│                     ingest_arrow_columnar(): pa.Array → numpy → ingest_batch()
├── streaming.py    — StreamingSession: batch ingest with progress callbacks
├── connection.py   — ApexConnection HTTP client → pandas/polars/numpy
└── utils.py        — check_dependencies(), versions()
```

Key implementation details:
- `pipeline.get_column()` returns a numpy array backed by the C++ Arrow buffer
  (zero-copy — no data moved, only a pointer shared via pybind11)
- `from_polars()`: `df.slice()` is zero-copy in Polars (Arrow view); `Series.to_numpy()`
  on numeric non-null series returns the Arrow buffer directly
- `from_arrow()`: `batch.to_numpy(zero_copy_only=False)` — zero-copy for contiguous
  non-null arrays; fills nulls with 0 via `pc.if_else()` before extraction
- `to_polars_zero_copy()`: `pl.from_arrow(table)` — both Polars and ZeptoDB use
  Arrow internally; no data is copied
- Test coverage: 208 tests across 5 test files, all passing

---

## 3. Security Architecture (Layer 5) — ✅ Implemented

Enterprise security is built as a cross-cutting layer that intercepts all HTTP API
requests before they reach any route handler.

### 3-A. Security Stack

```
HTTPS (TLS 1.2+, OpenSSL 3.2)
    │
    ▼
AuthManager::check()                ← set_pre_routing_handler
    ├── JwtValidator (HS256/RS256)   ← OIDC: Okta, Azure AD, Google
    ├── ApiKeyStore (SHA256 hash)    ← Service-to-service auth
    └── RBAC Engine                 ← 5 roles + symbol-level ACL
            │
            ▼
    AuditLogger (spdlog)            ← EMIR / MiFID II compliance
```

### 3-B. RBAC Layer (5 Roles)

| Role | Permissions | Typical user |
|------|------------|-------------|
| `admin` | ALL | DBA, ops team |
| `writer` | READ + WRITE | Feed handlers, pipelines |
| `reader` | READ | Quant researchers, BI |
| `analyst` | READ (symbol whitelist) | External/contracted analysts |
| `metrics` | /metrics only | Prometheus scraper |

### 3-C. Identity Priority

When a request carries a Bearer token:
1. Token starts with `ey` → try JWT/SSO first (human identity)
2. JWT invalid or not configured → try API key (service identity)
3. Both fail → `401 Unauthorized`

Public paths (`/ping`, `/health`, `/ready`) always bypass auth.

### 3-D. Enterprise Governance

Full policy documentation in `docs/design/layer5_security_auth.md`:
- API key lifecycle (create → active → revoke)
- Certificate rotation schedule (TLS: 90 days, mTLS node certs: 365 days)
- Access review cadence (monthly key inventory, quarterly RBAC review)
- Incident response procedures (key compromise, JWT secret compromise)
- Multi-tenant symbol isolation model
- Regulatory compliance mapping (EMIR, MiFID II, SOC2, ISO 27001)

---

## 4. Summary and Implemented Milestones

This architecture allows scripts written in Python — the language quant developers know — to be **immediately deployed to HFT at Ultra-Low Latency C++ level performance**, without a single line of manual translation.

**All core milestones completed:**
1. ✅ **Lock-Free MPMC Ring Buffer** (C++20, 5.52M ticks/sec)
2. ✅ **Arena Allocator** — columnar Arrow-layout memory, no malloc in hot path
3. ✅ **pybind11 Zero-copy Read API** — `get_column()` returns numpy view (522ns)
4. ✅ **zepto_py package** — full ecosystem: from_pandas/polars/arrow, ArrowSession,
   StreamingSession, ApexConnection (208 tests)
5. ✅ **Security Layer** — TLS/SSL + API Key + JWT/SSO + RBAC + Audit (37 tests)

*Last updated: 2026-03-23*
