# 122: Agent Memory Multi-Node Foundation

Date: 2026-05-28
Status: Complete

## Context

Agent Memory v0 is intentionally node-local. Multi-node support needs a safe
foundation before adding remote RPC dispatch: deterministic ownership keys,
stable owner decisions, and owner-scoped automatic ids that avoid collisions
across nodes and ring epochs.

## Changes

- Added `zeptodb::ai::AgentMemoryRouter`, a dependency-free consistent hash ring
  over Agent Memory node ids. It supports `Local` and `Routed` modes, live-node
  add/remove, stable memory/cache routing keys, and snapshot owner decisions.
- Added `AgentMemoryIdConfig` to `AgentMemoryStore`. Legacy ids remain the
  default (`mem_N`, `cache_N`); routed deployments can opt into
  `mem_<node>_<epoch>_<counter>` and `cache_<node>_<epoch>_<counter>`.
- Added C++ unit coverage for local-mode routing, routed stable ownership,
  node-removal rerouting, logical-subject priority, cache prompt-hash routing,
  and owner-scoped automatic ids.
- Updated the Agent Memory design, C++ API reference, and backlog to mark the
  router/id foundation complete while keeping routed RPC, fan-out search, shard
  snapshots, and replication open.

## Verification

- `ninja -j$(nproc) zepto_tests`
- `./tests/zepto_tests --gtest_filter="AgentMemory*"` — 22/22 passed.

Known unrelated warning: `src/server/http_server.cpp:2552` still reports an
unused lambda capture during the build.

## Follow-ups

- Add routed remote write RPCs for `PUT_MEMORY` and `STORE_CACHE`.
- Route `get_memory(memory_id)` and exact cache lookup to their owner node.
- Add fan-out search/context merge with strict and best-effort partial-failure
  policies.
- Move sidecar persistence to shard-local paths once ownership is active.
