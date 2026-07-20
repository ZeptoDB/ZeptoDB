# Research Experiment Governance

Date: 2026-06-21

This policy keeps research evidence, experimental runtime paths, and promoted
product features separate. It applies to all ZeptoDB research work, including
Action-Outcome Memory, AIOps, Physical AI, cluster execution experiments, and
future benchmark-driven prototypes.

## Classification

Every experiment must declare exactly one current classification.

| Classification | Meaning | Allowed surfaces |
| --- | --- | --- |
| Research-only | The work produces evidence, fixtures, reports, or scripts without changing runtime behavior. | `docs/research/`, `docs/research/tools/`, `docs/research/results/`, fixtures |
| Experimental runtime path | The work changes source code, APIs, routing, query planning, or telemetry to validate a bounded hypothesis. It is not yet a general product contract. | Runtime code, tests, devlog, API docs marked experimental, design docs with explicit boundaries |
| Promoted product feature | The work is ready to describe as supported product behavior for its documented scope. | Runtime code, API docs, README, operations docs, completed-feature docs |

Do not describe an experimental runtime path as a completed product feature.
Completed experiment validation and completed product readiness are different
states.

## Required Experiment Header

Each experiment document must include:

- Date.
- Status.
- Classification.
- Goal.
- Hypothesis.
- Procedure.
- Acceptance criteria.
- Result.
- Interpretation.
- Next product or research step.

Use status values that preserve the boundary:

- `Research complete` for research-only evidence.
- `Experimental validation complete` for runtime paths that pass the experiment
  but are not product-ready.
- `Product feature complete` only after the promotion gates below are met.

## Experimental Runtime Path Checklist

If an experiment changes source code or exposes an API, the devlog and design
docs must include an `Experimental Boundary` section with:

- Intended workload and table/query shape.
- Explicit non-goals.
- Hard limits or capacity bounds.
- Telemetry that proves the path was used and whether it failed or rejected
  input.
- Failure behavior and fallback behavior.
- Security and admin requirements for any endpoint or control-plane knob.
- Whether state is persisted across restart.
- Rollback or disable plan.
- Product-promotion criteria.

API documentation must label the surface as experimental unless it has passed
the product-promotion gates. `docs/COMPLETED.md` may record the experiment as
validated, but it must not imply broad product support.

## Product-Promotion Gates

Promote an experimental runtime path only after these are true for the intended
scope:

- The target workload is named and bounded.
- Behavior is deterministic across restart, node replacement, and rolling
  upgrade, or the docs explicitly state the weaker guarantee.
- Any operator-controlled state is persisted in DDL, catalog metadata, or a
  documented config layer when persistence is expected.
- The path has row, memory, latency, or concurrency limits where unbounded
  coordinator work is possible.
- Telemetry covers success, rejection, fallback, and error counts.
- Tests cover happy path, boundary limits, invalid inputs, and at least one
  distributed or failure-mode edge case when applicable.
- User-facing docs include limits, non-goals, and migration guidance.
- Security and RBAC behavior are tested for new HTTP or admin APIs.
- The backlog no longer lists open promotion blockers for that feature.

## Current Action-Outcome Classifications

