# 166: P3 Agent Memory Cluster Stats

Date: 2026-06-05
Status: Complete

## Context

The P3 multi-node Agent Memory roadmap had routed writes, point reads, fan-out
search/context, semantic-cache fan-out, owner-local persistence, replica WAL
ACK policy, delete tombstones, owner failover adoption, tenant quotas, and local
stats. Operators still needed a cluster-scoped view of Agent Memory counts and
partial node failures instead of scraping only pod-local `/api/ai/stats`.

## Changes

- Added `AgentMemoryStats::tenant_quota_count` so stats can carry quota policy
  count without exposing full eviction configuration over internal RPC.
- Added Agent Memory stats binary wire helpers and `AGENT_MEMORY_STATS` /
  `AGENT_MEMORY_STATS_RESULT` TCP RPC types.
- Added `TcpRpcServer::set_agent_memory_stats_callback()` and HTTP server
  callback `handle_agent_memory_stats_rpc()`.
- Extended `GET /api/ai/stats` with `scope=local|cluster`. The default remains
  local/backward-compatible. Cluster scope returns `scope`, `partial_failures`,
  aggregate stats, and per-node stats or errors.
- Updated HTTP API, Agent Memory design docs, backlog, and completed-feature
  docs. The P3 multi-node row is narrowed rather than closed.

## Verification

- `cmake --build build --target zepto_tests -j$(nproc)`
- `./build/tests/zepto_tests --gtest_filter='AgentMemoryWireTest.RoundTripsMemoryCacheAndStoreResult:AgentMemoryHttpTest.ClusterStatsAggregateLocalAndRemoteNodes:AgentMemoryHttpTest.ClusterStatsReportsMissingRemoteClient:AgentMemoryHttpTest.ExposesStatsAndPrometheusMetrics'`
- `./build/tests/zepto_tests --gtest_filter='AgentMemory*'`

Local x86_64 Agent Memory coverage passed 49/49. Cross-architecture and EKS
cluster verification were not run for this slice.

## Follow-ups

- Continue the P3 multi-node row with automatic TTL/capacity-eviction tombstones.
- Add replica promotion/degraded-state reporting.
- Add full transactional rollback for capacity-eviction side effects.
