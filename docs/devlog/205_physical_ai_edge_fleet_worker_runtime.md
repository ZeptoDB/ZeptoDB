# 205: Physical AI Edge/Fleet Worker Runtime

Date: 2026-06-23
Status: Complete

## Context

Devlog 204 moved the experimental Physical AI edge/fleet connector lifecycle
into the HTTP server, but the runtime still had no server-owned worker path.
Operators could configure and inspect the connector, while actual outbox
polling and fleet sink execution still lived in the standalone
`zepto_edge_fleet_replay` experiment tool.

## Changes

- Extended `EdgeFleetConnectorRuntime` with a transport-neutral worker hook
  contract:
  - `EdgeFleetOutboxLoader`
  - `EdgeFleetPassObserver`
  - `EdgeFleetConnectorRuntimeHooks`
- Added `EdgeFleetConnectorRuntime::setWorkerHooks()` and `runOnce()`.
- Added bounded background worker mode controlled by
  `EdgeFleetConnectorRuntimeConfig::worker_enabled` and
  `worker_poll_interval_ms`.
- Added worker lifecycle and pass telemetry to runtime snapshots:
  hook readiness, worker running state, worker start/pass/load-error/observer
  counters, and the last bounded pass result.
- Added Prometheus metrics for worker running state, worker passes, outbox load
  errors, and observer errors.
- Exposed worker status through `GET /admin/edge-fleet-connector`.
- Extended `POST /admin/edge-fleet-connector` with `worker_enabled` and
  `worker_poll_interval_ms`.
- Added `HttpServer::set_edge_fleet_connector_runtime_hooks()` so embeddings
  can install SQL/HTTP/RPC adapter callbacks while the HTTP admin endpoint owns
  lifecycle and visibility.

## Verification

```bash
cd build && ninja -j$(nproc) zepto_tests zepto_http_server zepto_edge_fleet_replay

./build/tests/zepto_tests \
  --gtest_filter='EdgeFleetFeedConnectorTest.*:EdgeFleetConnectorRuntimeTest.*'

./build/tests/zepto_tests \
  --gtest_filter='MetricsProviderTest.EdgeFleetConnector*:EdgeFleetConnectorAdminAuthTest.*'
```

Results:

- Build: pass.
- Connector/runtime focused tests: pass, 16/16.
- HTTP lifecycle/admin tests: pass, 5/5.

## Follow-ups

- Build the concrete SQL/HTTP outbox-loader and fleet-sink adapter into the
  server runtime instead of requiring embeddings to provide hooks.
- Persist connector config/catalog metadata across restart.
- Document idempotent sink requirements and ACK-boundary behavior for
  operators.
- Add long-running restart/outage/duplicate/late/fault-injection soak tests
  over live ZeptoDB tables.
- Run cross-architecture verification before product promotion.
