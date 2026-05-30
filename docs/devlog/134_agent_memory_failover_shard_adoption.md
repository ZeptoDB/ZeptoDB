# 134: Agent Memory Failover Shard Adoption

Date: 2026-05-28
Status: Complete

## Context

Agent Memory owner-local snapshots and WAL replay protected a node across local
restart, but there was no primitive for a replacement owner to load a failed
owner's sidecar shard after the routing ring removed that failed node.

## Changes

- Added `AgentMemoryStore::memory_records_snapshot()` and
  `AgentMemoryStore::cache_entries_snapshot()` for persistence/failover
  adoption without perturbing access counters.
- Added `HttpServer::adopt_agent_memory_owner_shard(source_node_id,
  source_ring_epoch)`.
  - Validates the source shard `manifest.json`.
  - Loads the source shard snapshot.
  - Replays the source shard `wal.log`.
  - Merges memory/cache entries into the replacement node's live store.
  - Publishes the replacement node's current shard snapshot.
- Point lookup for owner-scoped ids now falls back to current-ring routing when
  the id's embedded owner node is no longer present in the Agent Memory ring.
- Added an HTTP-level regression test that writes a `mem_1_7_*` record, adopts
  node 1's WAL-backed shard into node 2, and serves the old owner-scoped id from
  node 2 after the ring changes to `{2}`.

## Verification

- `ninja -j$(nproc) zepto_tests zepto_http_server` passed on x86_64.
- `./tests/zepto_tests --gtest_filter="AgentMemoryHttpTest.AdoptsFailedOwnerShardAndServesOwnerScopedIds"` passed: 1/1.

## Follow-ups

- Wire automatic failover orchestration so the cluster controller calls shard
  adoption after a node is declared dead and the ring epoch advances.
- Add quorum/sync replicated Agent Memory mutation records with per-shard
  sequence numbers.
- Add cluster-level Agent Memory stats and tenant quotas.
