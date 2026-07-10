# 212: Physical AI edge/fleet SQL/HTTP adapter

Date: 2026-07-09
Status: Complete

## Context

The experimental edge/fleet connector had a server-owned lifecycle and bounded
worker loop, but production-shaped pilots still needed embedding code or the
standalone `zepto_edge_fleet_replay` tool to connect ZeptoDB SQL tables to the
runtime hooks. This left the server path one step short of the Experiment 016
SQL/HTTP contract.

## Changes

- Added `EdgeFleetSqlHttpAdapterConfig` and
  `makeEdgeFleetSqlHttpRuntimeHooks()` under `include/zeptodb/server/`.
- Added local default Experiment 016 table bootstrap for edge outbox, fleet
  inbox, fleet decision, fleet retrieval, fleet suppression, ACK, and telemetry
  tables.
- Added a SQL/HTTP adapter implementation that reads the configured edge
  outbox table, writes fleet inbox/final/ACK rows, observes pass telemetry, and
  validates table identifiers and HTTP URLs before emitting SQL.
- Extended `HttpServer` with
  `set_edge_fleet_connector_sql_http_adapter()` and
  `POST /admin/edge-fleet-connector` fields for `sql_adapter_enabled`,
  `sql_adapter_create_tables`, optional edge/fleet SQL URLs, fleet table names,
  `outbox_query_limit`, and `record_pass_telemetry`.
- Added focused unit coverage for table bootstrap, decision/retrieval/
  suppression materialization, duplicate skip on a second pass, invalid
  identifiers, remote-bootstrap rejection, and HTTP admin adapter installation.

## Verification

- `ninja -C build -j$(nproc) zepto_tests`
- `./build/tests/zepto_tests --gtest_filter='EdgeFleet*'` — 21/21 passed.
- `./build/tests/zepto_tests --gtest_filter='MetricsProviderTest.EdgeFleetConnector*'`
  — 5/5 passed.
- `ctest -j$(nproc) -E "Benchmark\.|K8s" --output-on-failure --timeout 180`
  from `build/` — 0 failed out of 1717 counted tests; 3 perf tests disabled
  and live S3 opt-in skipped.
- `./tools/run-full-matrix.sh --stages=8 --force-resync` — aarch64
  Graviton SSH stage passed in 188s with 0 failed out of 1717 counted tests;
  3 perf tests disabled and live S3 opt-in skipped.

## Follow-ups

- Devlog 213 adds server-local config persistence, documented checkpoint-backed
  cursor reload, idempotent sink docs, restart/audit tests, and explicit
  SQL/backpressure limits.
- Remaining promotion evidence is long-running server-runtime soak/fault
  validation plus node-replacement testing over live edge/fleet tables.
