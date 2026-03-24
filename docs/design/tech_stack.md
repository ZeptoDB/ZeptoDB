# ZeptoDB — Tech Stack & Build Requirements

**Last updated:** 2026-03-23

---

## Overview

ZeptoDB is a C++20 in-memory columnar time-series database targeting ultra-low latency financial workloads. The build system is CMake + Ninja. All dependencies are either fetched automatically via CMake FetchContent or installed via the system package manager.

---

## 1. Language & Compiler

| Item | Requirement |
|------|-------------|
| **Language standard** | C++20 (required: concepts, `std::filesystem`, `std::span`, `std::optional`, structured bindings, designated initializers) |
| **Compiler (recommended)** | `clang++` 19 — `/usr/bin/clang++-19` |
| **Compiler (alternative)** | `g++` 14+ (C++20 support) |
| **Minimum CMake** | 3.22 |
| **Build system** | Ninja (preferred) or Make |
| **Linker** | `bfd` (lld-19 not required; `ld.bfd` from `binutils`) |

> **Note on lld:** lld-19 is not installed on the reference EC2 instance. Use `-DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=bfd"` explicitly.

---

## 2. System-Level Dependencies (OS packages)

Install on Amazon Linux 2023 / RHEL 9:

```bash
# Core build tools
sudo dnf install -y cmake ninja-build clang19 gcc14 g++14 binutils

# Required: NUMA (memory topology)
sudo dnf install -y numactl-devel

# Required: SIMD library
sudo dnf install -y highway-devel          # libhwy

# Required: UCX (RDMA/TCP cluster transport)
sudo dnf install -y ucx-devel              # libucs, libuct, libucp, libucm

# Required: LLVM 19 (JIT engine)
sudo dnf install -y llvm19-devel           # /usr/lib64/llvm19/

# Required: LZ4 (HDB column file compression)
sudo dnf install -y lz4-devel              # liblz4

# Required: OpenSSL 3.x (TLS + JWT/OIDC auth)
sudo dnf install -y openssl-devel          # OpenSSL 3.2+

# Optional: Apache Arrow + Parquet (Parquet HDB output, Python bridge)
sudo dnf install -y arrow-devel parquet-devel

# Optional: AWS SDK C++ S3 component (HDB → S3 upload)
sudo dnf install -y aws-sdk-cpp-s3

# Optional: Apache Kafka consumer
sudo dnf install -y librdkafka-devel

# Optional: tcmalloc (HFT allocation performance)
sudo dnf install -y gperftools-devel

# Optional: readline (zepto-cli interactive mode)
sudo dnf install -y readline-devel

# Optional: Python binding
sudo dnf install -y python3-devel
pip install pybind11 numpy
```

---

## 3. FetchContent Dependencies (auto-downloaded at cmake time)

These are fetched automatically — no manual install required.

| Library | Version | Purpose |
|---------|---------|---------|
| **fmtlib/fmt** | 11.1.4 | String formatting (used by spdlog) |
| **gabime/spdlog** | v1.15.1 | Structured logging |
| **google/googletest** | v1.15.2 | Unit test framework |
| **google/flatbuffers** | v24.12.23 | AST serialization (Layer 4) |

---

## 4. CMake Build Options

All options default to `ON` unless noted. Disable when the library is not installed.

| CMake Flag | Default | Description |
|------------|---------|-------------|
| `APEX_BUILD_TESTS` | ON | Build `zepto_tests` test binary |
| `APEX_BUILD_BENCH` | ON | Build benchmark binaries |
| `APEX_USE_HUGEPAGES` | ON | Linux HugePage support (2MB pages, `vm.nr_hugepages`) |
| `APEX_USE_UCX` | ON | UCX transport (TCP/RDMA/CXL cluster) |
| `APEX_USE_JIT` | ON | LLVM JIT engine (requires `llvm19-devel`) |
| `APEX_USE_HIGHWAY` | ON | Highway SIMD (AVX2/AVX-512/SVE) |
| `APEX_USE_LZ4` | ON | LZ4 column file compression |
| `APEX_USE_PARQUET` | ON | Apache Parquet HDB output |
| `APEX_USE_S3` | ON | AWS S3 HDB upload |
| `APEX_USE_TCMALLOC` | **OFF** | tcmalloc_minimal (HFT workloads) |
| `APEX_USE_LTO` | **OFF** | Link-Time Optimization (`-flto`) |
| `APEX_USE_KAFKA` | **OFF** | Apache Kafka consumer |
| `APEX_BUILD_PYTHON` | ON | pybind11 Python module |

