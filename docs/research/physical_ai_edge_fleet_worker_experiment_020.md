# Experiment 020: Physical AI Edge/Fleet Worker Runtime

Date: 2026-06-23
Classification: Experimental runtime path

## Goal

Move the Physical AI edge/fleet connector beyond lifecycle-only server control
by adding a bounded server-managed worker foundation.

## Hypothesis

If `EdgeFleetConnectorRuntime` owns bounded worker passes and telemetry, then
the standalone replay tool is no longer required to control connector
lifecycle. A production adapter can later supply the concrete SQL/HTTP outbox
loader and fleet sink without changing the delivery state machine.

## Implementation

- Added `EdgeFleetConnectorRuntimeHooks`:
  - `load_outbox`
  - `sink`
  - optional `observe_pass`
- Added manual `runOnce()`.
- Added background worker mode:
  - `worker_enabled`
  - `worker_poll_interval_ms`
- Added runtime snapshot fields for worker hook readiness, worker running
  state, worker counters, and last pass telemetry.
- Added Prometheus worker metrics.
- Added HTTP status/config fields and `HttpServer` hook installation API.

## Acceptance Checks

| Check | Expected |
| --- | --- |
| Manual bounded pass | Runtime processes only `min(batch_limit, max_inflight)` events. |
| Background convergence | Worker drains an outbox across multiple bounded passes. |
| Loader outage | Worker records load errors and recovers on a later pass. |
| Missing hooks | Worker mode rejects start with a clear error. |
| HTTP lifecycle | Admin lifecycle can start the worker when hooks are installed. |
| Metrics | Worker pass/error counters appear in `/metrics`. |

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

## Interpretation

Experiment 020 closes the server-managed worker lifecycle gap from
Experiment 019. The runtime can now own bounded processing and pass telemetry
instead of only owning configuration and start/stop state.

This is still not a promoted production feature. The worker currently requires
embedding code to install outbox-loader and fleet-sink hooks. Product promotion
still requires a built-in SQL/HTTP adapter, persisted config, idempotent sink
docs, live fault/soak tests, and cross-architecture verification.
