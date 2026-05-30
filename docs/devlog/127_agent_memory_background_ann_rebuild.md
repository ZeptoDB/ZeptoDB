# 127: Agent Memory Background ANN Rebuild

Date: 2026-05-28
Status: Complete

## Context

Devlog 125 moved ANN rebuild work outside the `AgentMemoryStore` mutex and added
append-only incremental maintenance, but lazy search still performed dirty
rebuilds synchronously. With HNSW this could put the first query after a dirty
mutation on the full graph-build path.

## Changes

- Added a background ANN rebuild worker owned by `AgentMemoryStore`.
- Changed lazy search to request a background rebuild instead of blocking on the
  rebuild. If no safe ANN index is available, the query uses the exact scan.
- Avoided dirtying ANN for updates that preserve tenant, namespace, and
  embedding, because candidate generation is unchanged.
- Changes that affect ANN candidates, TTL/capacity eviction, snapshot load,
  clear, and ANN config changes force exact fallback until the background worker
  swaps in a fresh index.
- Added generation-based request coalescing so repeated searches for the same
  dirty generation do not enqueue duplicate rebuilds, while mutations during an
  active rebuild request exactly one follow-up rebuild for the newest generation.
- Preserved `rebuild_ann_index()` as an explicit synchronous rebuild path for
  startup and benchmark callers.
- Added unit coverage for first-search exact fallback plus asynchronous ANN
  rebuild completion.

## Verification

- `ninja -j$(nproc) zepto_tests bench_agent_memory` with
  `ZEPTO_ENABLE_HNSWLIB=ON`.
- `./tests/zepto_tests --gtest_filter="AgentMemoryStoreTest.AnnSearchSchedulesBackgroundRebuild:AgentMemoryStoreTest.AnnAppendMaintainsCleanIndexIncrementally:AgentMemoryStoreTest.AnnMetadataUpdateDoesNotDirtyIndex:AgentMemoryStoreTest.AnnSearchPreservesTenantPartitionAndRanking:AgentMemoryStoreTest.HnswAnnSearchPreservesTenantPartition"`.
- `./tests/zepto_tests --gtest_filter="AgentMemory*"`.
- `./bench_agent_memory --records 10000 --dim 64 --iters 2 --compare-ann --recall-queries 2 --semantic-fixture`.

## Follow-ups

- Add HNSW update/delete or tombstone compaction so fewer mutations need full
  replacement indexes.
- Persist ANN sidecars if first-build latency becomes a startup problem.