---

## 5. Minimal Build (test environment, no optional deps)

The configuration used on the reference EC2 instance:

```bash
mkdir -p build_clang && cd build_clang

cmake .. \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-19 \
  -DCMAKE_C_COMPILER=/usr/bin/clang-19 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-O2 -march=native" \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=bfd" \
  -DAPEX_USE_PARQUET=OFF \
  -DAPEX_USE_S3=OFF \
  -DAPEX_BUILD_PYTHON=OFF \
  -GNinja

ninja -j$(nproc) zepto_tests
```

Run all tests:

```bash
./tests/zepto_tests
# Expected: 615/615 PASSED (as of 2026-03-23)
```

---

## 6. Full Production Build (PGO + LTO + tcmalloc)

Best performance configuration (requires PGO profile collected first):

```bash
# Step 1: Collect PGO profile (instrument build)
cmake .. \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-19 \
  -DCMAKE_C_COMPILER=/usr/bin/clang-19 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-O3 -march=native -fprofile-instr-generate=/tmp/zepto_pgo.profraw" \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=bfd" \
  -DAPEX_USE_PARQUET=OFF -DAPEX_USE_S3=OFF -DAPEX_BUILD_PYTHON=OFF \
  -GNinja

ninja -j$(nproc) zepto_tests
./tests/zepto_tests --gtest_filter="Benchmark.*:SqlExecutor*:FinancialFunction*:WindowJoin*"
llvm-profdata merge -o /tmp/zepto_pgo.profdata /tmp/zepto_pgo.profraw

# Step 2: Optimized build
cmake .. \
  -DCMAKE_CXX_FLAGS="-O3 -march=native -flto -fprofile-use=/tmp/zepto_pgo.profdata -fprofile-correction" \
  -DCMAKE_EXE_LINKER_FLAGS="-flto -fuse-ld=bfd" \
  -DAPEX_USE_TCMALLOC=ON \
  -DAPEX_USE_PARQUET=OFF -DAPEX_USE_S3=OFF -DAPEX_BUILD_PYTHON=OFF \
  -GNinja

ninja -j$(nproc) zepto_tests
```

---

## 7. Runtime Requirements

### Kernel / OS

| Setting | Value | Why |
|---------|-------|-----|
| **Linux kernel** | ≥ 5.15 | `io_uring`, HugePage support |
| **OS** | Amazon Linux 2023 / RHEL 9 / Ubuntu 22.04+ | |
| **HugePages** | `vm.nr_hugepages = 4608` (9GB) | 32MB arena × ~278 partitions |
| **Swappiness** | `vm.swappiness = 1` | Prevent swap-induced latency |
| **NUMA balancing** | `vm.numa_balancing = 0` | Avoid page migrations |
| **THP** | `transparent_hugepage = never` | Avoid THP defrag stalls |
| **C-states 2-9** | Disabled | Reduce wake-up latency |
| **ASLR** | **Keep enabled (= 2)** | Disabling causes L3 cache aliasing (+4ms) |

Apply at boot (add to `/etc/sysctl.d/zeptodb.conf`):

```
vm.nr_hugepages = 4608
vm.swappiness = 1
vm.numa_balancing = 0
vm.stat_interval = 120
vm.zone_reclaim_mode = 0
kernel.watchdog = 0
kernel.nmi_watchdog = 0
kernel.timer_migration = 0
```

Disable C-states at runtime:

