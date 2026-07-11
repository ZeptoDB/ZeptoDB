# Physical AI Edge/Fleet Connector Production Readiness Plan

Date: 2026-06-23
Status: Experimental runtime path

This plan lists the gates required before the Physical AI edge/fleet
Action-Outcome connector can be promoted from research/experimental runtime to
a production ZeptoDB feature.

## Production Meaning

In production, the connector is the bridge between edge-local safety memory and
fleet-global audit memory:

- edge systems keep immediate safety decisions local and latency-bounded,
- edge outbox events are transferred through bounded, retryable passes,
- fleet systems converge later for audit, replay, and policy improvement,
- every decision remains explainable as time-series action/outcome evidence.

## Required Gates

| Gate | Status | Completion Criteria |
| --- | --- | --- |
| Runtime state machine | Done, devlog 202 | Bounded passes, duplicate/late handling, retry accounting, checkpointing, metrics. |
| Live SQL/HTTP replay adapter proof | Done, devlog 203 | Two ZeptoDB HTTP nodes replay outage, dropped, duplicate, late, restart, ACK convergence, and audit JOIN checks. |
| Server lifecycle control plane | Done, devlog 204 | Admin-gated configure/start/stop/delete/status and metrics. |
| Server-managed worker foundation | Done, devlog 205 | Runtime owns bounded `runOnce()` and background worker loop with injected loader/sink hooks, status, and metrics. |
| Built-in SQL/HTTP adapter | Done, devlog 212 | Server can load the configured edge outbox table and apply fleet inbox/final/ACK/telemetry rows without `zepto_edge_fleet_replay` or embedding-only hooks. |
| Persisted connector configuration | Done, devlog 213 | Server-local versioned config persistence reloads SQL/HTTP hooks and enabled state; ACK/cursor state persists separately through `checkpoint_path`. |
| Idempotent sink contract | Done, devlog 213 | HTTP/design docs define `feed_event_id` idempotency, ACK-boundary replay, duplicate final-row behavior, and ACK-ledger source-of-truth rules. |
| Backpressure and rate limits | Done, devlog 213 | Worker has explicit max rows, max bytes, retry/backoff, and per-pass failure-budget behavior. |
| Live restart/fault tests | Done, devlogs 203, 213, and 215 | Standalone two-node replay covers live outage/fault cases; server tests cover HTTP restart config reload and checkpoint replay skip over live tables; devlog 215 adds server-runtime restart soak, node-replacement, and two-live-HTTP-node convergence through the built-in SQL/HTTP adapter. |
| Security and audit | Done, devlog 213 | Admin endpoints are RBAC/rate-limit gated and mutating edge/fleet config/start/stop/clear outcomes emit audit-buffer events without adapter secrets. |
| Cross-architecture verification | Done, devlogs 212-213 | x86_64 and aarch64 focused/broad builds/tests pass with matching connector semantics. |
| Promotion documentation | Partial, devlog 213 | API/design docs describe supported scope, limits, idempotency, and rollback/disable procedure. README-level GA positioning remains intentionally conservative while the path is experimental. |

## Implementation Sequence

1. Built-in SQL/HTTP adapter (done in devlog 212):
   - extract reusable SQL adapter pieces from `zepto_edge_fleet_replay`,
   - bind them to `EdgeFleetConnectorRuntimeHooks`,
   - keep table names and limits explicit,
   - reject unsupported schemas with clear errors.

2. Persistence (done in devlog 213):
   - persist runtime config separately from ACK checkpoint state,
   - load config before enabling the worker,
   - document rollback and disable behavior.

3. Operational hardening (done in devlog 213 for controlled pilots):
   - add bounded retry pacing,
   - expose last error and per-pass failure-budget status,
   - add audit events for admin lifecycle changes.

4. Live production-style validation:
   - run two-node edge/fleet replay through the server runtime,
   - inject dropped, duplicated, late, outage, restart, and ACK-boundary cases,
   - verify edge-local safety decisions remain independent of fleet outage,
   - verify fleet audit eventually converges.

5. Promotion review:
   - run focused cross-architecture verification,
   - update API/design/README docs,
   - mark the feature as promoted only after governance gates pass.

## Current Recommendation

The server path is now suitable for controlled experimental pilots behind the
admin-gated runtime: durable config reload, checkpoint cursor reload,
ACK-ledger paging for bounded SQL loads, idempotent sink docs,
retry/failure-budget controls, restart regressions, and admin audit coverage
are in place. Devlog 215 adds the missing server-runtime soak, node-replacement,
and two-live-HTTP-node convergence evidence. The remaining promotion work is an
explicit GA/operator rollout decision, public positioning update, and
release-grade cross-architecture validation for the chosen supported scope.
