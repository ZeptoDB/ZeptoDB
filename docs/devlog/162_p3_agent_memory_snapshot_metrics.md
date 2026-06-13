# 162: P3 Agent Memory Snapshot Metrics

Date: 2026-06-04
Status: Complete

## Context

P3 Agent Memory persistence already had configurable flush cadence, WAL replay,
and memory/eviction metrics. Operators still could not see whether snapshot
publishes were slow or failing without reading logs, which made persistence
health harder to automate in production.

## Changes

- Added snapshot observability fields to `AgentMemoryStats`:
  `snapshot_latency_seconds` and `snapshot_failures_total`.
- Instrumented `AgentMemoryStore::save_to_directory()` so every snapshot
  attempt records elapsed time, and failed attempts increment a monotonic
  failure counter.
- Exposed the fields through `/api/ai/stats`.
- Exported Prometheus metrics:
  `zepto_agent_memory_snapshot_latency_seconds` and
  `zepto_agent_memory_snapshot_failures_total`.
- Added store-level coverage for failed and successful snapshot attempts, and
  HTTP coverage for the JSON and Prometheus surfaces.
- Updated Agent Memory design and API docs, BACKLOG, and COMPLETED.

## Verification

- `cmake --build build --target zepto_tests -j$(nproc)`
- `./build/tests/zepto_tests --gtest_filter='AgentMemoryStoreTest.SnapshotStatsTrackSuccessAndFailures:AgentMemoryHttpTest.ExposesStatsAndPrometheusMetrics'`
  — 2/2 passed.
- `./build/tests/zepto_tests --gtest_filter='AgentMemory*'` — 44/44 passed.

## Follow-ups

- Tenant-scoped Agent Memory eviction was completed in devlog 163.
