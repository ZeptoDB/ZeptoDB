# 123: Agent Memory HNSW Backend

Date: 2026-05-28
Status: Complete

## Context

Sparse projection is fast to build, but recall is too workload-sensitive to make
it the default ANN path. The next comparison target was HNSW using the devlog 121
compare harness.

## Changes

- Added `ZEPTO_ENABLE_HNSWLIB=ON` CMake support for optional hnswlib integration.
  The default remains `OFF`, so normal builds do not fetch or expose hnswlib.
- Added an `AgentAnnIndex` backend interface and kept sparse projection as the
  default implementation.
- Added an optional HNSW backend using normalized vectors plus hnswlib `L2Space`;
  for unit vectors this preserves cosine-similarity ordering.
- Added `AgentMemoryAnnMode::Hnsw`, `--agent-memory-ann hnsw`, and benchmark
  parsing for `--ann hnsw`.
- Extended `bench_agent_memory --compare-ann` with `hnsw_fast`,
  `hnsw_recall`, HNSW tuning fields, and `--semantic-fixture`.
- Added HNSW unit coverage for tenant-partitioned search when hnswlib is enabled.

## Verification

- `ninja -j$(nproc) zepto_tests bench_agent_memory` with `ZEPTO_ENABLE_HNSWLIB=OFF`.
- `cmake -S . -B build -DZEPTO_ENABLE_HNSWLIB=ON`.
- `ninja -j$(nproc) zepto_tests bench_agent_memory` with `ZEPTO_ENABLE_HNSWLIB=ON`.
- `./tests/zepto_tests --gtest_filter="AgentMemoryStoreTest.HnswAnnSearchPreservesTenantPartition:AgentMemoryStoreTest.AnnSearchPreservesTenantPartitionAndRanking"`.
- `./bench_agent_memory --records 10000 --dim 64 --iters 3 --compare-ann --recall-queries 3`.
- `./bench_agent_memory --records 100000 --dim 128 --iters 5 --compare-ann --recall-queries 3`.
- `./bench_agent_memory --records 10000 --dim 64 --iters 3 --compare-ann --recall-queries 3 --semantic-fixture`.
- `./bench_agent_memory --records 100000 --dim 128 --iters 5 --compare-ann --recall-queries 3 --semantic-fixture`.

100K mixed-ranking fixture:

| Profile | Search p50 | Context p50 | Build | Recall avg | Recall min |
|---|---:|---:|---:|---:|---:|
| exact_scan | 8.46 ms | 6.71 ms | 0.00 ms | 1.000 | 1.000 |
| sparse_fast | 0.78 ms | 1.09 ms | 134.50 ms | 0.042 | 0.000 |
| sparse_wide | 5.96 ms | 5.90 ms | 395.00 ms | 0.333 | 0.312 |
| hnsw_fast | 0.07 ms | 0.44 ms | 14079.68 ms | 0.042 | 0.000 |
| hnsw_recall | 0.43 ms | 1.66 ms | 31039.75 ms | 0.062 | 0.000 |

100K semantic-only fixture:

| Profile | Search p50 | Context p50 | Build | Recall avg | Recall min |
|---|---:|---:|---:|---:|---:|
| exact_scan | 11.32 ms | 9.83 ms | 0.00 ms | 1.000 | 1.000 |
| sparse_fast | 0.87 ms | 1.29 ms | 161.05 ms | 0.396 | 0.188 |
| sparse_wide | 7.76 ms | 6.76 ms | 493.34 ms | 0.646 | 0.438 |
| hnsw_fast | 0.08 ms | 0.43 ms | 16317.66 ms | 0.458 | 0.188 |
| hnsw_recall | 0.48 ms | 1.79 ms | 35613.15 ms | 0.875 | 0.750 |

## Decision

HNSW is a strong semantic candidate generator, but it is not ready to become the
default Agent Memory ANN path. It wins query latency and semantic recall at the
`hnsw_recall` setting, but rebuild cost is orders of magnitude higher than sparse
projection, and mixed-ranking recall remains poor because final ranking also uses
importance, recency, pinned status, and access counts.

## Follow-ups

- Add clustered and real embedding fixtures before making any default-policy
  decision.
- Extend the append-only incremental path from devlog 125 with update/delete
  support instead of rebuilding on search after non-append mutations.
- Measure HNSW memory overhead and persisted sidecar footprint.
- Test an IVF/centroid baseline and mixed-ranking-aware candidate expansion.
