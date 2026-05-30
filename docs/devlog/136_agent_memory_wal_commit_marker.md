# 136: Agent Memory WAL Commit Marker

Date: 2026-05-28
Status: Complete

## Context

Quorum/sync Agent Memory replication previously appended the owner-local WAL
record before replica acknowledgement. If replica acknowledgement failed, the
client received an error but a restart could still replay the local WAL record.

## Changes

- Added prepared Agent Memory WAL records plus `COMMIT` markers for memory and
  cache upserts.
- Updated WAL replay to apply legacy committed records for compatibility, apply
  prepared records only after their commit marker, and ignore trailing prepared
  records without a commit.
- Updated replica WAL append RPC handling to accept prepared records and commit
  markers.
- Added live-store rollback for failed persisted memory/cache writes.
- Added `AgentMemoryStore::remove_memory()` and `remove_cache()` for rollback
  and administrative cleanup paths.

## Verification

- `ninja -j$(nproc) zepto_tests zepto_http_server`
- `./tests/zepto_tests --gtest_filter="AgentMemoryHttpTest.SyncReplicationRejectsMissingReplicaAck:AgentMemoryHttpTest.DefersPersistenceUntilStopWhenFlushEveryZero:AgentMemoryHttpTest.RoutedPersistenceReplaysWalWithoutSnapshot:AgentMemoryHttpTest.SyncReplicationCopiesOwnerWalToReplicaShard:AgentMemoryHttpTest.QuorumReplicationSucceedsWithMajorityAck"`
- `./tests/zepto_tests --gtest_filter="AgentMemoryStoreTest.*"`

Cross-architecture verification was intentionally skipped for this step.

## Follow-ups

- Add explicit delete and automatic expire WAL records.
- Add per-shard monotonic sequence numbers and automatic replica catch-up.
- Promote failed-write rollback to a full transaction snapshot if deployments
  need to undo capacity-eviction side effects from a rejected write.
