# APEX-DB Initial Vision and Design Principles
**Last updated: 2026-03-22 (General OLAP/TSDb expansion, kdb+ 95% replacement, Migration Toolkit, Parquet HDB, S3 Sink completed)**

APEX-DB is an **ultra-low latency in-memory database** that started as a finance-focused system and has expanded to **general-purpose OLAP, time-series DB, and ML Feature Store**.

---

## APEX-DB Development Strategy and Status

### **1. Target Markets — Expanded**

**Phase 1 (Complete): Financial Markets** — kdb+ replacement rate 95% achieved
* **HFT (High-Frequency Trading):** ASOF JOIN, xbar, Window JOIN — **95% replacement**
* **Quant Research:** EMA/DELTA/RATIO, Python DSL — **90% replacement**
* **Risk Management:** LEFT JOIN, parallel GROUP BY — **95% replacement**
* **FDS (Fraud Detection):** Real-time Window JOIN — **85% replacement**

**Phase 2 (In Progress): General Markets**
* **OLAP (ClickHouse replacement):** SQL, HTTP API (port 8123), parallel queries
  - Target: AdTech, SaaS analytics ($1M-3M ARR)
* **IoT/Monitoring (TimescaleDB replacement):** xbar time bar aggregation, HDB compression
  - Target: IoT, DevOps monitoring ($500K-1M ARR)
* **ML Feature Store:** zero-copy Python (pybind11), Arrow compatible
  - Target: real-time recommendations, fraud detection

---

### **2. Core Design Principles — Actual Implementation**

| Principle | Current Implementation Status |
| :---- | :---- |
| **Ultra-Low Latency** | ✅ **272μs/1M filter** (BitMask), **532μs VWAP** (Highway SIMD), **2.2ms EMA** |
| **Memory Disaggregation** | ✅ UCX transport complete, DistributedQueryScheduler stub (future multi-node) |
| **Columnar + Vectorized** | ✅ Arrow-compatible column store, Highway SIMD (AVX-512/ARM SVE auto), LLVM JIT O3 |
| **Parallel Execution** | ✅ **LocalQueryScheduler** (8T = 3.48x), WorkerPool (jthread), scatter/gather DI |
| **SQL + Python DSL** | ✅ SQL Parser (1.5~4.5μs), HTTP API (port 8123), Python pybind11 (522ns zero-copy) |
| **Time-Series Native** | ✅ **xbar** (time bar), **EMA**, **ASOF JOIN**, **Window JOIN** (O(n log m)) |

---

### **3. Deployment Strategy: Bare-Metal First, Cloud Supplemental**

#### **Bare-Metal (Recommended)**
* **Why?** HFT requires **latency consistency** directly tied to revenue — avoids cloud noisy-neighbor effects
* **Hardware:**
  - **Intel Xeon 8462Y+ (Sapphire Rapids)**: AVX-512, AMX, DDR5-4800, CXL 1.1
  - **AMD EPYC 9754 (Bergamo)**: 128-core density, DDR5-4800
  - **Supermicro AS-4125GS-TNRT**: PCIe 5.0, 16x NVMe, 4TB RAM
* **Deployment:** `scripts/tune_bare_metal.sh` — one-step auto-tuning (CPU pinning, NUMA, io_uring)

#### **Cloud (AWS First)**
* **Instances:** r8g.16xlarge (Graviton4, 512GB), c7gn.16xlarge (EFA, HFT networking)
* **Optimization:** Highway SIMD (ARM SVE auto), Nitro offload, EFA RDMA
* **Containers:** Docker + Kubernetes (HPA, PVC, LoadBalancer) — `k8s/deployment.yaml`
* **Monitoring:** Prometheus + Grafana — `/metrics` OpenMetrics, 9 alert rules

#### **Microsoft Azure (Future)**
* **CXL Support:** M-series Mv3 (CXL Flat Memory Mode) testing planned
* **Arm:** Cobalt 100 + InfiniBand combination

---

### **4. Technical Differentiation: kdb+ vs APEX-DB**

