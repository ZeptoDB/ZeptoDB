# 131: Agent Memory WAL Replay

Date: 2026-05-28
Status: Complete

## Context

Shard-local snapshots prevent multi-node snapshot overwrite, but mutations after
the last snapshot could still be lost on process crash. Agent Memory needed a
local replay log before moving on to replicated durability modes.

## Changes

- Added a local `wal.log` beside Agent Memory snapshot files.
- Appended local owner mutations for:
  - `PUT_MEMORY`
  - `STORE_CACHE`
- Replayed `wal.log` after loading `records.bin` and `vectors.bin`.
- Truncated `wal.log` after a successful snapshot publish.
- Kept standalone snapshot paths backward-compatible and reused routed
  `node-{node_id}/shard-0/` paths from devlog 130.
- Added read-only `AgentMemoryStore::get_cache()` so WAL append can persist exact
  cache entries without mutating access counters.
- Added tests for standalone deferred WAL replay, routed WAL replay without a
  snapshot, and WAL truncation after snapshot flush.

## Verification

- `ninja -j$(nproc) zepto_tests zepto_http_server`
- `./tests/zepto_tests --gtest_filter="AgentMemory*"`

Verification was run on the current x86_64 instance. Cross-architecture
verification was not run in this slice.

## Follow-ups

- Replicate Agent Memory WAL records to replicas for `quorum` and `sync`
  durability modes.
- Add delete and expire-batch WAL record types when those mutation APIs are
  introduced.
- Add failover replay from replica WAL and degraded-state reporting.
