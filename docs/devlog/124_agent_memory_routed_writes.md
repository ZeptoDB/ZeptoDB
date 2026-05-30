# 124: Agent Memory Routed Writes

Date: 2026-05-28
Status: Complete

## Context

Devlog 122 added deterministic Agent Memory ownership and owner-scoped ids, but
HTTP writes still landed only on the receiving pod. The next multi-node slice is
to route memory/cache writes to the owner node while preserving the existing
HTTP response shape.

## Changes

- Added Agent Memory wire serializers for `MemoryRecord`, `CacheEntry`, and
  `StoreResult`. The payload format lives in the AI layer; `TcpRpc` only moves
  opaque bytes.
- Added `AGENT_MEMORY_PUT`, `AGENT_MEMORY_RESULT`, `AGENT_CACHE_STORE`, and
  `AGENT_CACHE_RESULT` RPC message types.
- Added generic `TcpRpcClient::request_binary()` and server-side binary
  callbacks for Agent Memory write payloads.
- Added `HttpServer::set_agent_memory_routing()` plus RPC handlers that apply
  remote writes to the owner pod's local `AgentMemoryStore` and mark that pod's
  sidecar snapshot dirty.
- Wired `tools/zepto_http_server` cluster mode so `/api/ai/memories` and
  `/api/ai/cache/store` route to the owner pod when remote nodes are configured.
- Added unit coverage for wire round trips and an HTTP-to-remote-RPC routed
  memory/cache write path.
- Updated the design doc, HTTP API reference, C++ API reference, and backlog.

## Verification

- `ninja -j$(nproc) zepto_tests zepto_http_server`
- `./tests/zepto_tests --gtest_filter="AgentMemory*"` — 25/25 passed.
- `./tests/zepto_tests --gtest_filter="TcpRpc.*:TcpRpcServer*:TcpRpcClient*:AgentMemory*"` — 42/42 passed.

The first build attempt hit the sandboxed network limit while CMake tried to
update the existing spdlog FetchContent checkout. Re-running the same build with
approved network access succeeded.

## Follow-ups

- Route `get_memory(memory_id)` and exact prompt cache lookup to the owner node.
- Add fan-out search/context merge with strict and best-effort partial-failure
  policies.
- Add shard-local snapshot paths before enabling ownership migration.
- Add Agent Memory WAL mutation records before quorum/sync durability modes.