```bash
for i in /sys/devices/system/cpu/cpu*/cpuidle/state{2..9}/disable; do
    echo 1 > "$i" 2>/dev/null || true
done
```

### Shared Libraries at Runtime

When deploying binaries to a node that doesn't have the build toolchain:

```bash
# Required shared libs (copy from build node if not installed)
libLLVM-19.so.1      # LLVM JIT
libucs.so.0          # UCX
libuct.so.0          # UCX
libucp.so.0          # UCX
libucm.so.0          # UCX
liblz4.so.1          # LZ4
libssl.so.3          # OpenSSL
libcrypto.so.3       # OpenSSL

# Set on target node if libs are in a non-standard location
export LD_LIBRARY_PATH=/path/to/zepto_libs:$LD_LIBRARY_PATH
```

---

## 8. Python Binding Requirements

| Item | Version |
|------|---------|
| Python | 3.9+ |
| pybind11 | 2.13+ (`pip install pybind11`) |
| NumPy | 1.24+ (`pip install numpy`) |
| Optional | Polars, pandas, pyarrow (for `from_polars()` / `from_pandas()` / `to_arrow()`) |

Build with Python enabled:

```bash
cmake .. -DAPEX_BUILD_PYTHON=ON ...
# Produces: build_clang/zeptodb.cpython-3xx-x86_64-linux-gnu.so
```

---

## 9. Cluster / Networking Requirements

For multi-node deployments:

| Requirement | Detail |
|-------------|--------|
| **TCP** | Default transport; ports 9100–9200 range (configurable) |
| **UCX** | Optional RDMA/InfiniBand/EFA transport; requires UCX 1.14+ |
| **AWS EFA** | For EC2 hpc6a/hpc7g instances with InfiniBand-like latency |
| **VPC / Security Group** | Open TCP ports 9100–9200 between cluster nodes |
| **NTP / Chrony** | Wall-clock sync required for ASOF JOIN correctness (< 100ms skew) |
| **PTP (optional)** | Sub-microsecond sync for strict ASOF JOIN mode |

---

## 10. Disk Requirements (HDB / Snapshot)

| Mode | Disk Required | Format |
|------|--------------|--------|
| `PURE_IN_MEMORY` | None (optional for snapshot) | — |
| `TIERED` | Yes (HDB base path) | Binary `.bin` (LZ4) + optional Parquet |
| `PURE_ON_DISK` | Yes | Same as above |
| Snapshot path | Separate from HDB or same | Binary `.bin` |

Minimum per production node: **SSD preferred** for HDB flush throughput (4.8 GB/s measured on NVMe).

---

## 11. Library Dependency Graph

```
zepto_tests
  └─ zepto_sql          ← SQL parser + executor
       └─ zepto_core    ← Pipeline integration
            ├─ zepto_storage   ← Arena, ColumnStore, HDB, FlushManager, SchemaRegistry
            ├─ zepto_ingestion ← TickPlant, RingBuffer, WAL
            └─ zepto_execution ← VectorizedEngine, SIMD, JIT, ParallelScan

  └─ zepto_cluster      ← TCP RPC, QueryCoordinator, PartitionRouter
       └─ zepto_core + zepto_sql

  └─ zepto_auth         ← RBAC, API Key, JWT, TLS
  └─ zepto_server       ← HTTP API (cpp-httplib), /metrics, /query
  └─ zepto_kafka        ← Kafka consumer (optional librdkafka)
  └─ zepto_scheduler    ← Timer/cron jobs
  └─ zepto_feeds        ← FIX, NASDAQ ITCH parsers
  └─ zepto_migration    ← kdb+/q, ClickHouse, DuckDB, TimescaleDB
```

Third-party (header-only, bundled in `third_party/`):

| Library | Version | Purpose |
|---------|---------|---------|
| **cpp-httplib** | 0.18+ | HTTP/HTTPS server (`third_party/httplib.h`) |
| **nlohmann/json** | 3.11+ | JSON parsing for HTTP API (`third_party/json.hpp`) |
