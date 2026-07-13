# 222: Physical AI Edge/Fleet Controlled Pilot Rollout

Date: 2026-07-12
Status: Complete

## Context

Devlogs 212-215 completed the built-in SQL/HTTP adapter, durable config and
checkpoint reload, idempotent sink contract, bounded SQL/backpressure controls,
admin audit/rate-limit coverage, server-runtime restart soak, node-replacement
validation, and two-live-HTTP-node convergence. The remaining product question
was whether to proceed as a controlled pilot, limited operator feature, or full
GA. The chosen rollout scope is controlled pilot.

## Changes

- Added `docs/operations/PHYSICAL_AI_EDGE_FLEET_CONTROLLED_PILOT.md` with
  supported scope, non-goals, required admin configuration, idempotency
  contract, monitoring signals, alerts, fault/restart validation, rollback, and
  promotion criteria.
- Updated the Physical AI production-readiness plan and experiment-governance
  table to record controlled pilot as the explicit rollout decision.
- Updated the ingestion design and HTTP API reference so public wording now
  states the controlled-pilot scope directly.
- Replaced the backlog rollout-decision item with the remaining controlled
  pilot soak/fault and promotion-evidence work.
- Added a completed-feature entry for the rollout decision and runbook.

## Verification

- Documentation-only change.
- `git diff --check`

## Follow-ups

- Run a 24h+ live pilot soak/fault window.
- Collect real pilot dashboard/alert evidence for worker lag, ACK gaps,
  retries, failures, late events, and admin audit/rate-limit events.
- Reopen a production gate only when promoting from controlled pilot to Limited
  Operator Feature.
