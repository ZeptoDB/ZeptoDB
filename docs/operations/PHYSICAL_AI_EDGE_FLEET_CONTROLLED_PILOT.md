# Physical AI Edge/Fleet Controlled Pilot Runbook

Date: 2026-07-12
Status: Controlled pilot

This runbook defines the only supported rollout scope for the Physical AI
edge/fleet SQL/HTTP connector in the next release. The feature is production
validated for controlled pilots, but it is not a broad GA replication feature
and is not default-on operator functionality.

## Supported Scope

Controlled pilots may use the server-owned SQL/HTTP adapter behind
`/admin/edge-fleet-connector` when all of these constraints are met:

- One approved internal or customer pilot environment at a time.
- Admin-gated opt-in configuration; never enabled by default.
- ZeptoDB SQL/HTTP edge outbox and fleet sink endpoints only.
- Experiment 016-compatible event contract for decision, retrieval, and
  suppression evidence transfer.
- Stable `feed_event_id` per event and monotonically advancing `stream_seq`
  within the edge outbox.
- Durable `checkpoint_path` and, when the server owns configuration reload,
  `HttpServer::set_edge_fleet_connector_config_persistence(path)`.
- Bounded load settings: positive `outbox_query_limit`, positive
  `max_outbox_bytes`, positive `batch_limit`, positive `max_inflight`, and a
  configured `max_failures_per_pass`.

Out of scope for this pilot:

- Default-on deployment in Helm or packaged server profiles.
- Generic ZeptoDB replication, generic multi-table transactions, or external
  exactly-once sink claims.
- Arbitrary event schemas beyond the documented Physical AI edge/fleet
  Action-Outcome contract.
- Fleet actuator enforcement. Fleet-side rows are for audit, replay, and
  policy improvement.
- Public GA/SLA language beyond controlled pilot support.

## Required Configuration

Use the admin endpoint with explicit SQL adapter and bounded worker settings:

```bash
curl -X POST http://localhost:8123/admin/edge-fleet-connector \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "name": "physical_ai_edge_fleet",
    "enabled": true,
    "worker_enabled": true,
    "worker_poll_interval_ms": 1000,
    "checkpoint_path": "/var/lib/zeptodb/edge-fleet.checkpoint",
    "batch_limit": 128,
    "max_inflight": 128,
    "max_retries_per_event": 1,
    "max_failures_per_pass": 16,
    "retry_backoff_ms": 0,
    "allow_late_events": true,
    "sql_adapter_enabled": true,
    "sql_adapter_create_tables": false,
    "edge_sql_url": "http://edge-zeptodb:8123/",
    "fleet_sql_url": "http://fleet-zeptodb:8123/",
    "edge_outbox_table": "physical_ai_edge_feed_outbox_016",
    "fleet_ack_table": "physical_ai_fleet_feed_ack_016",
    "fleet_inbox_table": "physical_ai_fleet_feed_inbox_016",
    "fleet_decision_table": "physical_ai_fleet_edge_decisions_016",
    "fleet_retrieval_table": "physical_ai_fleet_retrieval_016",
    "fleet_suppression_table": "physical_ai_fleet_suppressions_016",
    "fleet_telemetry_table": "physical_ai_fleet_feed_telemetry_016",
    "outbox_query_limit": 128,
    "max_outbox_bytes": 1048576,
    "record_pass_telemetry": true
  }'
```

Use `sql_adapter_create_tables=true` only for local demos or empty test
environments. Remote edge/fleet SQL endpoints should be migrated explicitly by
the operator so schema ownership is clear.

## Idempotency Contract

- `feed_event_id` is the event idempotency key and must not change across
  retries, restarts, or node replacement.
- The fleet ACK table is the delivery ledger and the source of truth for
  completed events.
- The SQL/HTTP sink checks the ACK ledger before applying fleet final rows and
  inserts an ACK only after the final row succeeds.
- If final-row insertion succeeds but ACK insertion fails, the runtime reports
  `AppliedButAckFailed`; the next pass may replay the same event.
- Downstream consumers must dedupe fleet projections by `feed_event_id` or
  join against the ACK ledger when append-only tables are used.

## Monitoring

Poll lifecycle state:

```bash
curl http://localhost:8123/admin/edge-fleet-connector \
  -H "Authorization: Bearer $ADMIN_KEY"
```

Scrape Prometheus metrics:

```bash
curl http://localhost:8123/metrics
```

Pilot dashboards must include:

- `configured`, `enabled`, `worker_running`, and `worker_hooks_configured`
  from the admin status JSON.
- `worker_passes_total`, `worker_load_errors_total`,
  `worker_observer_errors_total`, `start_failures_total`, and
  `stop_failures_total`.
- Last-pass `outbox_events_seen`, `attempted_count`, `acked_count`,
  `transient_failure_count`, `permanent_failure_count`, `duplicate_count`,
  `late_count`, `rejected_count`, and `failure_budget_exhausted`.
- Fleet ACK table row count and highest acknowledged `stream_seq`.
- Fleet telemetry table row count when `record_pass_telemetry=true`.
- Server auth audit entries for configure/start/stop/delete actions.

Alert when any of these conditions persist for more than one worker interval:

- `worker_running=false` while the pilot is expected to run.
- `worker_load_errors_total` or `worker_observer_errors_total` increases.
- `failure_budget_exhausted=true` in the last pass.
- `highest_acked_stream_seq` stops advancing while the edge outbox grows.
- `late_count`, `permanent_failure_count`, or `rejected_count` increases.
- Admin rate-limit/audit denial events appear for pilot operations.

## Fault And Restart Validation

Before a pilot handles real customer data, run a bounded validation window that
covers:

- server restart with persisted config and `checkpoint_path`,
- fleet HTTP outage and recovery,
- edge/fleet node replacement using the same ACK ledger,
- duplicate `feed_event_id` replay,
- late `stream_seq` input,
- `max_outbox_bytes` and `outbox_query_limit` cap behavior,
- `max_failures_per_pass` exhaustion and recovery after the failing input is
  fixed.

Minimum acceptance target for the pilot window:

- 24 hours of continuous worker execution.
- Zero unbounded load attempts.
- Zero lost ACKed events after restart.
- Zero secret leakage in logs or audit rows.
- Fleet audit queries converge after injected outage/restart faults.

## Rollback

Disable the runtime without deleting edge/fleet SQL data:

```bash
curl -X DELETE http://localhost:8123/admin/edge-fleet-connector \
  -H "Authorization: Bearer $ADMIN_KEY"
```

Rollback expectations:

- The worker stops and checkpoint state is saved when `checkpoint_path` is set.
- Persisted SQL/HTTP adapter config is removed when config persistence is
  enabled.
- Edge-local safety decisions continue independently of fleet convergence.
- Re-enabling with the same ACK ledger and checkpoint must skip already ACKed
  events.

## Promotion Criteria

The controlled pilot can be considered for Limited Operator Feature promotion
only after all of these are true:

- Two or more pilot environments complete the validation window without data
  loss.
- Operators confirm dashboard and alert coverage for every required signal.
- Public docs describe exact supported schemas, limits, rollback, and
  non-goals.
- A release PR records passing x86_64 CTest, aarch64 Graviton CI, integration
  tests, and live S3 smoke when release proof requires it.
- A new production gate explicitly changes the status from controlled pilot to
  Limited Operator Feature.

Full GA requires a separate decision after longer soak, broader scale data,
and public support commitments.
