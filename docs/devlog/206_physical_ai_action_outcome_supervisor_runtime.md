# 206: Physical AI Action-Outcome Supervisor Runtime

Date: 2026-07-03
Status: Complete

## Context

The Physical AI Action-Outcome research path had replay evidence for using
historical action outcomes to suppress risky repeated actions, but `main` did
not yet have a production-shaped runtime surface for operating that idea. The
first safe promotion step is an experimental, shadow-only lifecycle manager
that can be admin-controlled, observed, tested, and later wired to concrete
SQL-backed source and sink adapters.

## Experimental Boundary

- Workload scope: bounded proposal batches for Physical AI action supervision.
- Mode: `shadow` only. The runtime writes advisory decisions through a sink
  hook; it does not publish actuator commands or enforce robot control.
- Input/output ownership: embedding code supplies proposal loader,
  already-decided, decision provider, and decision sink hooks. Built-in SQL
  source/sink adapters are follow-up work.
- Failure behavior: invalid proposals are rejected, already-decided proposals
  are skipped, decision-provider failures produce fail-closed
  `suppress_no_evidence` decisions with `manual_review` as the default final
  action, and sink failures surface as worker failures.
- Limits: each pass is bounded by `batch_limit`; worker pacing is controlled by
  `worker_poll_interval_ms`; repeated load/sink failures trip
  `failure_budget_exhausted`.
- Security: HTTP lifecycle control is admin-gated through the existing admin
  route guard; focused tests cover status access for missing, writer, and admin
  credentials.
- Persistence: configuration and counters are process-local in this PR.
- Disable plan: `DELETE /admin/action-outcome-supervisor` stops the runtime
  and clears process-local state.

## Changes

- Added `ActionOutcomeSupervisorRuntime` in the feeds layer with:
  - `ActionOutcomeProposal`
  - `ActionOutcomeDecision`
  - hook types for proposal loading, already-decided checks, decision
    generation, and decision sinks
  - `configure()`, `setWorkerHooks()`, `start()`, `stop()`, `clear()`,
    `runOnce()`, `snapshot()`, and `formatPrometheus()`
- Added admin HTTP endpoints:
  - `GET /admin/action-outcome-supervisor`
  - `POST /admin/action-outcome-supervisor`
  - `DELETE /admin/action-outcome-supervisor`
- Added `HttpServer::set_action_outcome_supervisor_runtime_hooks()` for
  embeddings that provide SQL, ROS, simulator, or VLA source/sink callbacks.
- Added Prometheus metrics for configured/enabled/running state, worker passes,
  processed proposals, duplicate skips, allow/suppress decisions, fail-closed
  decisions, evidence rows, worker failures, and last-pass latency.
- Added focused unit and HTTP tests for lifecycle, invalid config, bounded
  batch processing, idempotency skips, fail-closed decision errors, missing
  hooks, failure budget behavior, HTTP status, and metrics.

## Verification

```bash
cmake --build build --target zepto_tests -j$(nproc)

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSupervisorRuntimeTest.*:MetricsProviderTest.ActionOutcomeSupervisor*:EdgeFleetConnectorAdminAuthTest.*'

./build/tests/zepto_tests
```

Results:

- Build: pass.
- Focused Action-Outcome runtime, HTTP lifecycle, and admin auth tests: pass,
  12/12.
- Full C++ suite: 1544/1546 passed, with the live S3 opt-in test skipped and 3
  disabled tests unchanged. `HttpCluster.DynamicMode_StandaloneToCluster`
  failed once in the full run; an immediate isolated repeat passed 3/3.

## Follow-ups

- Build the concrete SQL-backed proposal loader and decision/evidence sink
  adapters.
- Persist supervisor config/catalog state across restart.
- Add broader RBAC/auth regression coverage for mutating supervisor controls.
- Document production table schemas, idempotency keys, and sink transaction
  requirements.
- Add long-running restart, outage, duplicate, similar-but-different scenario,
  and fault-injection soak tests over live ZeptoDB tables.
- Run cross-architecture verification before product promotion.
