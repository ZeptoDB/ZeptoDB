# 207: Physical AI Action-Outcome SQL Adapter

Date: 2026-07-04
Status: Complete

## Context

Devlog 206 added the shadow-only `ActionOutcomeSupervisorRuntime`, but the
runtime still required embedding code to provide proposal loading, idempotency,
decision, and sink hooks. The next production-shaped step is a ZeptoDB-backed
adapter that can read action proposals from SQL tables, compute a simple
historical outcome policy, and persist decision/evidence rows through native
SQL.

## Experimental Boundary

- Classification: experimental runtime path.
- Workload scope: bounded Physical AI action proposal supervision over the
  default proposal/history/decision/evidence table contract.
- Mode: still `shadow` only. Decisions are advisory and must not be described
  as robot actuator enforcement.
- Source: SQL `SELECT` from `proposal_table`, ordered by proposal timestamp
  and bounded by `proposal_query_limit` or `runtime.batch_limit`.
- Idempotency: SQL lookup in `decision_table` by `proposal_id`. The decision
  row is the durable ACK boundary for duplicate suppression.
- Decision policy: a simple deterministic historical-outcome policy over
  matching rows in `history_table`; rows with `outcome_score` below the
  configured threshold count as failed outcomes.
- Sink: writes one evidence summary row first, then writes the decision row.
  This avoids losing evidence when the decision row later suppresses retries,
  but it is not transactional; decision insert failure after evidence insert
  can leave duplicate evidence summaries on retry.
- Security: HTTP lifecycle and SQL-adapter installation are admin-gated by the
  existing `/admin/action-outcome-supervisor` route guard.
- Persistence: runtime config and worker counters remain process-local.
- Disable plan: `DELETE /admin/action-outcome-supervisor` stops the worker and
  clears the process-local config/hooks.

## Changes

- Added `include/zeptodb/server/action_outcome_sql_adapter.h` and
  `src/server/action_outcome_sql_adapter.cpp`.
- Added `ActionOutcomeSqlAdapterConfig`, config validation, default SQL schema
  bootstrap, and `makeActionOutcomeSqlRuntimeHooks()`.
- Added `HttpServer::set_action_outcome_supervisor_sql_adapter()` for
  embedding code that wants SQL-backed hooks without hand-assembling them.
- Extended `POST /admin/action-outcome-supervisor` with
  `sql_adapter_enabled`, `sql_adapter_create_tables`,
  `proposal_query_limit`, `history_evidence_limit`,
  `suppress_min_failure_count`, `suppress_outcome_score_below`, and selected
  column-name overrides.
- Added focused C++ tests for default table creation, SQL-backed
  load/decide/sink flow, duplicate suppression through decision rows,
  no-evidence fail-closed decisions, unsafe identifier rejection, and HTTP
  admin installation.

## Verification

```bash
cmake --build build --target zepto_tests -j$(nproc)

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSqlAdapter*:MetricsProviderTest.ActionOutcomeSupervisorAdminInstallsSqlAdapter:ActionOutcomeSupervisorRuntimeTest.*:MetricsProviderTest.ActionOutcomeSupervisor*'
```

Results:

- Build: pass.
- Focused Action-Outcome runtime, SQL adapter, and HTTP admin tests: pass,
  16/16.
- Full C++ suite was not rerun for this incremental adapter change in this
  pass.

## Follow-ups

- Add a transaction-like sink contract or idempotent evidence key to remove
  duplicate evidence summaries after decision insert failures.
- Persist supervisor SQL adapter config in catalog/config state.
- Add cluster-safe worker ownership so only one node processes a proposal
  shard.
- Add long-running restart, node replacement, duplicate proposal, and
  similar-but-different scenario soak tests over live SQL tables.
- Run cross-architecture verification before product promotion.
