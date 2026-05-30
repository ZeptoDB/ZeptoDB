# 133: Agent Memory Semantic Cache Fan-out

Date: 2026-05-28
Status: Complete

## Context

Routed Agent Memory already supported exact prompt cache lookup by routing the
normalized prompt hash to its owner. Semantic cache fallback was still local, so
a load-balanced request could miss semantically similar cache entries stored on
another Agent Memory node.

## Changes

- `HttpServer::lookup_agent_cache_routed_()` now keeps the exact prompt lookup
  as a single-owner request, then fans semantic fallback out to configured Agent
  Memory remotes when the exact lookup misses and the caller supplied an
  embedding.
- `HttpServer::handle_agent_cache_lookup_rpc()` now preserves the lookup
  embedding so the same RPC callback can serve semantic cache fallback as well as
  exact cache lookup.
- The coordinator returns the highest-score semantic hit across local and remote
  Agent Memory stores.
- Updated API/design docs and narrowed the multi-node Agent Memory backlog.

## Verification

- `ninja -j$(nproc) zepto_tests zepto_http_server` passed on x86_64.
- `./tests/zepto_tests --gtest_filter="AgentMemoryHttpTest.RoutedMemoryAndCacheWritesUseRemoteOwner"` passed: 1/1.

## Follow-ups

- Add explicit remote-cache lookup error reporting if operators prefer fail-closed
  semantic cache behavior.
- Implement quorum/sync replicated Agent Memory mutation records and failover
  replay.
