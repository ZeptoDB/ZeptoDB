# 167: P3 Agent Memory Eviction Tombstones

Date: 2026-06-05
Status: Complete

## Context

The multi-node Agent Memory backlog already had explicit memory/cache delete
tombstones, quorum/sync WAL replication, and cluster-scoped stats. One remaining
durability gap was automatic eviction: TTL, tenant-quota, and capacity eviction
could remove records from the live owner store without leaving a delete
tombstone in the owner WAL or replica WAL. A restart or later shard adoption
could therefore replay older upserts and resurrect entries that the live owner
had already evicted.

## Changes

- Added `AgentMemoryEvictionEvent` and `StoreResult.evictions` so
  `AgentMemoryStore::put_memory()` and `store_cache()` report tombstone keys for
  automatic TTL, tenant-quota, and capacity evictions caused by the write.
- Updated owner-side HTTP/RPC memory and cache write paths to persist those
  eviction keys through the existing prepared delete WAL plus commit flow.
- Preserved existing wire compatibility: remote callers still receive the same
  serialized `StoreResult`, while the remote owner persists eviction tombstones
  locally before acknowledging.
- Added regression coverage for store-level eviction event keys and WAL replay
  behavior with automatic memory/cache eviction tombstones.
- Updated Agent Memory design/API/backlog/completed docs to narrow the P3
  multi-node remaining gaps.

## Verification

- `cmake --build build --target zepto_tests -j$(nproc)`
- `./build/tests/zepto_tests --gtest_filter='AgentMemoryStoreTest.StoreResultReportsAutomaticEvictionTombstoneKeys:AgentMemoryHttpTest.WalTombstonesCaptureAutomaticEvictionsOnReplay'`
- `./build/tests/zepto_tests --gtest_filter='AgentMemoryStoreTest.*Evict*:AgentMemoryHttpTest.*Tombstone*:AgentMemoryHttpTest.ExposesStatsAndPrometheusMetrics:AgentMemoryHttpTest.WalTombstonesRemoveMemoryAndCacheOnReplay'`
- `./build/tests/zepto_tests --gtest_filter='AgentMemory*'` (51/51 passed)
- Pre-slice cross-arch EKS harness: `./deploy/scripts/run_arch_comparison_fast.sh --scenario basic` passed x86_64 and aarch64 build/scenario checks; NodePools were returned to CPU 0. The harness still emitted its existing ingestion baseline warning for the basic smoke workload.

## Follow-ups

- Replica promotion/degraded-state reporting when a failed owner has no usable
  replay source.
- Full transactional rollback for capacity-eviction side effects when a later
  tombstone persistence step fails.
