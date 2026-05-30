# 132: Agent Memory Remote Owner WAL And Fencing

Date: 2026-05-28
Status: Complete

## Context

Routed Agent Memory writes already sent mutations to the owner node and local WAL
replay now protects mutations after the last snapshot. The next multi-node slice
was to make the routed owner path explicit: remote write clients should carry
the routing epoch for fencing, and a remote owner with persistence enabled should
ACK only after the owner-local WAL append succeeds.

## Changes

- `HttpServer::set_agent_memory_routing()` now applies
  `AgentMemoryRouterConfig::ring_epoch` to every configured Agent Memory
  `TcpRpcClient`, so `AGENT_MEMORY_PUT` and `AGENT_CACHE_STORE` carry the epoch
  in the existing RPC header when writes are routed remotely.
- Remote owner write callbacks still apply the mutation to the local
  `AgentMemoryStore`, append the owner-local `wal.log` when persistence is
  enabled, and only then serialize a successful `StoreResult`.
- Routed shard manifest validation now treats WAL-only directories as owned
  shard data, so a missing manifest rejects startup before replay.
- Added regression tests for remote owner WAL replay and stale-epoch rejection
  through `TcpRpcServer` fencing.

## Verification

- `ninja -j$(nproc) zepto_tests zepto_http_server` passed on x86_64.
- `./tests/zepto_tests --gtest_filter="AgentMemoryHttpTest.RoutedRemoteOwnerWritePersistsOwnerWalBeforeAck:AgentMemoryHttpTest.RoutedRemoteWritesCarryRingEpochForFencing"` passed: 2/2.
- `./tests/zepto_tests --gtest_filter="AgentMemory*"` passed: 35/35.

## Follow-ups

- Implement Agent Memory semantic-cache fan-out.
- Add quorum/sync replicated Agent Memory mutation records with per-shard
  sequence numbers.
- Add failover replay that assigns a failed owner's sidecar snapshot/WAL to the
  replacement owner.
