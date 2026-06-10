# 168: P3 Agent Memory Failover Status

Date: 2026-06-06
Status: Complete

## Context

Multi-node Agent Memory already had deterministic successor failover and explicit
failed-owner shard adoption, but the result only exposed `ok`, `adopted`, and
the replacement node id. If a successor had no replay source, the hook returned a
successful no-op without a durable signal that the owner transition was degraded.
Operators also had no local HTTP surface to inspect the last owner-failover
state after the callback returned.

## Changes

- Extended `AgentMemoryOwnerFailoverResult` with source/replacement node ids,
  source/new ring epochs, `replica_promoted`, `degraded`,
  `replay_source_missing`, and `degraded_reason`.
- Recorded the last local Agent Memory owner-failover result in `HttpServer`.
- Added a `failover` object to local `GET /api/ai/stats` so operators can see
  whether the last transition cleanly promoted a replay source or entered a
  degraded state because no replay source was available.
- Marked successful successor replay as `replica_promoted=true` and missing
  source shards as `ok=true`, `degraded=true`, and
  `replay_source_missing=true`.
- Added tests for clean successor replay status and degraded missing-source
  reporting.

## Verification

- `cmake --build build --target zepto_tests -j$(nproc)`
- `./build/tests/zepto_tests --gtest_filter='AgentMemoryHttpTest.OwnerFailover*:AgentMemoryHttpTest.AdoptsFailedOwnerShardAndServesOwnerScopedIds:AgentMemoryHttpTest.ClusterStats*'`
- `./build/tests/zepto_tests --gtest_filter='AgentMemory*'` (52/52 passed)

## Follow-ups

- Capacity-eviction side-effect rollback was completed in devlog 169.