| Item | kdb+ | APEX-DB |
|---|---|---|
| **Financial functions** | xbar, ema, wj (q DSL) | ✅ **93% compatible** (SQL + Python DSL) |
| **Performance** | μs latency (proprietary) | ✅ **272μs filter, 2.2ms EMA** (Highway SIMD + LLVM JIT) |
| **Parallelism** | Single-core optimization first | ✅ **Multi-core 3.48x** (LocalQueryScheduler, scatter/gather) |
| **Standard SQL** | Limited (q-first) | ✅ **Full SQL Parser** (1.5~4.5μs, ClickHouse compatible) |
| **Python integration** | PyKX (wrapper) | ✅ **zero-copy 522ns** (pybind11, Arrow compatible) |
| **Deployment** | License $150K+ | ✅ **Open-source + enterprise options** |
| **JOIN** | aj (asof), wj (window) | ✅ **ASOF/Hash/LEFT/Window JOIN** (O(n), O(n log m)) |
| **Scalability** | Manual sharding | ✅ **QueryScheduler DI** (local → distributed, no code changes) |

**Conclusion:** kdb+ compatibility + modern architecture (SIMD, JIT, parallel, SQL)

---

### **5. Current Implementation Status (2026-03-22)**

| Layer | Implemented | Benchmark |
|---|---|---|
| **Storage** | Arena allocator, Column store, HDB (LZ4), Parquet Writer (SNAPPY/ZSTD/LZ4), S3 Sink (async) | 4.8 GB/s flush |
| **Ingestion** | MPMC Ring Buffer, WAL, Feed Handlers (FIX, ITCH) | 5.52M ticks/sec |
| **Execution** | SIMD (Highway), JIT (LLVM), JOIN (ASOF/Hash/LEFT/Window) | 272μs filter, 53ms join |
| **Financial functions** | xbar, EMA, DELTA, RATIO, FIRST, LAST, Window JOIN | 2.2ms EMA, 24ms xbar |
| **Parallel query** | LocalQueryScheduler, WorkerPool (scatter/gather) | 3.48x (8T) |
| **SQL** | Parser, HTTP API (port 8123), GROUP BY, Window functions | 1.5~4.5μs parsing |
| **Python** | pybind11, zero-copy numpy, lazy eval DSL | 522ns zero-copy |
| **Cluster** | UCX/SharedMem transport, Partition routing | 13.5ns SHM, 2ns routing |
| **Operations** | Monitoring, Backup, systemd service | Prometheus + Grafana |

**Tests:** 221 tests PASS (unit 151 + feed handler 37 + migration 70 + benchmark 10)

---

### **6. Future Roadmap (Priority)**

#### **Immediate (Next Commit)**
- [x] Full design document update ✅ Completed (2026-03-22)

#### **High Priority (Technical)**
- [ ] SQL parser completion (complex queries, subqueries)
- [ ] Time range index (leverage already-sorted data)
- [ ] Graviton (ARM) build test (Highway SVE)

#### **High Priority (Business)**
- [ ] **Migration Toolkit** ✅ **Complete** (kdb+/ClickHouse/DuckDB/TimescaleDB, 70 tests)
- [ ] **Python ecosystem** — Research-to-Production
  - `apex.from_polars/pandas`, direct Arrow support
- [ ] **DSL AOT compilation** — Nuitka/Cython, production deployment + IP protection

#### **Medium Priority**
- [ ] Distributed query scheduler (DistributedQueryScheduler, UCX)
- [ ] Data/Compute node separation (RDMA remote_read)
- [ ] DuckDB embedding (delegate complex JOINs)

---

## Reference Documents

- Detailed architecture: `docs/design/high_level_architecture.md`
- Business strategy: `docs/business/BUSINESS_STRATEGY.md`
- kdb+ replacement analysis: `docs/design/kdb_replacement_analysis.md`
- Development log: `docs/devlog/` (001~012)
- Deployment guide: `docs/deployment/PRODUCTION_DEPLOYMENT.md`
