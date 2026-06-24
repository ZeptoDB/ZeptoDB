# 202: Physical AI Edge/Fleet Runtime Connector

Date: 2026-06-23

Status: Complete

Classification: Experimental runtime path

## Context

Experiment 016 proved bounded edge-to-fleet feed semantics in a Python research
harness. This change promotes the core state machine into C++ runtime code so
Physical AI deployments can start wiring real edge outbox sources and fleet
sinks without reimplementing retry, ACK, duplicate, late, outage, and restart
handling in each adapter.

## Changes

- Added `include/zeptodb/feeds/edge_fleet_feed_connector.h`.
  - Public experimental `EdgeFleetFeedConnector` API.
  - Bounded `EdgeFleetFeedConfig`.
  - `EdgeFleetFeedEvent`, `EdgeFleetFeedStats`, and per-pass result structs.
  - Sink result contract for `Acked`, `TransientFailure`,
    `PermanentFailure`, and `AppliedButAckFailed`.
  - Optional checkpoint file load/save.
  - Prometheus/OpenMetrics formatter.
- Added `src/feeds/edge_fleet_feed_connector.cpp`.
  - Sorts edge outbox candidates by `stream_seq`.
  - Enforces `batch_limit` and `max_inflight`.
  - Retries transient failures up to `max_retries_per_event`.
  - Leaves `AppliedButAckFailed` events unacknowledged for idempotent replay.
  - Skips already ACKed event ids and counts duplicates.
  - Detects late events when `stream_seq <= highest_acked_stream_seq`.
  - Persists and reloads ACK checkpoint state when configured.
- Added `tests/unit/test_edge_fleet_feed_connector.cpp`.
  - Covers defaults/config validation.
  - Covers bounded batch processing and checkpoint persistence.
  - Covers dropped/outage-style transient retry and late delivery.
  - Covers restart checkpoint reload.
  - Covers final-table success followed by ACK failure.
  - Covers malformed event rejection and optional late-event blocking.
  - Covers within-pass transient retry.
  - Covers Prometheus metric formatting.
- Wired the connector into `zepto_feeds` and `zepto_tests`.
- Updated C++ API, design, governance, backlog/completed, and research log docs.

## Verification

```bash
cd build && ninja -j$(nproc) zepto_tests

./build/tests/zepto_tests --gtest_filter='EdgeFleetFeedConnectorTest.*'
```

Result: 8/8 `EdgeFleetFeedConnectorTest` tests passed.

## Experimental Boundary

This is an experimental runtime path, not a promoted product feature.

Intended workload:

- bounded Physical AI edge-to-fleet Action-Outcome evidence transfer,
- one edge outbox stream per connector instance,
- event kinds: `decision`, `retrieval`, `suppression`,
- source/sink adapters supplied by the embedding application.

Non-goals:

- built-in HTTP or SQL polling adapter,
- exactly-once distributed transactions,
- multi-edge fan-in scheduling,
- fleet-side conflict resolution beyond idempotent ACK replay,
- automatic ZeptoDB server startup/config integration.

Hard limits:

- every pass is bounded by `batch_limit` and `max_inflight`,
- checkpoint persistence stores ACK ids in a local file and is not catalog or
  cluster metadata,
- connector calls are single-worker and externally serialized.

Telemetry:

- cumulative stats for passes, attempted, ACKed, transient/permanent failures,
  ACK-boundary failures, duplicates, late events, rejected events, checkpoint
  load/save/failure counts, and max in-flight observed,
- `formatPrometheus()` exposes these counters/gauges for `/metrics` providers.

Failure behavior:

- transient failures remain unacknowledged for retry,
- permanent failures remain unacknowledged and are counted,
- `AppliedButAckFailed` remains unacknowledged so a later pass can replay the
  already-applied event idempotently,
- malformed events are rejected without sink delivery.

Persistence and restart:

- optional checkpoint path persists ACKed event ids and highest ACKed stream
  sequence,
- restart reload is explicit via `loadCheckpoint()`.

Rollback/disable plan:

- do not instantiate the connector or remove the metrics provider callback.
- No SQL, HTTP, storage, or cluster behavior changes when unused.

Product-promotion criteria:

- add a ZeptoDB SQL/HTTP outbox source adapter and fleet sink adapter,
- persist cursor/ACK state in catalog or documented runtime config,
- add admin/security controls for connector lifecycle,
- add live two-node runtime integration tests with outage/restart,
- document exactly-once limitations and idempotent sink requirements.

## Follow-ups

- Build a SQL/HTTP source/sink adapter for the connector.
- Add live two-node integration tests using real Experiment 016 tables.
- Add operator lifecycle and metrics registration hooks.
