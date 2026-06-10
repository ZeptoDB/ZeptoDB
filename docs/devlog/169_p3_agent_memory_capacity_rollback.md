# 169: P3 Agent Memory Capacity Rollback

Date: 2026-06-06
Status: Complete

## Context

Owner-side Agent Memory writes already rolled back the primary memory/cache
mutation when persistence or quorum/sync replication failed. The remaining P3
multi-node gap was the side effect caused by bounded retention: a failed write
could evict an older memory/cache entry before the failure was reported. If the
eviction tombstone was not durable, the live store and replay state could
temporarily diverge until restart.

## Changes

- Extended `AgentMemoryEvictionEvent` with memory/cache snapshots in addition to
  tombstone keys.
- Added `AgentMemoryStore::restore_evicted_entries()` for durability rollback.
  It intentionally bypasses eviction enforcement so failed writes can restore
  the pre-write live state.
- Restored write-triggered eviction side effects when primary owner durability
  fails for HTTP and RPC memory/cache writes.
- Tracked the failed tombstone index when persisting automatic eviction
  tombstones. After partial tombstone success, the owner restores only the
  failed and later evicted entries and leaves already durable tombstones applied.
- Added a sync-replication missing-replica regression covering memory and cache
  capacity eviction rollback plus WAL replay.
- Updated Agent Memory design, C++ API, HTTP API, backlog, and completion docs.

## Verification

- `cmake --build build --target zepto_tests -j$(nproc)`
- `./build/tests/zepto_tests --gtest_filter='AgentMemoryHttpTest.FailedDurabilityRestoresCapacityEvictionSideEffects'`
- `./build/tests/zepto_tests --gtest_filter='AgentMemory*'` (53/53 passed)

## Follow-ups

- Shard migration dual-write/catch-up remains future multi-node work outside
  the now-closed P3 Multi-node Agent Memory backlog row.
- P3 open work is now stronger ANN fixture evaluation and optional managed
  embedding provider integration.
