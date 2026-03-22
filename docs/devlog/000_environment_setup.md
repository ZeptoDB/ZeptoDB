# DevLog #000: Environment Setup and Dependency Installation

**Date:** 2026-03-21 (KST 03-22 01:51)
**Author:** Gosengyi (AI Dev Assistant)

---

## 1. Host Environment

| Item | Value |
|---|---|
| OS | Amazon Linux 2023 (x86_64) |
| Kernel | 6.1.163-186.299.amzn2023 |
| CPU | Intel Xeon 6975P-C, 8 vCPU (4 core, HT) |
| RAM | 30 GiB |
| Disk | 200 GB NVMe (192 GB free) |
| GCC | 11.5.0 (C++20 support) |
| CMake | 3.22.2 |
| Python | 3.9.25 |
| Git | 2.50.1 |
| Rust | ❌ not installed |
| LLVM | ❌ not installed |

## 2. Required Packages and Installation Plan

### Phase 1 (Immediate install - dnf packages)
| Package | Version | Purpose |
|---|---|---|
| llvm19-devel | 19.1.7 | JIT compiler (Layer 3) |
| clang19 | 19.1.7 | C++20 compiler (better modern standard support than GCC 11) |
| highway-devel | 1.2.0 | SIMD abstraction (Layer 3) |
| numactl-devel | 2.0.14 | NUMA-aware memory allocation (Layer 1) |
| ucx-devel | 1.12.1 | RDMA/communication abstraction (Layer 2) |
| boost-devel | 1.75.0 | Utilities (lockfree, program_options, etc.) |
| ninja-build | - | Build acceleration |

### Phase 2 (Source build - vcpkg/CMake FetchContent)
| Library | Purpose |
|---|---|
| Apache Arrow (C++) | Columnar data format (Layer 1, 4) |
| FlatBuffers | AST serialization (Layer 4) |
| nanobind | Python C++ bindings (Layer 4) |
| Google Test | Unit testing |
| spdlog | Logging |
| fmt | Formatting |

### Phase 3 (As needed later)
| Item | Purpose |
|---|---|
| Rust (rustup) | Safe packet parsing (Layer 2) |
| DPDK | Kernel-bypass networking (Layer 2, requires real NIC) |

## 3. Installation Decision Rationale

- **LLVM 19** chosen: v20 is too new and stability unverified; v18 works but v19 is the balance point
- **Clang 19** chosen: GCC 11.5 has partial C++20 support (concepts, coroutines, etc.). Clang 19 has much more complete C++20/23 support
- **Highway** chosen: SIMD library specified in design docs. Runtime already on Amazon Linux
- **UCX** chosen: explicitly specified in docs — unified on-prem/cloud transport layer
- **Apache Arrow built from source**: not available in dnf. Managed via CMake FetchContent

## 4. Installation Log

(Actual installation output recorded below)

---
