# 128: Agent Memory Fan-Out Search And Context

Date: 2026-05-28
Status: Complete

## Context

Routed Agent Memory writes, point reads, and exact cache reads were already
available, but memory search and context assembly still only inspected the HTTP
pod's local store. A load-balanced agent could therefore miss memory records that
were correctly written to another owner pod.

## Changes

- Added Agent Memory wire payloads for `MemoryQuery` and search result batches.
- Added `AGENT_MEMORY_SEARCH` and `AGENT_MEMORY_SEARCH_RESULT` TcpRpc message
  types.
- Extended `TcpRpcServer`, `HttpServer`, and `zepto_http_server` callback wiring
  for remote Agent Memory search.
- Updated `/api/ai/memories/search` to fan out to Agent Memory nodes in routed
  mode, merge node-local top-K results by `score` and `created_at_ns`, then trim
  to the requested `limit`.
- Updated `/api/ai/context` to run the same global fan-out search merge before
  applying content deduplication and the token budget.
- Added strict partial-failure behavior for this first fan-out slice: remote
  search failures return HTTP `502`.
- Extended tests to cover two-node routed search/context after remote writes.

## Verification

- `ninja -j$(nproc) zepto_tests zepto_http_server`
- `./tests/zepto_tests --gtest_filter="AgentMemory*"`
- `./tests/zepto_tests --gtest_filter="TcpRpc.*:TcpRpcServer*:TcpRpcClient*:AgentMemory*"`

Verification was run on the current x86_64 instance. Cross-architecture
verification was not run in this slice.

## Follow-ups

- Semantic cache fan-out for embedding-similarity cache fallback.
- Best-effort fan-out mode with warning metadata, if operators want partial
  results instead of strict `502` failures.
- Shard-local snapshots, Agent Memory WAL replication, failover replay, tenant
  quotas, and cluster aggregate stats.
