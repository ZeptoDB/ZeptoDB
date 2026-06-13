# 170: P3 Agent Memory Clustered ANN Fixture

Date: 2026-06-09
Status: Complete

## Context

The Agent Memory ANN compare harness already supported exact scan,
sparse-projection, and optional HNSW profiles, but its fixtures were either
mixed-ranking random vectors or a semantic-only mode over random vectors. That
made ANN recall repeatable but not representative of real embedding spaces,
where related memories tend to form local neighborhoods. The stronger ANN track
needed a clustered fixture before any default-policy decision.

## Changes

- Added `bench_agent_memory --fixture mixed|semantic|clustered`.
- Preserved `--semantic-fixture` as a compatibility alias for
  `--fixture semantic`.
- Added `--clusters N` and `--cluster-noise X` for deterministic clustered
  embeddings.
- Generated clustered record embeddings from a deterministic center vector plus
  deterministic noise, with tenant-balanced cluster assignment.
- Generated recall query vectors from the same fixture distribution so exact
  scan, sparse projection, and HNSW compare the same semantic neighborhoods.
- Printed the fixture name in benchmark setup output and the ANN decision table.
- Updated the Agent Memory design and backlog docs to narrow the stronger ANN
  row from synthetic fixture work toward real-vector and production-dump
  evaluation. Devlog 171 later added the real-vector file fixture.

## Verification

- `cmake --build build --target bench_agent_memory -j$(nproc)`
- `./build/bench_agent_memory --records 1000 --dim 16 --iters 1 --compare-ann --recall-queries 2 --fixture clustered --clusters 8 --cluster-noise 0.03 --skip-snapshot`
- `./build/bench_agent_memory --records 10000 --dim 64 --iters 2 --compare-ann --recall-queries 3 --fixture clustered --clusters 32 --cluster-noise 0.04 --skip-snapshot`

10K clustered fixture sample on the current local build:

| Profile | Search p50 | Context p50 | ANN build | Recall@16 avg/min |
|---|---:|---:|---:|---:|
| exact_scan | 0.72 ms | 0.41 ms | 0.00 ms | 1.000 / 1.000 |
| sparse_fast | 0.08 ms | 0.14 ms | 7.98 ms | 1.000 / 1.000 |
| sparse_wide | 0.39 ms | 0.38 ms | 17.68 ms | 1.000 / 1.000 |
| hnsw_fast | 0.03 ms | 0.14 ms | 303.94 ms | 1.000 / 1.000 |
| hnsw_recall | 0.11 ms | 0.51 ms | 632.54 ms | 1.000 / 1.000 |

## Follow-ups

- Real embedding file fixtures shipped in devlog 171; run larger production
  embedding dumps before changing the default ANN policy.
- Measure HNSW memory overhead and persisted sidecar footprint.
- Add update/delete maintenance or tombstone compaction for non-append ANN
  mutations.
- Add an IVF/centroid baseline and mixed-ranking-aware candidate expansion.
