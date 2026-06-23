# Physical AI Action-Outcome Experiment 019 Server Lifecycle

Date: 2026-06-23

Status: Experimental validation complete

Classification: Experimental runtime path

## Goal

Move the Physical AI edge/fleet connector from standalone replay-tool ownership
toward server-managed lifecycle ownership.

## Hypothesis

ZeptoDB can safely expose an experimental admin lifecycle for the edge/fleet
connector before adding the background SQL polling worker:

- admins can configure, enable, inspect, disable, and clear connector state,
- invalid bounded-capacity settings are rejected,
- auth-enabled deployments enforce admin-only access,
- `/metrics` exposes lifecycle and connector counters,
- missing checkpoint files start from empty ACK state.

## Procedure

1. Add a feed-layer lifecycle manager around `EdgeFleetFeedConnector`.
2. Add admin endpoints to `HttpServer`.
3. Add lifecycle metrics to `/metrics`.
4. Add focused unit tests for manager behavior.
5. Add HTTP endpoint tests for happy path, invalid input, 401, 403, and admin
   success.
6. Keep the feature marked experimental because no server background worker is
   attached yet.

## Acceptance Criteria

- `zepto_tests`, `zepto_http_server`, and `zepto_edge_fleet_replay` build.
- Runtime manager tests pass.
- HTTP lifecycle endpoint tests pass.
- Endpoint docs mark the feature experimental.
- Backlog narrows from lifecycle controls to background worker and persistence.

## Result

Verification:

```bash
cd build && ninja -j$(nproc) zepto_tests zepto_http_server zepto_edge_fleet_replay

./build/tests/zepto_tests \
  --gtest_filter='EdgeFleetFeedConnectorTest.*:EdgeFleetConnectorRuntimeTest.*'

./build/tests/zepto_tests \
  --gtest_filter='MetricsProviderTest.EdgeFleetConnector*:EdgeFleetConnectorAdminAuthTest.*'
```

Result: PASS.

| Check | Status |
| --- | --- |
| Runtime manager start/stop/metrics | pass |
| Invalid limit rejection | pass |
| Missing checkpoint starts empty | pass |
| Clear removes config | pass |
| HTTP configure/enable/metrics/delete | pass |
| HTTP invalid limit | pass |
| HTTP missing auth | 401 |
| HTTP writer credential | 403 |
| HTTP admin credential | 200 |

## Interpretation

Experiment 019 establishes the server control plane for the connector. It does
not yet make edge-to-fleet transfer automatic; instead it creates the bounded,
observable, admin-gated lifecycle surface needed before adding a background
worker.

## Next Product Or Research Step

Devlog 205 supersedes the generic worker portion of this step by adding a
bounded runtime worker hook/loop. The remaining product step is the built-in
SQL/HTTP adapter that reads the configured edge outbox table, applies fleet
sink rows, persists ACK state, and keeps `/metrics` updated without a
standalone tool or embedding-only hooks.
