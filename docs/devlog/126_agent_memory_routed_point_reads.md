# 126: Agent Memory Routed Point Reads

Date: 2026-05-28
Status: Complete

## Context

Devlog 124 added routed Agent Memory writes, but reads still stopped at the
receiving HTTP pod. That meant a memory or exact cache entry written to its owner
could still be missed by a later point lookup or exact prompt lookup sent to a
different pod.

## Changes

- Added Agent Memory wire payloads for memory point lookup and cache lookup
  results.
- Added `AGENT_MEMORY_GET`, `AGENT_MEMORY_GET_RESULT`,
  `AGENT_CACHE_LOOKUP_EXACT`, and `AGENT_CACHE_LOOKUP_RESULT` TcpRpc message
  types.
- Extended `TcpRpcServer` and `zepto_http_server` callback registration for
  remote Agent Memory point reads.
- Added `GET /api/ai/memories/:memory_id` with tenant and namespace query
  support.
- Routed exact prompt cache lookup to the prompt owner in multi-node mode.
  Semantic cache fallback remains local until the fan-out read phase.
- Updated HTTP, C++, design, and backlog documentation for the new routed read
  behavior and remaining multi-node gaps.

## Verification

- `ninja -j$(nproc) zepto_tests zepto_http_server`
- `./tests/zepto_tests --gtest_filter="AgentMemory*"`
- `./tests/zepto_tests --gtest_filter="TcpRpc.*:TcpRpcServer*:TcpRpcClient*:AgentMemory*"`

Verification was run on the current x86_64 instance. Cross-architecture
verification was not run in this slice.

## Follow-ups

- Fan-out memory search and context merge across Agent Memory nodes.
- Semantic cache fan-out, because embedding similarity is not key-addressable by
  the exact prompt owner.
- Shard-local snapshots, Agent Memory WAL replication, failover replay, tenant
  quotas, and cluster aggregate stats.
