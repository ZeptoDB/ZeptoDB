# 130: Agent Memory Shard-Local Persistence

Date: 2026-05-28
Status: Complete

## Context

Agent Memory routed writes and fan-out reads were multi-node aware, but
persistence still used a single sidecar directory. If multiple pods shared one
`--agent-memory-dir`, snapshot publication could overwrite another pod's local
Agent Memory files.

## Changes

- Kept standalone persistence backward-compatible at:
  - `{agent_memory_dir}/records.bin`
  - `{agent_memory_dir}/vectors.bin`
- Added routed-mode shard-local snapshot paths:
  - `{agent_memory_dir}/node-{node_id}/shard-0/records.bin`
  - `{agent_memory_dir}/node-{node_id}/shard-0/vectors.bin`
  - `{agent_memory_dir}/node-{node_id}/shard-0/manifest.json`
- Added routed snapshot manifest validation for `node_id`, `shard_id`, and
  `ring_epoch` before startup load.
- Updated `HttpServer::set_agent_memory_routing()` to return `false` when
  shard-local persistence validation or load fails.
- Updated `zepto_http_server` startup to fail fast if Agent Memory routed
  persistence cannot be validated.
- Added tests for shard-local write-through snapshots, restart reload after
  routing is applied, and wrong-owner manifest rejection.

## Verification

- `ninja -j$(nproc) zepto_tests zepto_http_server`
- `./tests/zepto_tests --gtest_filter="AgentMemory*"`

Verification was run on the current x86_64 instance. Cross-architecture
verification was not run in this slice.

## Follow-ups

- Agent Memory WAL mutation records and replay.
- Durability modes: `routed`, `quorum`, and `sync`.
- Shard migration and failover recovery beyond the current node-local
  `shard-0` snapshot.
