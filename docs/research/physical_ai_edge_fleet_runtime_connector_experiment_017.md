# Physical AI Action-Outcome Experiment 017 Runtime Connector

Date: 2026-06-23

Status: Experimental validation complete

Classification: Experimental runtime path

## Goal

Promote the Experiment 016 bounded edge-to-fleet feed semantics from a Python
research harness into a reusable C++ runtime connector.

## Hypothesis

The core edge-to-fleet state machine can be made transport-neutral and tested
without live SQL endpoints:

- every pass is bounded,
- duplicate ACKed events are skipped,
- transient failures remain retryable,
- late events can still converge,
- restart can reload ACK checkpoint state,
- final-table success followed by ACK failure does not incorrectly mark the
  event as delivered.

## Procedure

1. Add `EdgeFleetFeedConnector` to the feed layer.
2. Keep transport injectable as a sink callback.
3. Add an optional checkpoint file for restart reload.
4. Add runtime stats and Prometheus formatting.
5. Add focused C++ unit tests for bounded processing, duplicates, late events,
   outage-style transient failures, restart checkpoint reload, malformed input,
   and ACK-boundary failure.

## Acceptance Criteria

- `zepto_tests` builds.
- `EdgeFleetFeedConnectorTest.*` passes.
- Tests cover happy path, empty/malformed input, bounded capacity, retry,
  duplicate, late, outage-style failure, restart reload, and ACK-boundary
  behavior.
- Docs label the connector experimental and record non-goals and promotion
  blockers.

## Result

The connector was added under `include/zeptodb/feeds/` and `src/feeds/`.

Verification:

```bash
cd build && ninja -j$(nproc) zepto_tests

./build/tests/zepto_tests --gtest_filter='EdgeFleetFeedConnectorTest.*'
```

Result: 8/8 focused tests passed.

## Interpretation

The experiment converts the feed semantics into a runtime code path while
keeping product boundaries honest. The connector now owns bounded delivery,
ACK state, duplicate/late handling, retry accounting, checkpoint reload, and
ACK-boundary semantics, but it still requires concrete ZeptoDB SQL/HTTP
source/sink adapters before it can replace the Python replay harness in live
two-node operation.

## Next Product Or Research Step

Build the SQL/HTTP source/sink adapter for Experiment 016 tables and run the
same two-node replay through the C++ connector instead of the Python feed
worker.
