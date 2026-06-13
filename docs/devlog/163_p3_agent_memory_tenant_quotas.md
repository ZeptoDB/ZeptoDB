# 163: P3 Agent Memory Tenant Quotas

Date: 2026-06-05
Status: Complete

## Context

Agent Memory already had process-wide memory and cache capacity caps. Shared
agent deployments also need a way to keep one tenant or namespace from
consuming the whole local Agent Memory shard while preserving the existing
pinned-memory protection behavior.

## Changes

- Added `AgentMemoryTenantQuota` and
  `AgentMemoryEvictionConfig::tenant_quotas`.
- Tenant quotas can target a whole tenant when `namespace_id` is empty, or one
  tenant namespace when `namespace_id` is set.
- Quota enforcement runs before global caps and evicts only matching memory or
  cache entries by the existing lowest-retention policy.
- Pinned memories remain protected from capacity eviction and can still overflow
  a tenant quota; explicit TTL expiry still removes pinned records.
- `/api/ai/stats` now reports `tenant_quota_count` in `eviction_config`.
- `/metrics` now exports `zepto_agent_memory_tenant_quotas`.
- Updated C++/HTTP API docs, Agent Memory design docs, BACKLOG, and COMPLETED.

## Verification

- `cmake --build build --target zepto_tests -j$(nproc)`
- `./build/tests/zepto_tests --gtest_filter='AgentMemoryStoreTest.Tenant*Quota*:AgentMemoryHttpTest.ExposesStatsAndPrometheusMetrics'`
  — 4/4 passed.
- `./build/tests/zepto_tests --gtest_filter='AgentMemory*'` — 47/47 passed.

## Follow-ups

- OpenTelemetry/LLM trace ingest mapping was completed in devlog 164.
