# 137: Agent Memory Delete Tombstones

Date: 2026-05-28
Status: Complete

## Context

Agent Memory WAL replay covered memory/cache upserts and committed prepared
records, but explicit removal was still in-memory only. A deleted memory or cache
entry could therefore reappear after replaying an older upsert from `wal.log`.

## Changes

- Added Agent Memory memory/cache delete RPC payloads and `TcpRpc` write routing
  for `AGENT_MEMORY_DELETE` and `AGENT_CACHE_DELETE`.
- Added `DELETE /api/ai/memories/:memory_id` and `DELETE /api/ai/cache`.
- Persisted explicit memory/cache deletes as prepared WAL tombstones plus commit
  markers, including quorum/sync replica WAL append support.
- Extended WAL replay to apply committed memory/cache tombstones.
- Registered delete callbacks in `zepto_http_server`.

## Verification

- `ninja -j$(nproc) zepto_tests zepto_http_server`
- `./tests/zepto_tests --gtest_filter="AgentMemoryWireTest.RoundTripsMemoryCacheAndStoreResult:AgentMemoryHttpTest.WalTombstonesRemoveMemoryAndCacheOnReplay:AgentMemoryHttpTest.RoutedMemoryAndCacheWritesUseRemoteOwner"`
- `./tests/zepto_tests --gtest_filter="AgentMemoryHttpTest.DefersPersistenceUntilStopWhenFlushEveryZero:AgentMemoryHttpTest.RoutedPersistenceReplaysWalWithoutSnapshot"`

Cross-architecture verification was intentionally skipped for this step.

## Follow-ups

- Add automatic TTL/capacity-eviction tombstones.
- Add per-shard monotonic sequence numbers and automatic replica catch-up.
