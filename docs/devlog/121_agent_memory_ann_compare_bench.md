# 121: Agent Memory ANN Compare Benchmark

Date: 2026-05-28
Status: Complete

## Context

Sparse-projection ANN was useful as a fast candidate generator, but single-query
recall numbers made it too easy to overfit a setting. HNSW/IVF evaluation needs a
repeatable baseline that can compare exact scan and ANN profiles from one seeded
store without letting access-count updates perturb later rankings.

## Changes

- Added `MemoryQuery::update_access`, defaulting to `true`, so normal retrieval
  still updates `access_count` and `last_accessed_ns` while benchmark and
  diagnostic probes can run read-only.
- Extended `bench_agent_memory` with `--compare-ann`, which seeds one store and
  runs `exact_scan`, `sparse_fast`, and `sparse_wide` profiles against that same
  fixture.
- Added `--recall-queries N` and report both average and minimum recall@K across
  deterministic query embeddings.
- Updated C++ API and Agent Memory design docs for the read-only search option
  and comparison workflow.

## Verification

- `ninja -j$(nproc) zepto_tests bench_agent_memory`
- `./tests/zepto_tests --gtest_filter="AgentMemory*"` — 18/18 passed.
- `./bench_agent_memory --records 10000 --dim 64 --iters 3 --compare-ann --recall-queries 3`
- `./bench_agent_memory --records 100000 --dim 128 --iters 5 --compare-ann --recall-queries 3`

100K compare results on this host:

| Profile | Search p50 | Context p50 | Build | Recall avg | Recall min |
|---|---:|---:|---:|---:|---:|
| exact_scan | 8.24 ms | 6.74 ms | 0.00 ms | 1.000 | 1.000 |
| sparse_fast | 0.75 ms | 1.08 ms | 130.53 ms | 0.042 | 0.000 |
| sparse_wide | 5.97 ms | 5.96 ms | 386.42 ms | 0.333 | 0.312 |

## Follow-ups

- Use the optional HNSW backend from devlog 123 as a baseline for clustered and
  real embedding fixtures.
- Add clustered and real embedding fixtures; the current deterministic random
  vectors are useful for repeatability but not representative enough alone.
