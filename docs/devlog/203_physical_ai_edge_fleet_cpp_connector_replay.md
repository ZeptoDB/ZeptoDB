# 203: Physical AI Edge/Fleet C++ Connector Replay

Date: 2026-06-23

Status: Complete

Classification: Experimental runtime path

## Context

Devlog 202 introduced `EdgeFleetFeedConnector` as a transport-neutral C++
state machine. Experiment 018 wires that connector to two live ZeptoDB HTTP SQL
nodes so the Physical AI edge/fleet replay no longer depends on the Python feed
worker for bounded delivery, ACK checkpointing, duplicate/late handling,
outage retry, and restart convergence.

## Changes

- Added `tools/zepto_edge_fleet_replay.cpp`.
  - Applies the Experiment 016 edge SQL fixture to an edge HTTP node.
  - Applies only fleet base/DDL seed SQL to a fleet HTTP node.
  - Reads `physical_ai_edge_feed_outbox_016` through native SQL.
  - Converts edge outbox rows into `EdgeFleetFeedEvent` values.
  - Uses `EdgeFleetFeedConnector` to run bounded passes with outage, dropped,
    duplicate, late, restart, and final-drain phases.
  - Materializes fleet inbox, decision, retrieval, suppression, ACK, and
    telemetry rows through SQL inserts.
  - Validates final state through native SQL row counts and JOIN result rows.
  - Writes `docs/research/results/physical_ai_edge_fleet_cpp_connector_replay_018.md`.
- Added the `zepto_edge_fleet_replay` CMake target.

## Verification

```bash
cd build && ninja -j$(nproc) zepto_edge_fleet_replay zepto_tests

./build/tests/zepto_tests --gtest_filter='EdgeFleetFeedConnectorTest.*'

./build/zepto_http_server --port 19441 --node-id 1 --no-auth --storage-mode pure --log-level warn
./build/zepto_http_server --port 19442 --node-id 8 --no-auth --storage-mode pure --log-level warn

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

Result:

- `EdgeFleetFeedConnectorTest.*`: 8/8 passed.
- Experiment 018 live replay: PASS, 52/52 fleet ACK rows.

## Result Summary

| Check | Value |
| --- | ---: |
| Edge outbox rows | 52 |
| Fleet inbox rows | 52 |
| Fleet ACK rows | 52 |
| Fleet decision rows | 5 |
| Fleet retrieval rows | 15 |
| Fleet suppression rows | 32 |
| Fleet telemetry rows | 6 |
| Outage telemetry rows | 1 |
| Duplicate telemetry rows | 5 |
| Late telemetry rows | 1 |
| Restart telemetry rows | 1 |
| Recovery JOIN rows | 5 |
| Suppression audit JOIN rows | 32 |

## Experimental Boundary

This is an experimental runtime path, not a promoted replication product
feature.

Intended workload:

- Experiment 016 Physical AI Action-Outcome edge/fleet operational tables,
- one edge outbox table and one fleet sink table family,
- bounded batches of decision, retrieval, and suppression events,
- live two-node SQL replay over ZeptoDB HTTP endpoints.

Non-goals:

- automatic server lifecycle management,
- a general SQL replication connector,
- exactly-once distributed transactions,
- multi-edge scheduling or conflict resolution,
- RBAC/admin surface for enabling connectors,
- catalog-persisted feed configuration.

Hard limits:

- every connector pass is bounded by `batch_limit` and `max_inflight`,
- ACK state is persisted only to the configured local checkpoint file,
- the SQL/HTTP adapter is a standalone experiment tool, not a server-managed
  background service,
- sink idempotency is required for replay after ACK-boundary failure.

Telemetry:

- connector pass counters,
- fleet feed telemetry rows for outage, dropped/transient failure, duplicate,
  late, restart reload, and final convergence,
- native SQL validation counts and JOIN result cardinality.

Failure behavior:

- outage and dropped events return transient failure and remain unacknowledged,
- duplicate ACKed events are skipped by connector state,
- late events are accepted when `allow_late_events=true`,
- restart reloads checkpointed ACK state before continuing,
- final-table success followed by ACK failure remains replayable.

Rollback/disable plan:

- do not run `zepto_edge_fleet_replay`.
- No server, storage, SQL planner, or cluster behavior changes when the tool is
  not executed.

Product-promotion criteria:

- move source/sink adapter lifecycle into a documented server or operator
  control plane,
- persist connector configuration and cursor/ACK state in catalog or a
  documented runtime state directory,
- add RBAC/admin controls and security tests,
- add long-running integration coverage for restart, node replacement, and
  rolling upgrade,
- document idempotent sink requirements and exactly-once limitations.

## Follow-ups

- Productize connector lifecycle and metrics registration.
- Decide catalog/runtime persistence for feed cursor and ACK state.
- Add RBAC and operator docs before exposing this as a supported connector.
