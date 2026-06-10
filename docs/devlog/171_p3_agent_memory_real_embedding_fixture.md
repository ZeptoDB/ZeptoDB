# 171: P3 Agent Memory Real Embedding Fixture

Date: 2026-06-09
Status: Complete

## Context

The Agent Memory ANN compare harness can now compare exact scan, sparse
projection, and optional HNSW on mixed, semantic-only, and clustered synthetic
fixtures. The remaining fixture gap was a way to feed precomputed vectors from a
real embedding pipeline into the same benchmark without changing C++ code.

## Changes

- Added `bench_agent_memory --fixture real --embedding-file PATH`.
- Added a vector-only embedding-file parser that accepts comma, semicolon, or
  whitespace-separated finite floats, optional `[...]` brackets, blank lines,
  and `#` comments.
- Validated every row has the same embedding dimension and that `--records`
  does not exceed the number of loaded vectors.
- Defaulted `--records` to the loaded vector count when it is not specified and
  set `--dim` from the file unless the caller provided a matching explicit
  dimension.
- Reused loaded vectors for seeded memories, cache entries, and recall queries
  so exact scan and ANN profiles compare the same real-vector neighborhood.
- Added `tests/fixtures/agent_memory_real_embeddings.txt` as a small smoke
  fixture for the new input path.
- Kept `--semantic-fixture` as a compatibility alias for `--fixture semantic`
  and kept synthetic `mixed` / `clustered` fixtures available.

## Verification

- `cmake --build build --target bench_agent_memory -j$(nproc)`
- `./build/bench_agent_memory --fixture real --embedding-file tests/fixtures/agent_memory_real_embeddings.txt --iters 1 --compare-ann --recall-queries 2 --skip-snapshot`

Small real-fixture smoke on the current local build:

| Profile | Search p50 | Context p50 | ANN build | Recall@16 avg/min |
|---|---:|---:|---:|---:|
| exact_scan | 0.01 ms | 0.01 ms | 0.00 ms | 1.000 / 1.000 |
| sparse_fast | 0.01 ms | 0.01 ms | 0.02 ms | 1.000 / 1.000 |
| sparse_wide | 0.01 ms | 0.01 ms | 0.05 ms | 1.000 / 1.000 |
| hnsw_fast | 0.01 ms | 0.01 ms | 3.14 ms | 1.000 / 1.000 |
| hnsw_recall | 0.01 ms | 0.01 ms | 3.02 ms | 1.000 / 1.000 |

## Follow-ups

- Run larger production embedding dumps through `--fixture real` before
  changing the default ANN policy.
- Measure HNSW memory overhead and persisted sidecar footprint.
- Add update/delete maintenance or tombstone compaction for non-append ANN
  mutations.
- Add an IVF/centroid baseline and mixed-ranking-aware candidate expansion.
