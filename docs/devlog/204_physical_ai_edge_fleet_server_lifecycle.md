# 204: Physical AI Edge/Fleet Server Lifecycle

Date: 2026-06-23

Status: Complete

Classification: Experimental runtime path

## Context

Experiment 018 proved that the C++ connector can drive a live two-node SQL/HTTP
replay from a standalone tool. The next productization step is to let the
ZeptoDB server own connector lifecycle state, expose admin controls, and publish
operator-visible metrics without running the replay tool.

## Changes

- Added `EdgeFleetConnectorRuntime`.
  - Header: `include/zeptodb/feeds/edge_fleet_connector_runtime.h`
  - Implementation: `src/feeds/edge_fleet_connector_runtime.cpp`
  - Owns experimental connector configuration, enabled/disabled state, local
    checkpoint start/stop behavior, lifecycle counters, status snapshots, and
    Prometheus formatting.
- Added server-managed admin endpoints.
  - `GET /admin/edge-fleet-connector`
  - `POST /admin/edge-fleet-connector`
  - `DELETE /admin/edge-fleet-connector`
- Added `/metrics` exposure for connector lifecycle status and underlying
  connector counters.
- Added C++ tests for runtime config validation, start/stop/clear behavior,
  missing-checkpoint startup, metrics output, admin endpoint happy/invalid
  paths, and admin-only authorization.

## Verification

```bash
cd build && ninja -j$(nproc) zepto_tests zepto_http_server zepto_edge_fleet_replay

./build/tests/zepto_tests \
  --gtest_filter='EdgeFleetFeedConnectorTest.*:EdgeFleetConnectorRuntimeTest.*'

./build/tests/zepto_tests \
  --gtest_filter='MetricsProviderTest.EdgeFleetConnector*:EdgeFleetConnectorAdminAuthTest.*'
```

Result:

- Connector/runtime focused tests: 12/12 passed.
- HTTP lifecycle/admin tests: 3/3 passed.

## Experimental Boundary

This is still an experimental runtime path, not a promoted product connector.

Intended workload:

- server-owned lifecycle state for one Physical AI edge/fleet connector,
- admin configuration of connector name, edge outbox table, fleet ACK table,
  checkpoint path, batch limits, retry limit, and late-event policy,
- metrics visibility for lifecycle and connector counters.

Non-goals:

- no background SQL polling worker yet,
- no automatic fleet sink execution yet,
- no multi-edge scheduler,
- no catalog-persisted connector DDL,
- no exactly-once distributed transaction contract.

Failure behavior:

- invalid limits return HTTP 400,
- unauthenticated admin access returns 401 when auth is enabled,
- non-admin credentials return 403,
- missing checkpoint files start with empty ACK state,
- existing checkpoint parse/load failures block start and increment lifecycle
  failure counters.

Persistence:

- checkpoint state remains local-file based and experimental,
- connector configuration is process-local until a catalog/runtime state layer
  is added.

Rollback/disable plan:

- call `DELETE /admin/edge-fleet-connector`, or do not configure the connector.
- No SQL planner, storage, or cluster behavior changes when the connector is
  not enabled.

Product-promotion criteria:

- add the server-managed SQL outbox polling worker and fleet sink execution,
- persist connector config in catalog or a documented runtime state file,
- add long-running restart/node-replacement tests,
- document idempotent sink requirements and exactly-once limitations.

## Follow-ups

- Wire a bounded background poll/apply worker behind the lifecycle manager.
- Persist connector configuration across restart.
- Add operations documentation for enabling and monitoring the connector.
