# 125: Agent Memory ANN Rebuild Path

Date: 2026-05-28
Status: Complete

## Context

The optional HNSW backend made semantic candidate search fast, but full graph
construction is expensive. The first implementation rebuilt the ANN index while
holding the `AgentMemoryStore` mutex and marked the whole index dirty after every
memory write, so a single append could force the next search to rebuild the full
graph.

## Changes

- Changed `AgentMemoryStore::rebuild_ann_index()` and lazy search rebuilds to
  snapshot memory vectors under the mutex, build the next ANN index outside the
  mutex, and swap it in only if no newer mutation superseded the snapshot.
- Added an ANN generation guard so stale off-lock rebuilds cannot replace a
  newer index after concurrent writes.
- Added append-only incremental ANN maintenance for clean indexes. New memory
  records are added directly to the active sparse-projection or HNSW backend
  when no eviction or row-id shift happened.
- Kept updates, TTL/capacity eviction, snapshot loads, and ANN config changes on
  the dirty/full-rebuild path because they can change row ids or vector contents.
- Added unit coverage that verifies append-only inserts increase indexed vectors
  without increasing the rebuild counter.

## Verification

- `ninja -j$(nproc) zepto_tests zepto_http_server bench_agent_memory` with
  `ZEPTO_ENABLE_HNSWLIB=ON`.
- `./tests/zepto_tests --gtest_filter="AgentMemoryStoreTest.AnnAppendMaintainsCleanIndexIncrementally:AgentMemoryStoreTest.AnnSearchPreservesTenantPartitionAndRanking:AgentMemoryStoreTest.HnswAnnSearchPreservesTenantPartition"`.
- `./tests/zepto_tests --gtest_filter="AgentMemory*"`.
- `./bench_agent_memory --records 10000 --dim 64 --iters 2 --compare-ann --recall-queries 2 --semantic-fixture`.
- `git diff --check`.

## Follow-ups

- Add true HNSW update/delete support or tombstone compaction for non-append
  mutations.
- Background rebuild scheduling shipped in devlog 127; remaining work is focused
  on update/delete graph maintenance and persistence.
- Persist ANN sidecars if first-build latency becomes a startup problem.