| Item | Classification | Rationale |
| --- | --- | --- |
| Generic SQL `INSERT ... VALUES` materialization for declared tables | Promoted product feature | This is general SQL table correctness, not Action-Outcome-specific behavior. |
| String-key hash JOIN materialization and alias-aware JOIN predicates | Promoted product feature | This fixes general SQL JOIN correctness for declared string keys and predicates. |
| Cluster full-data window materialization for declared operational tables | Promoted product feature | Promoted for bounded declared-table operational/control queries only. Devlog 219 adds explicit enable/disable policy, row cap, estimated materialized-byte cap, optional latency cap, cap telemetry, and fail-closed errors instead of partial scatter fallback. |
| Bounded small-table distributed hash JOIN | Promoted product feature | Promoted for simple declared-table hash JOINs over small operational/control tables only. Devlog 218 adds explicit enable/disable policy, per-side row cap, estimated materialized-byte cap, optional latency cap, cap telemetry, and focused rejection tests. It is still not a general distributed JOIN optimizer. |
| Legacy distributed HTTP SELECT override | Experimental runtime path | Disabled by default and available only through `--allow-experimental-distributed-queries`. The coordinator path does not yet carry request-owned row/byte limits or cancellation across RPC; promotion requires bounded distributed execution, telemetry, and fault cleanup tests. |
| Non-atomic Arrow Flight DoPut override | Experimental runtime path | Disabled by default and available only through `--allow-non-atomic-put` for disposable compatibility tests. Rows from earlier batches may remain after a later stream error; promotion requires atomic commit, retry/idempotency policy, recovery evidence, and partial-failure telemetry. |
| Runtime operational table placement policy | Experimental runtime path | Validated for explicit Action-Outcome control-table placement. Devlog 217 adds catalog/DDL persistence and coordinator restart re-apply, but placement is still not a rebalance/failover policy. |
| Physical AI edge/fleet feed connector, SQL/HTTP replay adapter, server lifecycle, and worker runtime | Experimental runtime path, controlled pilot approved | C++ runtime state machine plus standalone live SQL/HTTP replay validates bounded delivery, ACK checkpoint reload, duplicate/late handling, outage-style retry, dropped-event retry, and fleet audit convergence. Devlogs 204-205 add admin-gated server lifecycle state, metrics, and a bounded worker hook/loop. Devlogs 212-215 add the built-in SQL/HTTP adapter, persisted config, idempotent sink contract, bounded load/backpressure controls, admin audit/rate-limit coverage, server-runtime restart soak, node-replacement evidence, and two-live-HTTP-node convergence. Devlog 222 records the product decision: controlled pilots may use the admin-gated SQL/HTTP adapter under the operations runbook, while Limited Operator Feature and GA promotion remain blocked until additional soak/fault evidence and a new production gate are complete. |
| Physical AI Action-Outcome supervisor runtime and SQL adapter | Experimental runtime path | Devlogs 206-207 add a shadow-only, admin-gated runtime, hook contract, SQL-backed proposal/history/decision/evidence adapter, default demo schema bootstrap, advisory historical-outcome policy, and status plus Prometheus metrics. Experiment 021 / devlog 208 adds A/B shadow evidence and a decision-ledger restart idempotency guard. Devlog 209 adds server-local durable SQL adapter config and a live HTTP restart reinstall regression. Devlog 210 adds SQL catalog-backed config, atomic commit-ledger sink repair, managed SQL worker leases with heartbeat/expiry, owner id/epoch fencing, mutating-admin RBAC/audit/rate-limit coverage, per-pass decision/sink error budgets, and a SQL-backed soak/fault harness. Experiments 022-023 / devlog 211 close the node-replacement validation and commit-ledger stress questions for controlled shadow pilots. Devlog 216 records the rollout decision in code and API: `controlled_shadow_pilot` is the only accepted rollout stage, while `promoted_operator_feature` is rejected until GA/operator gates are reopened. It remains experimental and shadow-only: the SQL lease is not consensus, and the commit ledger is a supervisor-specific sink contract rather than a generic multi-table transaction primitive. |

## Documentation Routing

Record experiments as follows:

- `docs/research/*_experiment_NNN.md`: procedure and acceptance criteria.
- `docs/research/results/*_NNN.md`: immutable run result and interpretation.
- `docs/research/action_outcome_research_process_log.md`: chronological work
  log for Action-Outcome research.
- `docs/devlog/NNN_*.md`: implementation or policy changes made in the repo.
- `docs/design/`: architectural boundaries and future promotion criteria.
- `docs/api/`: only if a runtime/API surface exists; label experimental until
  promoted.
- `docs/BACKLOG.md`: remaining promotion blockers.
- `docs/COMPLETED.md`: completed experiments or product features, clearly
  distinguished by classification.
