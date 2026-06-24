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
| Built-in SQL/HTTP adapter | Open | Server can load the configured edge outbox table and apply fleet inbox/final/ACK/telemetry rows without `zepto_edge_fleet_replay` or embedding-only hooks. |
| Persisted connector configuration | Open | Connector config survives restart through catalog metadata, config file, or documented runtime persistence. |
| Idempotent sink contract | Open | Operator docs define event idempotency, ACK-boundary behavior, duplicate application behavior, and required table keys. |
| Backpressure and rate limits | Open | Worker has explicit max rows, max bytes, retry/backoff, and failure-budget behavior. |
| Live restart/fault tests | Open | Tests cover edge outage, fleet outage, duplicate events, late events, ACK failure, process restart, and node replacement over live tables. |
| Security and audit | Partial | Admin endpoints are RBAC-gated; still need audit events for config/start/stop and secret-free adapter config docs. |
| Cross-architecture verification | Open | x86_64 and aarch64 focused builds/tests pass with matching connector semantics. |
| Promotion documentation | Open | README/API/design docs describe supported scope, limits, non-goals, and rollback/disable procedure. |

## Implementation Sequence

1. Built-in SQL/HTTP adapter:
   - extract reusable SQL adapter pieces from `zepto_edge_fleet_replay`,
   - bind them to `EdgeFleetConnectorRuntimeHooks`,
   - keep table names and limits explicit,
   - reject unsupported schemas with clear errors.

2. Persistence:
   - persist runtime config separately from ACK checkpoint state,
   - load config before enabling the worker,
   - document rollback and disable behavior.

3. Operational hardening:
   - add exponential backoff or bounded retry pacing,
   - expose last error, last successful pass timestamp, and last ACK timestamp,
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

The next engineering task is the built-in SQL/HTTP adapter. That is the largest
remaining difference between the current experimental runtime and a production
candidate because it removes the standalone replay tool from the operational
path.
