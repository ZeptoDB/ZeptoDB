# Physical AI Action-Outcome Experiment 018 C++ Connector Replay

Date: 2026-06-23

Status: Experimental validation complete

Classification: Experimental runtime path

## Goal

Connect the C++ `EdgeFleetFeedConnector` to the existing two-node Physical AI
edge/fleet SQL replay and prove that the connector can replace the Python feed
worker for bounded Action-Outcome delivery.

## Hypothesis

The runtime connector plus a narrow SQL/HTTP source-sink adapter can preserve
the Experiment 016 semantics:

- edge-local decisions remain materialized immediately,
- fleet-global audit converges through bounded batches,
- dropped and outage failures remain retryable,
- duplicate and late events are visible in telemetry,
- restart reloads ACK state from a checkpoint,
- fleet SQL/JOIN validation still passes after replay.

## Procedure

1. Start two ZeptoDB HTTP servers with `--no-auth --storage-mode pure`.
2. Apply the Experiment 016 edge SQL fixture to the edge node.
3. Apply only fleet base/DDL seed statements to the fleet node.
4. Read `physical_ai_edge_feed_outbox_016` from the edge node through native SQL.
5. Convert outbox rows into `EdgeFleetFeedEvent` values.
6. Run `EdgeFleetFeedConnector` passes for outage, bounded recovery with a
   dropped event and duplicate event, restart reload with late delivery, and
   final drain.
7. Materialize fleet inbox, final operational rows, ACK rows, and telemetry
   rows through SQL inserts.
8. Validate counts and JOIN result cardinality through native SQL.
9. Write the immutable result report.

## Acceptance Criteria

- `zepto_edge_fleet_replay` builds.
- `EdgeFleetFeedConnectorTest.*` remains green.
- Live two-node replay reaches 52/52 fleet ACK rows.
- Fleet materializes 5 decision rows, 15 retrieval rows, and 32 suppression
  rows.
- Telemetry records outage, duplicate, late, and restart reload evidence.
- Native recovery JOIN returns 5 rows.
- Native suppression audit JOIN returns 32 rows.
- Docs keep the path classified as experimental.

## Result

Result report:

- `docs/research/results/physical_ai_edge_fleet_cpp_connector_replay_018.md`

Verification:

```bash
cd build && ninja -j$(nproc) zepto_edge_fleet_replay zepto_tests

./build/tests/zepto_tests --gtest_filter='EdgeFleetFeedConnectorTest.*'

./build/zepto_edge_fleet_replay \
  --edge-url http://127.0.0.1:19441 \
  --fleet-url http://127.0.0.1:19442 \
  --outage-url http://127.0.0.1:1 \
  --edge-sql docs/research/results/physical_ai_edge_fleet_feed_replay_016_edge.sql \
  --fleet-seed-sql docs/research/results/physical_ai_edge_fleet_feed_replay_016_fleet.sql \
  --output docs/research/results/physical_ai_edge_fleet_cpp_connector_replay_018.md \
  --checkpoint /tmp/zeptodb_edge_fleet_cpp_connector_018.checkpoint \
  --batch-limit 12 \
  --max-inflight 12
```

Result: PASS.

| Check | Value |
| --- | ---: |
| Edge outbox rows | 52 |
| Fleet ACK rows | 52 |
| Fleet decision rows | 5 |
| Fleet retrieval rows | 15 |
| Fleet suppression rows | 32 |
| Outage telemetry rows | 1 |
| Duplicate telemetry rows | 5 |
| Late telemetry rows | 1 |
| Restart telemetry rows | 1 |
| Recovery JOIN rows | 5 |
| Suppression audit JOIN rows | 32 |

## Interpretation

Experiment 018 closes the gap between research harness semantics and runtime
C++ connector behavior. The feed state machine is now validated against live
ZeptoDB SQL endpoints, not only mocked sink callbacks.

This is still not a promoted product connector. The SQL/HTTP adapter is a
standalone experiment tool, and product readiness still requires server
lifecycle controls, RBAC/admin policy, catalog or documented runtime
persistence, and long-running operational tests.

## Next Product Or Research Step

Productize the connector lifecycle: decide how operators enable the feed,
where cursor/ACK state is persisted, which RBAC roles can configure it, and how
metrics appear in `/metrics` without running a standalone experiment tool.
