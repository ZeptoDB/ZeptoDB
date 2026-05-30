# 138: Agent Memory Owner Failover

Date: 2026-05-28
Status: Complete

## Context

Agent Memory already had routed ownership, shard-local persistence, WAL replay,
replica WAL acknowledgement modes, and an explicit failed-owner shard adoption
primitive. The remaining gap for the next failover step was a callback-friendly
orchestration hook that can be invoked when the cluster failover manager removes
a dead node.

## Changes

- Added `AgentMemoryOwnerFailoverResult` and
  `HttpServer::handle_agent_memory_owner_failover()`.
- The hook advances the local Agent Memory ring to the supplied live nodes and
  new ring epoch, removes the failed owner, and rewrites the surviving node's
  shard snapshot manifest under the new epoch.
- Replacement selection is deterministic: first live node id greater than the
  failed source id, wrapping to the lowest live id.
- Only the selected replacement calls
  `adopt_agent_memory_owner_shard(source_node_id, source_ring_epoch)`.
  Non-successor nodes return success without adoption.
- Same-node shard manifests can now be loaded when their persisted ring epoch is
  older than the new local routing epoch. Source shard adoption still requires
  the exact source epoch.
- `zepto_http_server --failover-enabled` now starts `HealthMonitor` and
  `FailoverManager` in non-HA cluster mode, registers node endpoints, tracks the
  current Agent Memory ring epoch, and invokes the owner-failover hook after a
  node is declared dead.
- Added `--agent-memory-ring-epoch`, `--health-heartbeat-port`,
  `--health-tcp-port`, `--health-suspect-ms`, and `--health-dead-ms` CLI knobs.

## Verification

- `ninja -j$(nproc) zepto_tests`
- `ninja -j$(nproc) zepto_tests zepto_http_server`
- `./tests/zepto_tests --gtest_filter="AgentMemoryHttpTest.OwnerFailover*:AgentMemoryHttpTest.AdoptsFailedOwnerShardAndServesOwnerScopedIds"`
- `./tests/zepto_tests --gtest_filter="Failover.*:AgentMemoryHttpTest.OwnerFailover*:AgentMemoryHttpTest.AdoptsFailedOwnerShardAndServesOwnerScopedIds"` — 7 tests passed.
- `./tests/zepto_tests --gtest_filter="AgentMemory*"` — 43 tests passed.
- `./tests/zepto_tests --gtest_filter="TcpRpc.*:TcpRpcServer*:TcpRpcClient*:AgentMemory*"` — 60 tests passed.
- `./zepto_http_server --help`
- Local two-process e2e on one host:
  - node 1: `zepto_http_server --node-id 1 --add-node 2:127.0.0.1:<port> --agent-memory-dir <tmp> --agent-memory-flush-every 1 --agent-memory-ring-epoch 1`
  - node 2: same shared `--agent-memory-dir`, `--failover-enabled`,
    `--health-tcp-port 0`, and short health timeouts.
  - Stored `mem_1_1_1` on node 1 with tenant `t1`, namespace `agent`, content
    `e2e failover memory exact`; stopped node 1; node 2 logged
    `adopted=true, ring_epoch=2`; `GET /api/ai/memories/mem_1_1_1` on node 2
    returned the original tenant, namespace, and content.
  - Verified node 2 shard manifest has `node_id: 2` and `ring_epoch: 2`.
- `git diff --check`

## Follow-ups

- Add replica-promotion and degraded-state reporting for cases where the failed
  owner's persisted shard is unavailable.
