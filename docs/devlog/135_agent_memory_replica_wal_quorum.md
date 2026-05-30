# 135: Agent Memory Replica WAL Quorum

Date: 2026-05-28
Status: Complete

## Context

Agent Memory owner-local WAL replay and explicit failed-owner shard adoption were
available, but owner mutations were not copied to replicas before ACK. Production
memory needs a configurable ACK policy so callers can choose single-owner
latency, quorum durability, or all-replica sync durability.

## Changes

- Added `AgentMemoryReplicationMode` with `Routed`, `Quorum`, and `Sync` modes.
- Added `--agent-memory-replication-mode local|routed|quorum|sync` to
  `zepto_http_server`.
- Added `AGENT_MEMORY_REPLICA_APPEND` / `AGENT_MEMORY_REPLICA_ACK` RPC types and
  `TcpRpcServer::set_agent_memory_replica_append_callback()`.
- Added `HttpServer::handle_agent_memory_replica_append_rpc()`.
  - Replicas append source-owner WAL records under
    `node-{source_node_id}/shard-0/wal.log`.
  - Replica append writes a source-owner manifest and validates any existing
    manifest before appending.
  - Replica append does not apply records to the replica's live store.
- Owner mutation persistence now copies `PUT_MEMORY` and `STORE_CACHE` WAL
  records to configured replicas when `Quorum` or `Sync` mode is enabled.
  - `Quorum` counts the owner-local WAL plus replica ACKs and requires a majority
    of configured Agent Memory nodes.
  - `Sync` requires every configured Agent Memory node.

## Verification

- `ninja -j$(nproc) zepto_tests zepto_http_server` passed on x86_64.
- `./tests/zepto_tests --gtest_filter="AgentMemoryHttpTest.SyncReplicationCopiesOwnerWalToReplicaShard:AgentMemoryHttpTest.QuorumReplicationSucceedsWithMajorityAck:AgentMemoryHttpTest.SyncReplicationRejectsMissingReplicaAck"` passed: 3/3.

## Follow-ups

- Add automatic failover orchestration that calls shard adoption after node death.
- Add delete/expire WAL records and per-shard sequence numbers.
- Add tenant quotas and cluster-level Agent Memory stats.
