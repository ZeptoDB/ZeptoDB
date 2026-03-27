# Devlog 032: README Rebrand, Electric Indigo Theme, DATE_TRUNC, Multi-Node Cluster

> Date: 2026-03-27
> Status: Complete — 6 commits

---

## Overview

Major update spanning brand identity, SQL engine, cluster infrastructure, and build system. The README was fully rewritten to position ZeptoDB as an in-memory time-series database for high-throughput workloads, moving away from the kdb+ replacement narrative. Simultaneously, the codebase gained DATE_TRUNC GROUP BY support, hostname-based cluster connectivity, pyarrow build fallback, and the Electric Indigo design system.

---

## 1. README Full Rewrite

Repositioned ZeptoDB from "kdb+ replacement" to an independent time-series database identity.

| Before | After |
|--------|-------|
| "Ultra-Low Latency In-Memory Database" | "In-Memory Time-Series Database for High-Throughput Workloads" |
| kdb+ replacement rate tables at top | Removed — no competitor comparison |
| HFT-specific positioning | General-purpose time-series across 6 industries |
| Development Phases section | Removed — internal roadmap doesn't belong in README |

New sections added:
- **What is ZeptoDB?** — 3 paragraphs defining identity: in-memory, time-series native, HW/SW co-optimized
- **Key Design Principles** — 7 principles (in-memory first, time-series native, HW-optimized, SW-optimized, ingest+analyze, standard SQL, zero-copy Python)
- **Optimization Stack** — HW (SIMD, NUMA, RDMA, CXL, Arena) and SW (LLVM JIT, MPMC, columnar, partition-parallel, prefix-sum, dictionary) separated
- **Use Cases** — Finance, Quant, Crypto, IoT, Autonomous Vehicles, Observability

Core message: *"Ingest millions of events per second. Analyze them in microseconds."*

## 2. DATE_TRUNC in GROUP BY

Added `DATE_TRUNC('unit', column)` support in GROUP BY clauses.

```sql
SELECT DATE_TRUNC('min', timestamp) AS minute, SUM(volume) AS vol
FROM trades GROUP BY DATE_TRUNC('min', timestamp)
```

Supported units: `us`, `ms`, `s`, `min`, `hour`, `day`, `week` — all converted to nanosecond buckets at parse time.

| File | Change |
|------|--------|
| `include/zeptodb/sql/ast.h` | `date_trunc_buckets` vector in `GroupByClause` |
| `src/sql/parser.cpp` | Parse `DATE_TRUNC(string, col)` in GROUP BY |
| `src/sql/executor.cpp` | Apply dt_bucket floor in single/multi-column GROUP BY |
| `include/zeptodb/execution/query_scheduler.h` | `group_dt_bucket` in `QueryFragment` |

## 3. Cluster: Hostname Resolution + Admin API

**TCP RPC hostname support**: `TcpRpcClient::connect_to_server()` now falls back to `getaddrinfo()` when `inet_pton()` fails, enabling hostname-based cluster node addresses instead of requiring raw IPs.

**HTTP admin cluster awareness**: `/admin/cluster` now reports actual `mode` (standalone/cluster) and `node_count` from the coordinator when available, instead of hardcoded standalone.

**Log level tuning**: Health/ready/metrics/admin endpoint access logs demoted to DEBUG to reduce noise.

**Tests**: 770+ lines of multi-node HTTP cluster integration tests added to `test_cluster.cpp`.

## 4. Build: pyarrow Fallback for Parquet

When system Arrow/Parquet libraries are not found via CMake config or pkg-config, the build now detects pyarrow's bundled C++ libraries as a fallback:

```
python3 -c "import pyarrow; print(pyarrow.get_library_dirs()[0])"
```

Creates imported targets `PyArrow::arrow` and `PyArrow::parquet`, linked to `zepto_storage`. This eliminates the need for system-level Arrow installation when pyarrow is already available via pip.

## 5. Tools: Cluster Self-Registration

**zepto_data_node**: New flags `--node-id`, `--symbol`, `--coordinator`, `--api-key`. Nodes auto-register with the coordinator via HTTP POST on startup, enabling zero-config cluster bootstrapping.

**zepto_http_server**: New flags `--data-nodes`, `--api-key`. Automatically initializes a `QueryCoordinator` and connects to specified data nodes for scatter-gather query execution.

New docs: `docs/deployment/MULTI_NODE_CLUSTER.md`, `docs/PARQUET_S3_ACTIVATION.md`.

## 6. Electric Indigo Brand Redesign

Migrated the entire visual identity from Neon Cyan/Purple to Electric Indigo.

| Token | Old | New |
|-------|-----|-----|
| Primary | `#00E5FF` (Neon Cyan) | `#4D7CFF` (Electric Indigo) |
| Secondary | `#7C4DFF` (Electric Purple) | `#00F5D4` (Teal Mint) |
| Background | `#05080E` | `#0A0C10` (Deep Obsidian) |
| Surface | `rgba(11,17,29,0.7)` | `#11161D` (Carbon Dark) |
| Warning | `#FFC107` | `#FFB300` (Amber) |
| Error | `#FF2A55` | `#FF1744` (Laser Red) |

Applied across:
- `docs/brand_guidelines.md` — full English rewrite with Electric Indigo palette
- `docs/index.md` — redesigned landing page with styled hero and use-case cards
- `web/src/theme/theme.ts` — MUI theme updated
- `web/src/app/cluster/page.tsx` — border/status colors aligned
- `web/src/app/login/page.tsx`, `tables/page.tsx` — brand token consistency

---

## Commits

```
d1eef00 feat(sql): add DATE_TRUNC support in GROUP BY clause
88eafa6 feat(cluster): hostname resolution in TCP RPC + cluster-aware admin API
ce0ae90 build: fallback to pyarrow bundled Arrow/Parquet libraries
8fcccf1 feat(tools): cluster self-registration and multi-node deployment
72c99ba design: rebrand to Electric Indigo theme
9cab984 docs: rewrite README as in-memory time-series DB for high-throughput workloads
```
