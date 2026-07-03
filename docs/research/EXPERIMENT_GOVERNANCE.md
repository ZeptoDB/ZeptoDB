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
| Cluster full-data window materialization for declared operational tables | Experimental runtime path | Correct for the validated replay shape, but coordinator-local full-data materialization still needs product limits and telemetry for large tables. |
| Bounded small-table distributed hash JOIN | Experimental runtime path | Useful for small operational/control tables under a strict row cap, but not a general distributed JOIN optimizer. |
| Runtime operational table placement policy | Experimental runtime path | Validated for explicit Action-Outcome control-table placement, but placement is not yet catalog/DDL persisted or rebalance-aware. |
| Physical AI edge/fleet feed connector, SQL/HTTP replay adapter, server lifecycle, and worker runtime | Experimental runtime path | C++ runtime state machine plus standalone live SQL/HTTP replay validates bounded delivery, ACK checkpoint reload, duplicate/late handling, outage-style retry, dropped-event retry, and fleet audit convergence. Devlogs 204-205 add admin-gated server lifecycle state, metrics, and a bounded worker hook/loop. It still needs a built-in SQL/HTTP adapter, persisted connector config/catalog metadata, documented idempotent sink requirements, long-running fault/soak tests, and cross-architecture verification before product promotion. |
| Physical AI Action-Outcome supervisor runtime | Experimental runtime path | Devlog 206 adds a shadow-only, admin-gated runtime and hook contract for loading action proposals, checking idempotency, producing advisory decisions, fail-closing decision errors to manual review, sinking decisions/evidence, and exposing status plus Prometheus metrics. It still needs built-in SQL-backed adapters, durable config/catalog state, broader RBAC/auth tests for mutating controls, production schema docs, restart/node-replacement validation, long-running fault/soak tests, rate/backpressure limits, and cross-architecture verification before product promotion. |

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
