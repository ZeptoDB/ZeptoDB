# Action-Outcome Memory Research Process Log

Date started: 2026-06-13
Branch: `codex/aiops-time-series-memory-research`

This log records the concrete research process for the Action-Outcome Memory
Engine work. It is append-only by convention: add new entries rather than
rewriting prior decisions unless a correction is explicitly needed.

## Session 001 - Research Kickoff

Date: 2026-06-13

### Request

Create a sequential, concrete research plan and start the work. Record every
step.

### Current State

- Research branch confirmed: `codex/aiops-time-series-memory-research`.
- Existing research files:
  - `docs/research/time_series_agent_memory_edge.md`
  - `docs/research/aiops_time_series_memory_research_data.md`
  - `docs/research/action_outcome_memory_engine_plan.md`
  - `docs/research/action_outcome_research_execution_roadmap.md`
- Current research direction: Action-Outcome Memory Engine for AIOps first,
  with later extension to physical AI and agentic SOC.

### Execution Plan

1. Create this persistent process log.
2. Define a first action-outcome episode schema.
3. Create a small 20+ episode fixture for initial replay experiments.
4. Document the first experiment procedure.
5. Verify generated files and record the result.

### Decisions

- Start with AIOps rather than robotics because AIOps has clearer incident,
  alert, runbook, telemetry, and postmortem structures.
- Begin with replay-only research, not live autonomous remediation.
- Store machine-readable fixture data separately from narrative research notes.
- Include failed and rolled-back actions in the fixture because negative outcomes
  are essential to action-outcome memory.
- After each completed work unit, report both what was completed and the best
  next step to take.

### Files Created In This Session

- `docs/research/action_outcome_research_process_log.md`
- `docs/research/action_outcome_episode_schema.md`
- `docs/research/fixtures/action_outcome_episodes.json`
- `docs/research/action_outcome_replay_experiment_001.md`
- `docs/research/tools/action_outcome_replay.py`
- `docs/research/results/action_outcome_replay_001.md`

### Verification

- JSON fixture validation command: `jq empty docs/research/fixtures/action_outcome_episodes.json`
- Episode count command: `jq '.episodes | length' docs/research/fixtures/action_outcome_episodes.json`
- Result: 24 episodes.
- Incident family distribution:
  - `checkout_latency_after_deploy`: 4
  - `inventory_db_connection_pool`: 4
  - `order_queue_backlog`: 4
  - `payment_dependency_timeout`: 4
  - `recommendation_cache_stale`: 4
  - `search_index_memory_leak`: 4

### Correction

The initial plan called for a 20-episode minimum fixture. During creation, the
fixture was expanded to 24 episodes by adding a sixth incident family,
`search_index_memory_leak`, because memory-leak behavior is a useful contrast
case for restart, rollback, traffic drain, and scale-out. Related documents were
updated to say 24 episodes where they refer to the actual initial fixture.

### Experiment 001 Execution

Command:

```bash
python3 docs/research/tools/action_outcome_replay.py \
  --output docs/research/results/action_outcome_replay_001.md
```

Result summary:

- Query episodes: 6
- Top-3 successful action hit rate: 1.00
- Failed-action avoidance rate: 1.00

Interpretation:

- This is a fixture-level sanity check, not a benchmark claim.
- The initial deterministic scorer successfully avoided repeating failed actions
  in the selected query cases.
- The result is likely optimistic because the fixture is synthetic and incident
  families are cleanly separated.
- Next evaluation must add component score breakdowns, baseline action
  recommendations, stricter validation, and eventually noisy public/lab data.

### Status

Phase 0 and the first part of Phase 1 are started:

- Process log created.
- Episode schema created.
- Initial 24-episode fixture created.
- First replay experiment procedure created.
- Deterministic replay evaluator created.
- Experiment 001 result report generated.

Next step: add score breakdowns and baseline recommended-action comparisons so
the result can explain why each action was recommended.

## Session 002 - Score Breakdown And Fixture Validation

Date: 2026-06-13

### Request

Continue the research and report what to do next after each completed work unit.

### Completed Work

1. Updated `docs/research/tools/action_outcome_replay.py`.
   - Added baseline action recommendations for keyword, text-only, time-series,
     and action-outcome retrieval.
   - Added top action-outcome candidate component breakdowns:
     - symptom similarity,
     - temporal motif similarity,
     - topology similarity,
     - change similarity,
     - action-outcome score,
     - postmortem text similarity,
     - recency,
     - risk penalty.

2. Regenerated `docs/research/results/action_outcome_replay_001.md`.
   - Query episodes: 6
   - Top-3 successful action hit rate: 1.00
   - Failed-action avoidance rate: 1.00
   - Result now includes baseline recommendations and top-candidate score
     breakdowns.

3. Added `docs/research/tools/validate_action_outcome_fixture.py`.
   - Validates required fields.
   - Validates action classes, policy decisions, risk tiers, and outcome labels.
   - Validates timestamp ordering.
   - Validates incident family counts.

4. Generated `docs/research/results/action_outcome_fixture_validation_001.md`.
   - Status: pass
   - Episodes: 24
   - Incident families: 6
   - Validation errors: none

### Verification Commands

```bash
python3 docs/research/tools/action_outcome_replay.py \
  --output docs/research/results/action_outcome_replay_001.md

python3 docs/research/tools/validate_action_outcome_fixture.py \
  --output docs/research/results/action_outcome_fixture_validation_001.md

python3 -m py_compile \
  docs/research/tools/action_outcome_replay.py \
  docs/research/tools/validate_action_outcome_fixture.py
```

### Notes

- `py_compile` generated a temporary `__pycache__` directory under
  `docs/research/tools/`; it was removed because it is not a research artifact.
- The current replay result is still optimistic because the fixture is synthetic
  and incident families are cleanly separated.

### Next Best Step

Add **top-3 per-component retrieval breakdowns** to the replay report.

Why this is next:

- Top-1 explanation is useful, but action recommendations aggregate multiple
  retrieved episodes.
- If the top recommendation comes from a weak or accidental candidate, the report
  should expose that.
- Top-3 breakdowns will make it easier to spot overfitting to incident type,
  service name, or clean synthetic labels.

## Session 003 - Top-3 Retrieval Breakdown

Date: 2026-06-13

### Request

Continue from the previous next best step.

### Completed Work

1. Updated `docs/research/tools/action_outcome_replay.py`.
   - Added top-3 action-outcome candidate breakdowns for each query.
   - Each candidate now reports total score plus:
     - symptom similarity,
     - temporal motif similarity,
     - topology similarity,
     - change similarity,
     - action-outcome score,
     - text similarity,
     - recency,
     - risk penalty.

2. Regenerated `docs/research/results/action_outcome_replay_001.md`.
   - The report now shows baseline action recommendations and top-3 candidate
     explanations.

### Verification Commands

```bash
python3 docs/research/tools/action_outcome_replay.py \
  --output docs/research/results/action_outcome_replay_001.md
```

### Observations

- The top-3 breakdown exposes a useful weakness: for `aoe_search_003`, the third
  action-outcome candidate is `aoe_checkout_003`, a different incident family.
- This means action-outcome and change-context scores can pull in cross-family
  candidates when the action class and deploy context look similar.
- Cross-family retrieval is not always bad, but in this fixture it is likely a
  weak candidate and should not strongly influence action ranking.

### Next Best Step

Tune action aggregation and retrieval guardrails.

Why this is next:

- The report now explains candidate scores well enough to reveal retrieval
  mistakes.
- The next improvement should prevent a weak positive candidate from dominating
  several stronger negative or same-family cases.
- A good next experiment is to add a family/topology guardrail or ablation report
  that shows how recommendations change when action-outcome, topology, or change
  context is removed.

## Session 004 - Retrieval Guardrails And Robust Aggregation

Date: 2026-06-13

### Request

Start the next task after top-3 breakdown: tune action aggregation and retrieval
guardrails.

### Completed Work

1. Added `docs/research/action_outcome_guardrail_experiment_002.md`.
   - Defines Experiment 002.
   - Compares baseline action-outcome retrieval with guarded V2 retrieval.
   - Adds metrics for cross-family top-3 candidates and weak cross-family top-3
     candidates.

2. Added `docs/research/tools/action_outcome_guardrail_compare.py`.
   - Reuses Experiment 001 scoring functions.
   - Adds cross-family penalties:
     - same family: 0.00
     - strong cross-family topology/symptom match: 0.05
     - moderate cross-family topology match: 0.12
     - weak cross-family relation: 0.22
   - Adds robust action aggregation that separates positive and negative outcome
     evidence.

3. Generated `docs/research/results/action_outcome_guardrail_002.md`.

### Intermediate Finding

The first guarded run reduced cross-family top-3 retrieval but introduced a new
aggregation problem:

- Baseline cross-family top-3 candidates: 1
- Guarded cross-family top-3 candidates: 0
- Baseline failed-action avoidance rate: 1.00
- Guarded failed-action avoidance rate: 0.67

Cause:

- Retrieval guardrails only affected the top retrieved candidates.
- Action aggregation still used lower-ranked cross-family candidates, allowing
  weak cross-family success cases to push actions such as `restart` back into
  the recommendation list.

Fix:

- Exclude high-penalty cross-family candidates from guarded action aggregation.
- Downweight weakly allowed cross-family candidates by 50%.

### Final Result

After tuning aggregation:

- Baseline top-3 successful action hit rate: 1.00
- Guarded top-3 successful action hit rate: 1.00
- Baseline failed-action avoidance rate: 1.00
- Guarded failed-action avoidance rate: 1.00
- Baseline cross-family top-3 candidates: 1
- Guarded cross-family top-3 candidates: 0
- Baseline weak cross-family top-3 candidates: 1
- Guarded weak cross-family top-3 candidates: 0

### Verification Commands

```bash
python3 docs/research/tools/action_outcome_guardrail_compare.py \
  --output docs/research/results/action_outcome_guardrail_002.md

python3 -m py_compile \
  docs/research/tools/action_outcome_replay.py \
  docs/research/tools/action_outcome_guardrail_compare.py \
  docs/research/tools/validate_action_outcome_fixture.py

python3 docs/research/tools/validate_action_outcome_fixture.py \
  --output docs/research/results/action_outcome_fixture_validation_001.md
```

### Notes

- Guarded recommendation scores are ranking utilities, not probabilities.
- Guarded V2 changes top actions for some queries. This is acceptable only when
  the changed action remains within known useful actions for the incident family.
- The fixture remains synthetic and clean, so the result is still a directional
  research signal rather than a benchmark claim.

### Next Best Step

Add an ablation report that removes one signal family at a time.

Why this is next:

- Guarded V2 now performs better on cross-family control, but we still do not
  know which signal families are doing the most work.
- Ablation will show whether recommendations depend mainly on incident family,
  topology, change context, action outcomes, or text.
- This is necessary before adding noisy distractor episodes or mapping data into
  ZeptoDB tables.

## Session 005 - Signal Ablation Report

Date: 2026-06-13

### Request

Create the ablation report.

### Completed Work

1. Added `docs/research/action_outcome_ablation_experiment_003.md`.
   - Defines Experiment 003.
   - Specifies ablation variants for symptom, temporal motif, topology, change,
     action-outcome, text, recency, risk, and cross-family guardrail.

2. Added `docs/research/tools/action_outcome_ablation.py`.
   - Runs all ablation variants against the same six query episodes.
   - Reports top-3 hit rate, failed-action avoidance, cross-family top-3 count,
     weak cross-family top-3 count, and top-action changes.

3. Generated `docs/research/results/action_outcome_ablation_003.md`.

### Verification Commands

```bash
python3 docs/research/tools/action_outcome_ablation.py \
  --output docs/research/results/action_outcome_ablation_003.md

python3 -m py_compile \
  docs/research/tools/action_outcome_replay.py \
  docs/research/tools/action_outcome_guardrail_compare.py \
  docs/research/tools/action_outcome_ablation.py \
  docs/research/tools/validate_action_outcome_fixture.py

python3 docs/research/tools/validate_action_outcome_fixture.py \
  --output docs/research/results/action_outcome_fixture_validation_001.md
```

### Result Summary

- `full_guarded`:
  - Top-3 hit rate: 1.00
  - Failed-action avoidance: 1.00
  - Cross-family top-3: 0
  - Weak cross-family top-3: 0

- `no_action_outcome`:
  - Top-3 hit rate: 1.00
  - Failed-action avoidance: 1.00
  - Cross-family top-3: 0
  - Weak cross-family top-3: 0
  - Top action changes versus `full_guarded`: 0

- `no_cross_family_guardrail`:
  - Top-3 hit rate: 1.00
  - Failed-action avoidance: 0.83
  - Cross-family top-3: 1
  - Weak cross-family top-3: 1
  - Top action changes versus `full_guarded`: 1

### Interpretation

The ablation result is useful but not yet flattering to the core claim.

Key finding:

- Removing `action_outcome` does not change top actions or headline safety
  metrics on the current fixture.

What this means:

- The fixture is too clean. Incident family, topology, and action labels are
  enough to recover good recommendations.
- The current fixture does not yet prove that action-outcome memory is necessary.
- The cross-family guardrail is clearly useful: removing it reintroduces weak
  cross-family retrieval and lowers failed-action avoidance.

### Next Best Step

Add noisy distractor episodes and retrieval quality labels.

Why this is next:

- We need cases where symptom/topology/change similarity alone is ambiguous.
- We need episodes where the same action succeeds in one context and fails in a
  subtly different context.
- We need labels that distinguish "retrieved a useful action" from "retrieved a
  superficially similar episode".
- Only then can ablation show whether action-outcome memory adds real value.

## Session 006 - Noisy Distractors and Retrieval Quality Labels

Date: 2026-06-14

### Request

Continue to the next research step after the ablation report.

### Correction Before New Work

The first Experiment 003 ablation had a measurement bug: `no_action_outcome`
removed action-outcome from retrieval scoring but still used candidate outcome
labels during action aggregation.

Fix:

- In `docs/research/tools/action_outcome_ablation.py`, when
  `action_outcome` is removed, action aggregation now treats candidate outcomes
  as neutral positive support:
  `value = 1.0 if "action_outcome" in removed else replay.outcome_value(episode)`.

Corrected base result:

- `no_action_outcome` still has top-3 hit rate 1.00 and failed-action avoidance
  1.00.
- It now changes one top action versus `full_guarded`:
  `aoe_search_003` changes from `rollback` to `restart`.
- The fixture is still too clean because headline metrics remain perfect.

### Completed Work

1. Added `docs/research/action_outcome_noisy_distractor_experiment_004.md`.
   - Defines why noisy same-family distractors are required.
   - Defines retrieval quality labels: `useful`, `superficial`, and
     `misleading`.

2. Added `docs/research/fixtures/action_outcome_distractor_episodes.json`.
   - Adds eight synthetic distractor episodes.
   - Covers recommendation cache, order queue, search memory, payment timeout,
     and inventory DB pool incident families.
   - Each distractor shares surface symptoms with a query but differs in causal
     context or outcome.

3. Added `docs/research/fixtures/action_outcome_retrieval_quality_labels.json`.
   - Labels query-candidate pairs for retrieval quality.
   - Separates evidence usefulness from simple similarity.

4. Extended `docs/research/tools/action_outcome_ablation.py`.
   - Adds `--extra-fixture`.
   - Adds `--quality-labels`.
   - Reports labeled top-3 retrieval quality counts.

5. Extended `docs/research/tools/validate_action_outcome_fixture.py`.
   - Adds `--min-family-size` so distractor-only fixtures can be validated.

6. Generated:
   - `docs/research/results/action_outcome_ablation_004_noisy.md`
   - `docs/research/results/action_outcome_distractor_validation_004.md`

### Verification Commands

```bash
python3 -m json.tool \
  docs/research/fixtures/action_outcome_distractor_episodes.json >/dev/null

python3 -m json.tool \
  docs/research/fixtures/action_outcome_retrieval_quality_labels.json >/dev/null

python3 docs/research/tools/validate_action_outcome_fixture.py \
  --fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --min-family-size 1 \
  --output docs/research/results/action_outcome_distractor_validation_004.md

python3 docs/research/tools/action_outcome_ablation.py \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_ablation_004_noisy.md

python3 -m py_compile \
  docs/research/tools/action_outcome_replay.py \
  docs/research/tools/action_outcome_guardrail_compare.py \
  docs/research/tools/action_outcome_ablation.py \
  docs/research/tools/validate_action_outcome_fixture.py
```

### Result Summary

Experiment 004 intentionally made the fixture harder.

- `full_guarded`:
  - Top-3 hit rate: 1.00
  - Failed-action avoidance: 0.67
  - Cross-family top-3: 0
  - Weak cross-family top-3: 0
  - Labeled top-3 quality: useful 8, superficial 5, misleading 5

- `no_action_outcome`:
  - Top-3 hit rate: 1.00
  - Failed-action avoidance: 1.00
  - Top action changes versus `full_guarded`: 2
  - Labeled top-3 quality: useful 8, superficial 4, misleading 6

Critical per-query failures for `full_guarded`:

- `aoe_cache_002`: top action becomes `restart`, repeating a failed action
  because `aoe_distractor_cache_001` had a successful restart in a pod-local
  cache context.
- `aoe_queue_003`: top action becomes `restart`, repeating a failed action
  because `aoe_distractor_queue_001` had a successful restart for one stuck
  consumer partition.
- `aoe_search_003`: top action becomes `restart`; this avoids repeating
  scale-out, but it prefers temporary restart evidence over durable rollback.

### Interpretation

Experiment 004 found the first important negative result.

The current Action-Outcome Memory scoring is not safe enough. A successful
historical action is dangerous if its success came from a different causal
context. The issue is not cross-family retrieval; all failures are same-family
distractor failures.

Research implication:

- Action-outcome memory must be context-conditioned.
- The commercial product claim should be "safe conditional reuse of historical
  action outcomes", not simply "remember what worked before."

### Next Best Step

Add a context-conditioned outcome gate.

Why this is next:

- We need to reduce the influence of successful but context-incompatible
  distractors.
- The gate must preserve useful outcomes while suppressing outcomes from
  mismatched blast radius, metric discriminator, or change context.
- This is the missing safety layer before SQL-backed replay.

## Session 007 - Context-Conditioned Outcome Gate

Date: 2026-06-14

### Request

Continue to the next research step.

### Completed Work

1. Added `docs/research/action_outcome_context_gate_experiment_005.md`.
   - Defines the context-conditioned outcome gate.
   - Frames the core research claim as conditional outcome reuse.

2. Added `docs/research/tools/action_outcome_context_gate.py`.
   - Compares `full_guarded` with `context_gated`.
   - Adds heuristic context compatibility checks:
     - single-entity versus service-wide mismatch
     - CPU saturation mismatch
     - DB connection saturation mismatch
     - consumer error mismatch
     - cache flag versus deploy mismatch
     - change-type mismatch
   - Downweights positive or negative historical outcomes when the context gate
     finds incompatible causal context.
   - Reports gate suppressions with reasons.

3. Regenerated current reports so next-step text is consistent:
   - `docs/research/results/action_outcome_ablation_003.md`
   - `docs/research/results/action_outcome_ablation_004_noisy.md`
   - `docs/research/results/action_outcome_context_gate_005.md`
   - `docs/research/results/action_outcome_fixture_validation_001.md`
   - `docs/research/results/action_outcome_distractor_validation_004.md`

### Verification Commands

```bash
python3 docs/research/tools/action_outcome_context_gate.py \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_context_gate_005.md

python3 docs/research/tools/action_outcome_ablation.py \
  --output docs/research/results/action_outcome_ablation_003.md

python3 docs/research/tools/action_outcome_ablation.py \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_ablation_004_noisy.md

python3 docs/research/tools/validate_action_outcome_fixture.py \
  --output docs/research/results/action_outcome_fixture_validation_001.md

python3 docs/research/tools/validate_action_outcome_fixture.py \
  --fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --min-family-size 1 \
  --output docs/research/results/action_outcome_distractor_validation_004.md

python3 -m py_compile \
  docs/research/tools/action_outcome_replay.py \
  docs/research/tools/action_outcome_guardrail_compare.py \
  docs/research/tools/action_outcome_ablation.py \
  docs/research/tools/action_outcome_context_gate.py \
  docs/research/tools/validate_action_outcome_fixture.py
```

### Result Summary

`full_guarded` versus `context_gated` on the noisy fixture:

| Variant | Top-3 Hit Rate | Failed-Action Avoidance | Cross-Family Top3 | Weak Cross-Family Top3 | Gate Suppressions |
| --- | ---: | ---: | ---: | ---: | ---: |
| `full_guarded` | 1.00 | 0.67 | 0 | 0 | 0 |
| `context_gated` | 1.00 | 1.00 | 0 | 0 | 21 |

Top-action changes:

- `aoe_payment_002`: `restart` -> `traffic_drain`
- `aoe_cache_002`: `restart` -> `cache_purge`
- `aoe_queue_003`: `restart` -> `scale_out`
- `aoe_search_003`: `restart` -> `rollback`

Important suppressions:

- `aoe_distractor_cache_001` restart success was downweighted for
  `aoe_cache_002` because it was single-pod cache skew, not shared redis
  freshness lag.
- `aoe_distractor_queue_001` restart success was downweighted for
  `aoe_queue_003` because it was one stuck consumer partition, not
  capacity-driven backlog.
- `aoe_distractor_payment_001` scale-out success was downweighted for
  `aoe_payment_002` because the candidate had CPU saturation while the query had
  low CPU and external dependency timeout evidence.
- `aoe_distractor_search_001` restart success was downweighted for
  `aoe_search_003` because it was single-pod and not deploy-correlated.

### Interpretation

The context gate is the first version that supports the stronger commercial
claim:

- It keeps top-3 successful action coverage at 1.00.
- It restores failed-action avoidance from 0.67 to 1.00 under noisy same-family
  distractors.
- It gives auditable explanations for why historical outcomes were suppressed.

The labeled top-3 misleading count does not disappear. That is acceptable for
this experiment because the gate's main job is safer action aggregation, not
perfect retrieval filtering. A future retrieval-stage gate can reduce misleading
top-3 candidates earlier.

### Next Best Step

Map the episode fixture into ZeptoDB SQL tables and replay Experiment 005 through
database-backed retrieval.

Why this is next:

- The current engine is still a Python fixture evaluator.
- SQL-backed replay will connect the research to ZeptoDB's commercial edge:
  time-series-native storage, filtering, and replay.
- The table design can become the first product-facing Action-Outcome Memory
  schema.

## Session 008 - SQL-Backed Replay Contract

Date: 2026-06-18

### Request

Start `P0: Action-Outcome SQL-backed replay`.

### Completed Work

1. Added `docs/research/action_outcome_sql_backed_replay_experiment_006.md`.
   - Defines the first SQL-backed replay slice.
   - Separates the local SQL harness from the later live ZeptoDB HTTP run.
   - Defines success criteria for SQL row counts, JSON-control matching, and
     gate suppression materialization.

2. Added `docs/research/tools/action_outcome_sql_replay.py`.
   - Creates ZeptoDB-compatible SQL tables with `STRING`, `INT64`, and
     `DOUBLE` columns.
   - Loads base episodes, distractor episodes, and retrieval quality labels into
     a local SQL execution harness.
   - Reconstructs replay episodes from SQL rows.
   - Reruns `full_guarded` and `context_gated` replay from the SQL-backed
     episode store.
   - Writes recommendation rows and context-gate suppression rows back into SQL
     result tables.
   - Generates a replay report and a SQL seed/result file.

3. Generated:
   - `docs/research/results/action_outcome_sql_replay_006.md`
   - `docs/research/results/action_outcome_sql_replay_006.sql`

### Verification Commands

```bash
python3 docs/research/tools/action_outcome_sql_replay.py \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_sql_replay_006.md \
  --sql-output docs/research/results/action_outcome_sql_replay_006.sql

python3 -m py_compile \
  docs/research/tools/action_outcome_replay.py \
  docs/research/tools/action_outcome_guardrail_compare.py \
  docs/research/tools/action_outcome_ablation.py \
  docs/research/tools/action_outcome_context_gate.py \
  docs/research/tools/action_outcome_sql_replay.py \
  docs/research/tools/validate_action_outcome_fixture.py
```

### Result Summary

SQL table row counts:

| Table | Rows |
| --- | ---: |
| `action_outcome_episodes` | 32 |
| `action_outcome_episode_metrics` | 101 |
| `action_outcome_retrieval_quality_labels` | 26 |
| `action_outcome_replay_recommendations` | 18 |
| `action_outcome_gate_suppressions` | 21 |

Replay result:

| Variant | Source | Top-3 Hit Rate | Failed-Action Avoidance | Gate Suppressions |
| --- | --- | ---: | ---: | ---: |
| `full_guarded` | SQL | 1.00 | 0.67 | 0 |
| `context_gated` | SQL | 1.00 | 1.00 | 21 |
| `context_gated` | JSON control | 1.00 | 1.00 | 21 |

SQL-backed `context_gated` top actions matched the JSON control for all six
query episodes:

- `aoe_checkout_002`: `rollback`
- `aoe_payment_002`: `traffic_drain`
- `aoe_inventory_002`: `config_revert`
- `aoe_cache_002`: `cache_purge`
- `aoe_queue_003`: `scale_out`
- `aoe_search_003`: `rollback`

### Interpretation

Experiment 006 moves the work from "Python fixture only" to "SQL-backed replay
contract." The generated SQL now captures:

- episode state,
- metric discriminator rows,
- retrieval quality labels,
- replay recommendations,
- context-gate suppression explanations.

This is still not a live ZeptoDB server replay. The local SQL harness validates
schema shape and deterministic reconstruction, while the next step must execute
the generated SQL through ZeptoDB's HTTP SQL endpoint.

### Next Best Step

Run the generated SQL seed against a live ZeptoDB HTTP server and compare live
query results with `docs/research/results/action_outcome_sql_replay_006.md`.

Why this is next:

- It will close the gap between SQL-shaped research and actual ZeptoDB-backed
  replay.
- It will reveal any SQL dialect or type support issues before promoting the
  schema into a design document.
- It creates the first live demo path for Action-Outcome Memory on ZeptoDB.

## Session 009 - Live ZeptoDB SQL Endpoint Replay

Date: 2026-06-18

### Request

Start the next task after the SQL-backed replay contract.

### Completed Work

1. Added `docs/research/action_outcome_live_sql_replay_experiment_007.md`.
   - Defines live ZeptoDB HTTP SQL validation for the Action-Outcome seed.
   - Separates parser/ingest row-count validation from value-level semantic
     replay validation.
   - Records the current engine boundary around generic table INSERT
     projection.

2. Added `docs/research/tools/action_outcome_live_sql_replay.py`.
   - Loads `docs/research/results/action_outcome_sql_replay_006.sql` into a
     live `POST /` endpoint.
   - Counts expected rows from generated INSERT statements.
   - Queries live row counts for each Action-Outcome table.
   - Attempts top-action replay queries and writes a markdown report.

3. Updated `docs/research/tools/action_outcome_sql_replay.py`.
   - Emits `STRING` for textual columns.
   - Renames replay recommendation `rank` to `recommendation_rank` to avoid a
     live parser keyword collision.
   - Emits numeric default `0` instead of `NULL` for live ZeptoDB INSERT
     compatibility.

4. Generated:
   - `docs/research/results/action_outcome_live_sql_replay_007.md`
   - refreshed `docs/research/results/action_outcome_sql_replay_006.md`
   - refreshed `docs/research/results/action_outcome_sql_replay_006.sql`

### Verification Commands

```bash
python3 docs/research/tools/action_outcome_sql_replay.py \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_sql_replay_006.md \
  --sql-output docs/research/results/action_outcome_sql_replay_006.sql

build/zepto_http_server --port 19125 --no-auth --log-level error

python3 docs/research/tools/action_outcome_live_sql_replay.py \
  --url http://127.0.0.1:19125/ \
  --sql-file docs/research/results/action_outcome_sql_replay_006.sql \
  --output docs/research/results/action_outcome_live_sql_replay_007.md

python3 -m py_compile \
  docs/research/tools/action_outcome_live_sql_replay.py \
  docs/research/tools/action_outcome_sql_replay.py
```

### Result Summary

Live SQL load:

| Metric | Value |
| --- | ---: |
| Statements attempted | 203 |
| Statements succeeded | 203 |
| Statements failed | 0 |

Live row-count validation:

| Table | Rows |
| --- | ---: |
| `action_outcome_episodes` | 32 |
| `action_outcome_episode_metrics` | 101 |
| `action_outcome_retrieval_quality_labels` | 26 |
| `action_outcome_replay_recommendations` | 18 |
| `action_outcome_gate_suppressions` | 21 |

Semantic replay status:

- Expected top-action rows: 6.
- Live top-action rows: 0.
- Diagnostic projection returned rows, but non-tick columns were materialized as
  zero values.

### Interpretation

Experiment 007 proves that the generated Action-Outcome SQL seed is accepted by
the live ZeptoDB SQL endpoint and that row counts match the local SQL control.
It also shows that value-level replay is blocked by the current SQL INSERT path,
which maps rows into `TickMessage` fields (`symbol`, `price`, `volume`,
`timestamp`) instead of materializing arbitrary declared table columns.

### Next Best Step

Add a failing C++ regression test for arbitrary CREATE TABLE + INSERT + SELECT
projection, then decide whether to implement generic table INSERT support or a
tick-shaped Action-Outcome projection.

## Session 010 - Generic INSERT Projection Red Regression

Date: 2026-06-18

### Request

Add the failing C++ regression test for the Experiment 007 blocker.

### Completed Work

1. Added `ActionOutcomeSQLReplay.GenericInsertProjectionMaterializesDeclaredColumns`
   to `tests/unit/test_sql.cpp`.
   - Creates `action_outcome_projection` with `STRING` and `INT64` declared
     columns.
   - Verifies the empty table returns `count(*) = 0`.
   - Inserts one replay recommendation row.
   - Verifies `count(*) = 1`.
   - Expects `SELECT score_micros FROM action_outcome_projection WHERE
     top_action = 1` to return one row with `670031`.

2. Added `docs/devlog/185_action_outcome_generic_insert_regression.md`.

3. Updated backlog and Experiment 007 documentation so the next task is generic
   SQL INSERT materialization, not discovery.

### Verification Commands

```bash
cd build && ninja -j$(nproc) zepto_tests

cd build && ./tests/zepto_tests \
  --gtest_filter='ActionOutcomeSQLReplay.GenericInsertProjectionMaterializesDeclaredColumns'
```

### Result Summary

- Build result: pass.
- Filtered regression result: expected failure.
- Failure:

```text
Expected equality of these values:
  replay.rows.size()
    Which is: 0
  1u
    Which is: 1
```

### Interpretation

The red test captures the precise product gap found by Experiment 007. ZeptoDB
accepts the arbitrary schema INSERT and counts the row, but `top_action` is not
materialized as a queryable column, so `WHERE top_action = 1` returns zero rows.

### Next Best Step

Implement generic table INSERT support for declared schema columns. The first
passing milestone is the new C++ regression returning one row with
`score_micros = 670031`; the next milestone is rerunning Experiment 007 with
live top-action rows matching the SQL control.

## Session 011 - Generic INSERT Materialization Implementation

Date: 2026-06-18

### Request

Start implementing generic table INSERT materialization after the failing C++
regression.

### Completed Work

1. Implemented schema-aware SQL `INSERT ... VALUES` materialization.
   - Tables registered in `SchemaRegistry` now materialize declared columns via
     `TypedRowMessage`.
   - Legacy tables without `CREATE TABLE` keep the tick-shaped
     `symbol, price, volume, timestamp` path.
   - `STRING`, `SYMBOL`, integer, float, bool, and `TIMESTAMP_NS` columns are
     converted according to declared schema types.
   - Symbol-less Action-Outcome rows route to symbol `0`; timestamp routing uses
     `timestamp`, `timestamp_ns`, the first `TIMESTAMP_NS` column, or current
     time.
   - New typed-row partitions are registered in the pipeline partition index so
     `total_stored_rows()` and existing table-scoped accounting stay consistent.

2. Added cluster typed-row routing support.
   - `ClusterNodeBase` now exposes `ingest_typed_row()`.
   - `ClusterNode` routes typed rows through owner/migration logic and remote
     TCP RPC `TYPED_ROW_INGEST`.

3. Expanded C++ regression coverage.
   - The Action-Outcome regression now passes.
   - Added schema-order `STRING`/`FLOAT64`/`BOOL` materialization coverage.
   - Added `INT32` overflow rejection coverage with no partial insert.

4. Updated the live replay validator and report.
   - `docs/research/tools/action_outcome_live_sql_replay.py` now returns
     non-zero if row-count or semantic replay checks fail.
   - `docs/research/results/action_outcome_live_sql_replay_007.md` now records
     a full pass.

### Verification Commands

```bash
cd build && ninja -j$(nproc) zepto_tests

cd build && ninja -j$(nproc) zepto_http_server

cd build && ./tests/zepto_tests \
  --gtest_filter='ActionOutcomeSQLReplay.*:GenericInsertSQL.*:CatalogSQL.ShowTablesRowCountAfterInsert:SqlExecutorTest.SpatialDistanceAndWithinPredicates'

cd build && ./tests/zepto_tests \
  --gtest_brief=1 \
  --gtest_filter='*SQL*:*Sql*:*DDL*:*DML*:*TableScoped*:*Catalog*'

python3 docs/research/tools/action_outcome_live_sql_replay.py \
  --url http://127.0.0.1:19023/ \
  --output docs/research/results/action_outcome_live_sql_replay_007.md
```

### Result Summary

Live Experiment 007 now passes:

| Metric | Value |
| --- | ---: |
| Statements attempted | 203 |
| Statements succeeded | 203 |
| Statements failed | 0 |
| Top-action rows | 6 |
| Failed-action avoidance rows | 6 |

### Interpretation

The Action-Outcome SQL-backed replay contract is now validated against a live
ZeptoDB endpoint, not just a local SQL control. Generic declared-schema INSERT
is the right product path because Action-Outcome Memory needs direct query
access to categorical action, outcome, policy, and evidence fields.

### Next Best Step

Add a distributed two-node Action-Outcome live replay run to exercise typed-row
cluster routing for symbol-less generic tables. After that, extend the
acceptance harness with JOIN/window queries over recommendations and episode
metadata.

## Session 012 - Distributed Two-Node Live Replay

Date: 2026-06-18

### Request

Start distributed two-node Action-Outcome live replay.

### Completed Work

1. Added distributed typed-row routing to the HTTP server cluster path.
   - `CoordinatorRoutingAdapter` now overrides `ingest_typed_row()`.
   - Non-HA `zepto_http_server` peer RPC now registers a
     `TYPED_ROW_INGEST` callback.

2. Fixed distributed string result preservation.
   - `QueryResultSet` RPC serialization now carries an optional decoded string
     tail for `SYMBOL` columns; Session 013 extends this boundary to
     `STRING` and typed-row string payloads.
   - Distributed concat merge preserves remote string tails and reconstructs
     local dictionary-backed strings.

3. Added C++ regression tests.
   - `CoordinatorRoutingAdapter.RoutesTypedRowToLocalWhenOwnerIsSelf`
   - `CoordinatorRoutingAdapter.RoutesTypedRowToRemoteWhenOwnerIsDifferent`
   - `CoordinatorRoutingAdapter.DropsTypedRowOnUnknownOwner`
   - `RpcProtocol.RoundTripPreservesSymbolStrings`
   - `PartialAgg.MergeConcatPreservesSymbolStrings`

4. Added distributed live replay tooling and reports.
   - `docs/research/tools/action_outcome_distributed_live_sql_replay.py`
   - `docs/research/action_outcome_distributed_live_sql_replay_experiment_008.md`
   - `docs/research/results/action_outcome_distributed_live_sql_replay_008.md`
   - `docs/devlog/187_action_outcome_distributed_live_replay.md`

### Verification Commands

```bash
cd build
ninja -j$(nproc) zepto_tests
ninja -j$(nproc) zepto_http_server

./tests/zepto_tests \
  --gtest_filter='RpcProtocol.RoundTripPreservesSymbolStrings:PartialAgg.MergeConcatPreservesSymbolStrings:CoordinatorRoutingAdapter.*Typed*'

python3 docs/research/tools/action_outcome_distributed_live_sql_replay.py \
  --coordinator-url http://127.0.0.1:19241/ \
  --node-a-id 1 \
  --node-b-id 8 \
  --node-a-stats-url http://127.0.0.1:19241/stats \
  --node-b-stats-url http://127.0.0.1:19242/stats \
  --sql-file docs/research/results/action_outcome_sql_replay_006.sql \
  --output docs/research/results/action_outcome_distributed_live_sql_replay_008.md
```

### Result Summary

Distributed Experiment 008 passed:

| Metric | Value |
| --- | ---: |
| Statements attempted | 203 |
| Statements succeeded | 203 |
| Statements failed | 0 |
| Node 1 rows ingested | 140 |
| Node 8 rows ingested | 58 |
| Semantic top-action rows | 6 |
| Failed-action avoidance rows | 6 |
| Remote decoded string SELECT | pass |

The first 1/2 node-id run loaded successfully but did not distribute rows
because all five symbol-less Action-Outcome tables routed to node 1 under
`(stable_table_id, symbol_id=0)`. The final 1/8 run intentionally splits the
table owners across both nodes:

- node 1: episode metrics, replay recommendations, gate suppressions;
- node 8: episodes, retrieval quality labels.

The validator also queries `action_outcome_episodes` through the coordinator
and confirms that remote node 8 returns decoded `episode_id` and `action_class`
strings instead of local dictionary codes.

### Interpretation

The Action-Outcome SQL-backed replay contract is now validated across a real
two-node HTTP/RPC topology. This proves the current time-series memory research
path can move beyond single-node replay into sharded storage while preserving
semantic query output.

### Next Best Step

Add a CI-sized C++ integration test that starts two HTTP/RPC nodes and asserts
schema-aware typed-row routed INSERT plus decoded `STRING` SELECT results. The
next research extension after that is JOIN/window replay over
`action_outcome_episodes` and `action_outcome_replay_recommendations`.

## Session 013 - Distributed Replay C++ Regression

Date: 2026-06-18

### Request

Continue the next task after distributed two-node Action-Outcome live replay.

### Completed Work

1. Added a CI-sized C++ distributed replay regression.
   - `DistributedInsert.CoordinatorAdapterRoutesTypedRowsOverRpcAndDecodesRemoteStrings`
     starts two in-memory pipelines and a real remote `TcpRpcServer`.
   - The test broadcasts Action-Outcome DDL through `QueryCoordinator`.
   - A local `QueryExecutor` uses `CoordinatorRoutingAdapter` to route a
     schema-aware `INSERT` through `TYPED_ROW_INGEST` TCP RPC.
   - The assertion checks that the row materializes only on the remote owner
     and that coordinator `SELECT` returns decoded `episode_id` and
     `action_class` strings.

2. Fixed the distributed string-code boundary found by the regression.
   - `TypedColumnValue` now carries optional original text for `STRING` and
     `SYMBOL` values.
   - `TYPED_ROW_INGEST` RPC serializes that string payload by column index.
   - Remote owners bind sender dictionary codes through `StringDictionary`.
   - `QueryResultSet` RPC, concat merge, and HTTP JSON output now treat both
     `STRING` and `SYMBOL` as decoded string columns.

3. Added protocol coverage.
   - `RpcProtocol.RoundTripPreservesTypedRowStringPayload` verifies non-empty
     and empty string payloads survive typed-row serialization.

4. Updated docs.
   - `docs/devlog/188_action_outcome_distributed_replay_regression.md`
   - `docs/design/phase_c_distributed.md`
   - `docs/api/CPP_REFERENCE.md`
   - `docs/COMPLETED.md`
   - `docs/BACKLOG.md`

### Verification Commands

```bash
cd build
ninja -j$(nproc) zepto_tests
ninja -j$(nproc) zepto_http_server

./tests/zepto_tests \
  --gtest_filter='RpcProtocol.RoundTripPreservesTypedRowStringPayload:DistributedInsert.CoordinatorAdapterRoutesTypedRowsOverRpcAndDecodesRemoteStrings'

./tests/zepto_tests \
  --gtest_filter='RpcProtocol.RoundTripPreservesSymbolStrings:RpcProtocol.RoundTripPreservesTypedRowStringPayload:PartialAgg.MergeConcatPreservesSymbolStrings:DistributedInsert.*'

./tests/zepto_tests
```

### Result Summary

| Metric | Value |
| --- | ---: |
| New focused tests | 2/2 pass |
| Related protocol/merge/distributed tests | 8/8 pass |
| Full local C++ suite | 1508 pass, 1 skipped |
| Disabled tests | 3 |

The C++ regression caught a real semantic gap that live Experiment 008 could
mask: coordinator-local dictionaries could decode remote `STRING` codes, but
remote owners did not have enough text payload to rebuild their own dictionary
state. The typed-row RPC payload now carries the original string value so the
owner storage node can preserve semantic text independently.

### Interpretation

Action-Outcome distributed SQL replay is now covered by both a live research
harness and a CI-sized C++ regression. This makes the research path much safer
to extend because future JOIN/window experiments can rely on generic
schema-aware rows preserving text semantics across owner shards.

### Next Best Step

Add JOIN/window replay acceptance over `action_outcome_episodes` and
`action_outcome_replay_recommendations`. The first useful target is a query
that joins top recommendations back to episode outcomes and computes whether
recommended actions avoided repeating failed historical actions within a
bounded time window.

## Session 014 - JOIN/Window Replay Acceptance

Date: 2026-06-18

### Request

Continue the next task after the distributed replay C++ regression.

### Completed Work

1. Added Experiment 009 live JOIN/window replay tooling.
   - `docs/research/tools/action_outcome_join_window_replay.py` loads the
     Experiment 006 SQL seed into a live ZeptoDB endpoint.
   - The tool parses the same seed into an in-memory SQLite control database.
   - It derives six query-level control rows and eighteen recommendation rows.
   - It creates numeric acceptance projection tables with explicit `symbol` and
     `timestamp_ns` columns for stable ZeptoDB partitioning.

2. Validated SQL window semantics on the native replay table.
   - The native query partitions by `query_id`.
   - `ROW_NUMBER()` matches `recommendation_rank` for all eighteen replay rows.

3. Validated JOIN/window semantics on the acceptance projection.
   - Numeric JOIN returns all eighteen recommendation rows joined to their
     query controls.
   - Top recommendations match expected action codes.
   - For query episodes with human failure labels, top recommendations do not
     repeat the observed failed action.
   - `LAG(score_num, 1, 0)` returns the previous recommendation score within
     each query sequence.

4. Recorded the current native string-key JOIN boundary.
   - Direct JOIN between `action_outcome_replay_recommendations.query_id` and
     `action_outcome_episodes.episode_id` returns zero-valued string cells
     under the current hash JOIN executor.
   - The report marks this as `blocked_current_engine`, not as a numeric
     acceptance failure.

5. Updated docs.
   - `docs/research/action_outcome_join_window_replay_experiment_009.md`
   - `docs/research/results/action_outcome_join_window_replay_009.md`
   - `docs/devlog/189_action_outcome_join_window_replay.md`
   - `docs/BACKLOG.md`
   - `docs/COMPLETED.md`

### Verification Commands

```bash
python3 -m py_compile docs/research/tools/action_outcome_join_window_replay.py

python3 docs/research/tools/action_outcome_join_window_replay.py \
  --url http://127.0.0.1:19341/ \
  --output docs/research/results/action_outcome_join_window_replay_009.md \
  --timeout 10
```

### Result Summary

| Metric | Value |
| --- | ---: |
| Seed statements attempted | 203 |
| Seed statements succeeded | 203 |
| Seed statements failed | 0 |
| Seed row-count verification | pass |
| Projection row-count verification | pass |
| Native string window | pass |
| Numeric JOIN/window acceptance | pass |
| Native string-key JOIN | blocked_current_engine |

### Interpretation

Action-Outcome replay now has a live SQL acceptance layer for JOIN and window
semantics. The useful research contract is already testable through a numeric
projection, while the remaining product gap is specific: native hash JOIN over
declared `STRING` keys needs typed string materialization.

### Next Best Step

Add a C++ failing regression for string-key hash JOIN on declared `STRING`
columns. Once it is red, implement string-key JOIN materialization so
Experiment 009 can validate the native Action-Outcome schema directly without
numeric projection tables.

## Session 015 - String-Key Hash JOIN Materialization

Date: 2026-06-18

### Request

Continue the next task after Experiment 009.

### Completed Work

1. Added a C++ regression for native string-key hash JOIN.
   - `ActionOutcomeSQLReplay.StringKeyHashJoinPreservesDecodedStringColumns`
     creates declared-schema `STRING` join-key tables.
   - The test inserts matching and non-matching Action-Outcome-style rows.
   - It asserts one matched JOIN row and decoded semantic strings for
     `query_id`, recommended action, and human outcome.

2. Confirmed the regression failed before the fix.
   - `column_types` came back as `INT64`.
   - Joined string result cells reused the join-key dictionary code rather than
     preserving the selected string columns.

3. Implemented hash JOIN string materialization.
   - JOIN key collection now reads through `ColumnVector` type-aware access.
   - Result column types are copied from the selected source side.
   - Joined row assembly reads cells through the same typed accessor.
   - The result carries the pipeline string dictionary for decoded output.

4. Re-ran Experiment 009.
   - Native string JOIN now passes with eighteen semantic joined rows.
   - Numeric projection remains for top-action/outcome-avoidance semantics
     until hash JOIN supports alias-aware `WHERE` predicates.

5. Updated docs.
   - `docs/devlog/190_string_key_hash_join.md`
   - `docs/research/action_outcome_join_window_replay_experiment_009.md`
   - `docs/research/results/action_outcome_join_window_replay_009.md`
   - `docs/api/SQL_REFERENCE.md`
   - `docs/BACKLOG.md`
   - `docs/COMPLETED.md`

### Verification Commands

```bash
ninja -C build -j$(nproc) zepto_tests
ninja -C build -j$(nproc) zepto_http_server

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSQLReplay.StringKeyHashJoinPreservesDecodedStringColumns'

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSQLReplay.*:GenericInsertSQL.*:HashJoin*:*Join_Executes:*Join_ConcatenatesRows:*Join_HasColumns'

python3 -m py_compile docs/research/tools/action_outcome_join_window_replay.py

python3 docs/research/tools/action_outcome_join_window_replay.py \
  --url http://127.0.0.1:19342/ \
  --output docs/research/results/action_outcome_join_window_replay_009.md \
  --timeout 10
```

### Result Summary

| Metric | Value |
| --- | ---: |
| New string-key JOIN regression | pass |
| Related JOIN/generic-insert tests | 24/24 pass |
| Experiment 009 seed statements | 203/203 pass |
| Experiment 009 native string JOIN | pass |
| Experiment 009 native string window | pass |
| Experiment 009 numeric JOIN/window | pass |

### Interpretation

Native Action-Outcome schema joins now work for declared `STRING` keys and
joined string result columns. This removes the biggest single-node blocker from
Experiment 009. The next remaining executor gap is not string materialization;
it is applying alias-aware `WHERE` predicates in hash JOIN queries.

### Next Best Step

Add alias-aware `WHERE` predicate handling for hash JOIN queries, then port
Experiment 009 into the two-node live harness to record distributed JOIN/window
planner limits separately.

## Session 016 - AIOps Time-Series Industry Scan

Date: 2026-06-18

### Request

Research how Datadog-like AIOps products use time-series data and identify the
unique innovation point for ZeptoDB.

### Completed Work

1. Reviewed public vendor materials and one AIOps time-series survey.
   - Datadog Watchdog and anomaly monitors.
   - Datadog Bits AI SRE and Bits Investigation.
   - Dynatrace Davis / Dynatrace Intelligence causal RCA.
   - New Relic anomaly detection.
   - Splunk ITSI KPI anomaly detection.
   - Grafana Cloud metric forecasting.
   - Elastic AIOps anomaly, log pattern, forecast, and correlation features.
   - arXiv AIOps time-series anomaly detection survey.

2. Identified the common industry pattern.
   - Current platforms use time-series mostly for anomaly detection,
     forecasting, alert correlation, RCA, change impact, and investigation
     traceability.
   - Public product positioning is strongest around "detect/explain/triage."

3. Identified ZeptoDB's sharper innovation point.
   - Do not position against Datadog/Dynatrace as another anomaly detector.
   - Position Action-Outcome Memory as the next loop after detection:
     storing pre-action state, action, post-action observation window, outcome,
     and future policy guardrail.
   - The core claim is state-action-outcome memory for operations, not
     signal-only AIOps.

4. Recorded a dedicated research note.
   - `docs/research/aiops_timeseries_industry_scan_2026.md`

### Result Summary

| Finding | Interpretation |
| --- | --- |
| Vendors already use time-series for anomaly bands, seasonality, forecasting, KPI anomalies, log-rate anomalies, and RCA correlation. | Competing head-on as "better anomaly detection" is weak. |
| Datadog Bits AI SRE and Dynatrace Intelligence are moving toward agentic investigation. | The market is validating AI operations agents, but also raising the differentiation bar. |
| Public materials emphasize investigation and root-cause workflows more than user-owned, replayable action-outcome memory. | ZeptoDB can own the memory/evaluation layer beneath action recommendation. |
| Experiment 005 already shows context gating can improve failed-action avoidance from 0.67 to 1.00 on the noisy fixture. | The strongest research story is safe reuse of prior action outcomes. |

### Interpretation

The commercial wedge should be "shadow-mode Action-Outcome Memory for SRE
teams": recommend or suppress remediation actions based on prior outcome
windows, while preserving SQL replay and human approval. This complements
Datadog-like observability stacks instead of trying to replace them.

### Next Best Step

Convert this industry scan into a vendor-inspired benchmark plan with four
baselines: anomaly-only, RCA-only, retrieval-only memory, and context-gated
Action-Outcome Memory.

## Session 017 - Action-Outcome Industry Research Scan

Date: 2026-06-18

### Request

Identify industry and research work adjacent to Action-Outcome Memory.

### Completed Work

1. Reviewed adjacent industry categories.
   - AIOps incident action recommendation.
   - Next-best-action recommendation.
   - Closed-loop remediation.
   - Runbook automation.
   - Autonomous AIOps agent benchmarks.

2. Reviewed adjacent agent-learning research.
   - Reflexion: verbal reinforcement and episodic reflection memory.
   - ExpeL: self-gathered success/failure experiences and extracted insights.
   - Voyager: skill library updated from environment feedback and execution
     errors.
   - RL-based autoscaling as a narrower state/action/reward operations use case.

3. Identified the defensible ZeptoDB gap.
   - The industry already validates action recommendation and remediation
     automation.
   - The less-common piece is a user-owned, SQL/time-series-native
     action-outcome memory substrate with replayable evaluation and
     context-conditioned suppression.

4. Recorded a dedicated research note.
   - `docs/research/action_outcome_industry_research_scan_2026.md`

### Result Summary

| Cluster | Relationship to ZeptoDB |
| --- | --- |
| IBM/BMC/PagerDuty/incident.io action and runbook recommendation | Commercially validates moving from insight to action. |
| Dynatrace closed-loop remediation | Validates post-action observation as part of remediation. |
| Microsoft AIOpsLab | Validates need for realistic AIOps agent benchmarks. |
| Reflexion/ExpeL/Voyager | Validates outcome-driven agent memory, but not operational time-series schemas. |
| RL autoscaling | Gives formal state/action/reward grounding, but is narrower and safer than incident remediation. |

### Interpretation

Action-Outcome Memory is not a completely empty field. The opportunity is to
make it explicit, structured, queryable, and replayable for operational agents.
ZeptoDB should avoid claiming novelty on "action recommendation" alone and
instead claim a time-series action episode substrate with context gates and
outcome replay.

### Next Best Step

Build a vendor-inspired baseline experiment comparing similar-incident
retrieval, runbook/action-prior recommendation, reflection-only memory, and
context-gated Action-Outcome Memory on the same noisy AIOps fixture.

## Session 018 - Vendor Baseline Experiment 010

Date: 2026-06-18

### Request

Implement and document Experiment 010 comparing similar-incident retrieval,
runbook/action-prior recommendation, reflection-only memory, and context-gated
Action-Outcome Memory on the existing noisy AIOps fixture.

### Completed Work

1. Added a vendor-inspired baseline harness.
   - `docs/research/tools/action_outcome_vendor_baseline.py`
   - Reuses the existing base fixture, distractor fixture, and retrieval
     quality labels.
   - Implements four variants:
     - `similar_incident`
     - `runbook_action_prior`
     - `reflection_only_memory`
     - `context_gated_action_outcome`

2. Generated the Experiment 010 result report.
   - `docs/research/results/action_outcome_vendor_baseline_010.md`
   - Captures Top-3 hit rate, failed-action avoidance, useful/superficial/
     misleading top-3 evidence counts, top-action changes versus the context
     gate, and suppression count.

3. Documented the experiment.
   - `docs/research/action_outcome_vendor_baseline_experiment_010.md`
   - `docs/devlog/191_action_outcome_vendor_baseline.md`
   - Updated backlog and completed-feature docs.

### Verification Commands

```bash
python3 -m py_compile docs/research/tools/action_outcome_vendor_baseline.py

python3 docs/research/tools/action_outcome_vendor_baseline.py \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_vendor_baseline_010.md
```

### Result Summary

| Variant | Top-3 Hit Rate | Failed-Action Avoidance | Misleading Top3 | Suppressions |
| --- | ---: | ---: | ---: | ---: |
| `similar_incident` | 1.00 | 0.67 | 6 | 0 |
| `runbook_action_prior` | 1.00 | 0.67 | 5 | 0 |
| `reflection_only_memory` | 1.00 | 0.83 | 6 | 0 |
| `context_gated_action_outcome` | 1.00 | 1.00 | 6 | 21 |

### Interpretation

Experiment 010 gives the strongest product-positioning evidence so far. The
context gate does not merely find similar incidents, reuse a runbook prior, or
remember reflections. It decides whether a past outcome is safe to reuse under
the current temporal, topology, change, and metric context.

The result is especially useful because the misleading top-3 count stays high:
the gate is not hiding the difficult evidence. It is preventing mismatched
outcomes from dominating the final action ranking.

### Next Best Step

Replay Experiment 010 through SQL tables after alias-aware hash JOIN `WHERE`
predicates are implemented, then use native JOIN/window queries to audit action
comparison, outcome joins, and suppression rows.

## Session 019 - Vendor SQL/JOIN/Window Replay

Date: 2026-06-18

### Request

Complete ZeptoDB SQL/JOIN/window replay verification for the Action-Outcome
vendor baseline comparison, including any required intermediate work.

### Completed Work

1. Closed the remaining native hash JOIN filtering gap.
   - Added alias-aware post-join `WHERE` predicate handling to the hash JOIN
     executor.
   - The evaluator resolves columns against the left and right table aliases.
   - It supports comparison operators, `BETWEEN`, `IN`, `IS NULL`, `LIKE`,
     `AND`, `OR`, and `NOT`.
   - Declared `STRING` and `SYMBOL` cells are decoded before string
     comparisons and `LIKE` evaluation.

2. Strengthened the C++ Action-Outcome JOIN regression.
   - `ActionOutcomeSQLReplay.StringKeyHashJoinPreservesDecodedStringColumns`
     now includes a matching row that must be filtered out by
     `r.top_action = 1`.
   - The query also filters the right side with
     `e.human_outcome = 'failure'`.

3. Added the live Experiment 010 SQL replay harness.
   - `docs/research/tools/action_outcome_vendor_sql_replay.py`
   - Loads the existing SQL seed into ZeptoDB HTTP.
   - Materializes Experiment 010 query, recommendation, retrieval, and
     suppression tables.
   - Validates failed-repeat JOIN, context top-action JOIN, suppression JOIN,
     misleading retrieval JOIN, ROW_NUMBER, and LAG checks.

4. Generated the live replay report.
   - `docs/research/results/action_outcome_vendor_sql_replay_010.md`
   - The replay loads 203/203 seed statements and materializes 6 query rows,
     72 recommendation rows, 72 retrieval rows, and 21 suppression rows.

5. Documented the experiment.
   - `docs/research/action_outcome_vendor_sql_replay_experiment_010.md`
   - `docs/devlog/192_action_outcome_vendor_sql_replay.md`
   - Updated backlog, completed-feature docs, and SQL reference.

### Verification Commands

```bash
python3 -m py_compile docs/research/tools/action_outcome_vendor_sql_replay.py

ninja -C build -j$(nproc) zepto_tests zepto_http_server

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSQLReplay.*:GenericInsertSQL.*:HashJoin*:*Join_Executes:*Join_ConcatenatesRows:*Join_HasColumns'

build/zepto_http_server --port 19343 --ticks 0 --no-auth --log-level error

python3 docs/research/tools/action_outcome_vendor_sql_replay.py \
  --url http://127.0.0.1:19343/ \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_vendor_sql_replay_010.md \
  --timeout 10
```

### Result Summary

| Check | Status |
| --- | --- |
| Seed statements | 203/203 succeeded |
| Seed row counts | pass |
| Vendor table counts | pass |
| Failed-repeat JOIN | pass |
| Context top-action JOIN | pass |
| Suppression JOIN | pass |
| Misleading retrieval JOIN | pass |
| ROW_NUMBER window replay | pass |
| LAG window replay | pass |
| Focused SQL/JOIN regression suite | 24/24 pass |

### Interpretation

Experiment 010 is now database-auditable. The core claim is no longer only a
Python fixture result: ZeptoDB can store the recommendation candidates,
retrieval evidence, suppressions, and query controls, then use native SQL to
show that context-gated Action-Outcome Memory avoids the historical failed
action on all six noisy AIOps queries.

### Next Best Step

Port this SQL/JOIN/window replay into the two-node live harness and record
which checks are already distributed-safe versus which still require
distributed JOIN/window planner work.

## Session 020 - Distributed Vendor SQL Replay Classification

Date: 2026-06-19

### Request

Continue with the next Action-Outcome research task after single-node
Experiment 010 SQL/JOIN/window replay.

### Completed Work

1. Added Experiment 011 distributed replay tooling.
   - `docs/research/tools/action_outcome_distributed_vendor_sql_replay.py`
   - Reuses Experiment 010 SQL materialization and validation.
   - Reuses Experiment 008 two-node owner routing and node-local stats deltas.
   - Classifies each JOIN/window check rather than collapsing all failures into
     one generic distributed SQL failure.

2. Ran the two-node live replay against node 1 / node 8.
   - Coordinator endpoint: `http://127.0.0.1:19241/`
   - Remote node endpoint: `http://127.0.0.1:19242/`
   - Result file:
     `docs/research/results/action_outcome_distributed_vendor_sql_replay_011.md`

3. Documented Experiment 011.
   - `docs/research/action_outcome_distributed_vendor_sql_replay_experiment_011.md`
   - `docs/devlog/193_action_outcome_distributed_vendor_sql_replay.md`
   - Updated backlog, completed-feature docs, and distributed design notes.

### Verification Commands

```bash
python3 -m py_compile \
  docs/research/tools/action_outcome_distributed_vendor_sql_replay.py \
  docs/research/tools/action_outcome_vendor_sql_replay.py \
  docs/research/tools/action_outcome_distributed_live_sql_replay.py

ninja -C build -j$(nproc) zepto_http_server

python3 docs/research/tools/action_outcome_distributed_vendor_sql_replay.py \
  --coordinator-url http://127.0.0.1:19241/ \
  --node-a-id 1 \
  --node-b-id 8 \
  --node-a-stats-url http://127.0.0.1:19241/stats \
  --node-b-stats-url http://127.0.0.1:19242/stats \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_distributed_vendor_sql_replay_011.md \
  --timeout 10

git diff --check
```

### Result Summary

| Check | Status |
| --- | --- |
| Seed statements | 203/203 succeeded |
| Seed row counts | pass |
| Vendor table counts | pass |
| Distributed ingest | pass |
| Failed-repeat JOIN | pass |
| Context top-action JOIN | pass |
| Misleading retrieval JOIN | pass |
| Suppression JOIN | expected_gap_cross_node_join |
| ROW_NUMBER | expected_gap_cluster_window_values |
| LAG | expected_gap_cluster_window_values |

### Interpretation

Experiment 011 narrows the distributed SQL roadmap. The Action-Outcome vendor
tables can be loaded and counted correctly through the two-node topology, and
co-located vendor JOINs pass. The remaining gaps are more specific than
"distributed SQL is incomplete": cross-node hash JOIN needs a small-table
broadcast/replication strategy, and cluster-mode window functions over declared
operational tables need value materialization repair.

### Next Best Step

Fix cluster-mode window value materialization for declared operational tables.
After ROW_NUMBER/LAG pass in Experiment 011, implement small-table distributed
hash JOIN for the suppression table.

---

## Session 022 - Cluster-Mode Window Typed Materialization

Date: 2026-06-20

### Request

Fix cluster-mode window value materialization.

### Completed Work

1. Added a C++ distributed regression for declared operational-table windows.
   - `DistributedInsert.ClusterWindowMaterializesGenericTableValues`
   - Routes Action-Outcome-style typed rows to a remote owner through TCP/RPC.
   - Verifies cluster-mode `ROW_NUMBER` and `LAG` preserve `group_id`,
     `recommendation_rank`, and `score_micros` instead of returning zeroed
     value columns.

2. Fixed coordinator full-data window replay materialization.
   - `QueryCoordinator` now replays fetched full-data rows into the temporary
     local pipeline with schema-aware `TypedRowMessage` rows.
   - Local schema snapshots are reused when available; otherwise the temporary
     schema is inferred from merged `SELECT *` result metadata.
   - Declared generic integer, timestamp, float, bool, `STRING`, and `SYMBOL`
     cells are preserved before the original window query executes locally.

3. Re-ran Experiment 011 and updated the generated report.
   - `docs/research/results/action_outcome_distributed_vendor_sql_replay_011.md`
   - `row_number_window`: pass, 72/72 rows.
   - `lag_window`: pass, 72/72 rows.
   - Remaining expected gap: `suppression_join` is still
     `expected_gap_cross_node_join`.

4. Updated docs and backlog.
   - `docs/devlog/194_cluster_window_typed_materialization.md`
   - `docs/BACKLOG.md`
   - `docs/COMPLETED.md`
   - `docs/design/phase_c_distributed.md`
   - `docs/api/SQL_REFERENCE.md`
   - `docs/research/action_outcome_distributed_vendor_sql_replay_experiment_011.md`

### Verification Commands

```bash
ninja -C build -j$(nproc) zepto_tests

./build/tests/zepto_tests \
  --gtest_filter='QueryCoordinator.TwoNodeRemote_DistributedWindowFunction:QueryCoordinator.TwoNodeRemote_DistributedFirstLast:QueryCoordinator.TwoNodeRemote_DistributedDistinct:DistributedInsert.ClusterWindowMaterializesGenericTableValues'

ninja -C build -j$(nproc) zepto_http_server

python3 docs/research/tools/action_outcome_distributed_vendor_sql_replay.py \
  --coordinator-url http://127.0.0.1:19241/ \
  --node-a-id 1 \
  --node-b-id 8 \
  --node-a-stats-url http://127.0.0.1:19241/stats \
  --node-b-stats-url http://127.0.0.1:19242/stats \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_distributed_vendor_sql_replay_011.md \
  --timeout 10
```

### Result Summary

| Check | Status |
| --- | --- |
| Focused C++ regression set | 4/4 pass |
| Seed statements | 203/203 succeeded |
| Seed row counts | pass |
| Vendor table counts | pass |
| Distributed ingest | pass |
| Failed-repeat JOIN | pass |
| Context top-action JOIN | pass |
| Misleading retrieval JOIN | pass |
| ROW_NUMBER | pass |
| LAG | pass |
| Suppression JOIN | expected_gap_cross_node_join |

### Interpretation

The distributed window gap was not a window-function correctness problem. It
was a temporary-table replay bug: the coordinator reduced declared generic
rows to legacy tick-shaped fields before local window evaluation. Schema-aware
typed replay closes that gap and leaves Experiment 011 with a single planner
boundary: cross-node hash JOIN for small operational tables.

### Next Best Step

Implement small-table distributed hash JOIN for operational tables. Start with
broadcast/replicated dimension-table joins so
`action_outcome_vendor_suppressions_010` on node 1 can join
`action_outcome_vendor_recommendations_010` on node 8.

---

## 2026-06-21 — Small-Table Distributed Hash JOIN

### Request

Implement a narrow small-table broadcast hash JOIN path for distributed
operational tables, so Experiment 011 `suppression_join` changes from
`expected_gap_cross_node_join` to `pass`.

### Completed Work

1. Added a failing C++ regression for the Experiment 011 boundary.
   - `DistributedInsert.SmallTableBroadcastJoinMaterializesCrossNodeOperationalTables`
   - Uses `action_outcome_vendor_suppressions_010` and
     `action_outcome_vendor_recommendations_010`.
   - Asserts suppressions route to node 1 and recommendations route to node 8
     under the 1/8 ring.
   - Verifies a coordinator JOIN returns the expected suppression row and
     decoded string columns.

2. Implemented bounded coordinator-local hash JOIN for small operational
   tables.
   - Candidate shape: declared-table `INNER`, `LEFT`, `RIGHT`, or `FULL`
     equi hash JOIN.
   - Fetches both base tables with `SELECT * ... LIMIT row_cap + 1`.
   - Rejects tables beyond the small-table row cap.
   - Materializes both sides into a temporary typed `ZeptoPipeline`.
   - Executes the original JOIN SQL locally so existing hash JOIN semantics,
     alias-aware `WHERE`, and `ORDER BY` logic are reused.

3. Preserved string semantics across the temp pipeline boundary.
   - Local scatter results can carry a live `symbol_dict` without populated
     `string_rows`; remote RPC results usually carry decoded `string_rows`.
   - The coordinator now decodes local `STRING`/`SYMBOL` cells before typed
     materialization and snapshots decoded temp-pipeline result strings before
     returning.

4. Re-ran Experiment 011 in strict full SQL mode.
   - `suppression_join`: pass, 21/21 rows.
   - `row_number_window`: pass, 72/72 rows.
   - `lag_window`: pass, 72/72 rows.
   - Full distributed SQL/JOIN/window status: pass.

5. Updated docs and backlog.
   - `docs/devlog/195_small_table_distributed_hash_join.md`
   - `docs/BACKLOG.md`
   - `docs/COMPLETED.md`
   - `docs/design/phase_c_distributed.md`
   - `docs/api/SQL_REFERENCE.md`
   - `docs/research/action_outcome_distributed_vendor_sql_replay_experiment_011.md`
   - `docs/research/results/action_outcome_distributed_vendor_sql_replay_011.md`

### Verification Commands

```bash
ninja -C build -j$(nproc) zepto_tests

./build/tests/zepto_tests \
  --gtest_filter='QueryCoordinator.TwoNodeRemote_DistributedWindowFunction:QueryCoordinator.TwoNodeRemote_DistributedFirstLast:QueryCoordinator.TwoNodeRemote_DistributedDistinct:QueryCoordinator.TwoNodeRemote_DistributedAvg_Correct:QueryCoordinator.TwoNodeRemote_DistributedAvg_MixedAggs:QueryCoordinator.TwoNodeRemote_GroupBy_CrossNode_XbarMerge:QueryCoordinator.TwoNodeRemote_DistributedVwap:QueryCoordinator.TwoNodeRemote_OrderByLimit:QueryCoordinator.TwoNodeRemote_DistributedHaving:QueryCoordinator.TwoNodeRemote_MultiColumnOrderBy:DistributedInsert.ClusterWindowMaterializesGenericTableValues:DistributedInsert.SmallTableBroadcastJoinMaterializesCrossNodeOperationalTables:DistributedInsert.SmallTableBroadcastJoinRejectsRowsOverLimit'

ninja -C build -j$(nproc) zepto_http_server

python3 docs/research/tools/action_outcome_distributed_vendor_sql_replay.py \
  --coordinator-url http://127.0.0.1:19241/ \
  --node-a-id 1 \
  --node-b-id 8 \
  --node-a-stats-url http://127.0.0.1:19241/stats \
  --node-b-stats-url http://127.0.0.1:19242/stats \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_distributed_vendor_sql_replay_011.md \
  --timeout 10 \
  --strict-full-sql
```

### Result Summary

| Check | Status |
| --- | --- |
| Focused C++ regression set | 13/13 pass |
| Seed statements | 203/203 succeeded |
| Seed row counts | pass |
| Vendor table counts | pass |
| Distributed ingest | pass |
| Failed-repeat JOIN | pass |
| Context top-action JOIN | pass |
| Suppression JOIN | pass |
| Misleading retrieval JOIN | pass |
| ROW_NUMBER | pass |
| LAG | pass |
| Strict full distributed SQL/JOIN/window | pass |

### Interpretation

This closes Experiment 011's last research harness boundary. The result is
commercially meaningful for Action-Outcome Memory because operational control
tables are naturally small: recommendations, suppressions, runbook priors, and
query controls can be joined across owners without waiting for a full
distributed SQL optimizer.

The path is deliberately bounded. It should not be treated as the final answer
for large cross-node hash JOINs over high-cardinality time-series fact tables.

### Next Best Step

Define symbol-less operational table shard-key policy and add row-cap
observability for the small-table JOIN path. This will make placement explicit
instead of depending on stable table id plus `symbol_id=0`.

---

## 2026-06-21 — Operational Placement Policy And Telemetry

### Request

Implement Experiment 012: explicit operational table placement policy and
telemetry for bounded small-table Action-Outcome JOINs.

### Completed Work

1. Added explicit table placement policy to the cluster router.
   - `HashByTableAndSymbol`: default table+symbol route key.
   - `HashByTable`: all rows for a table use the same table-level route key.
   - `PinnedNode`: a declared table is routed to a specified node id.
   - The policy is keyed by declared table id and applies to
     `route(table_id, symbol)`.

2. Added coordinator and HTTP control-plane APIs.
   - `QueryCoordinator::set_table_placement()`
   - `QueryCoordinator::clear_table_placement()`
   - `POST /admin/table-placement`
   - Declared symbol-less coordinator INSERTs now route by `(table_id, 0)`, so
     runtime policy applies even when the coordinator is used directly.

3. Added bounded small-table JOIN telemetry.
   - `/stats` includes `small_table_join`.
   - `/metrics` exposes Prometheus counters/gauges:
     - `zepto_small_table_join_candidates_total`
     - `zepto_small_table_join_accepted_total`
     - `zepto_small_table_join_row_cap_rejections_total`
     - `zepto_small_table_join_errors_total`
     - `zepto_small_table_join_rows_materialized_total`
     - `zepto_small_table_join_last_left_rows`
     - `zepto_small_table_join_last_right_rows`

4. Added/extended C++ regressions.
   - `DistributedInsert.OperationalTablePlacementPolicyPinsSymbollessTable`
   - `DistributedInsert.SmallTableBroadcastJoinMaterializesCrossNodeOperationalTables`
   - `DistributedInsert.SmallTableBroadcastJoinRejectsRowsOverLimit`

5. Added Experiment 012 live replay tooling and report.
   - Tool:
     `docs/research/tools/action_outcome_operational_placement_experiment.py`
   - Procedure:
     `docs/research/action_outcome_operational_placement_experiment_012.md`
   - Result:
     `docs/research/results/action_outcome_operational_placement_012.md`

### Verification Commands

```bash
ninja -C build -j$(nproc) zepto_tests zepto_http_server

./build/tests/zepto_tests \
  --gtest_filter='DistributedInsert.OperationalTablePlacementPolicyPinsSymbollessTable:DistributedInsert.SmallTableBroadcastJoinMaterializesCrossNodeOperationalTables:DistributedInsert.SmallTableBroadcastJoinRejectsRowsOverLimit'

python3 -m py_compile \
  docs/research/tools/action_outcome_vendor_sql_replay.py \
  docs/research/tools/action_outcome_operational_placement_experiment.py \
  docs/research/tools/action_outcome_distributed_vendor_sql_replay.py

python3 docs/research/tools/action_outcome_operational_placement_experiment.py \
  --coordinator-url http://127.0.0.1:19241/ \
  --node-a-id 1 \
  --node-b-id 8 \
  --node-a-stats-url http://127.0.0.1:19241/stats \
  --node-b-stats-url http://127.0.0.1:19242/stats \
  --metrics-url http://127.0.0.1:19241/metrics \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_operational_placement_012.md \
  --timeout 10
```

### Result Summary

| Check | Status |
| --- | --- |
| Focused C++ regression set | 3/3 pass |
| Explicit placement policy | pass |
| Seed row counts | pass |
| Vendor table counts | pass |
| Distributed ingest | pass |
| Full SQL/JOIN/window replay | pass |
| Small-table JOIN telemetry | pass |
| Prometheus telemetry | pass |
| Experiment 012 overall | pass |

Experiment 012 pinned query, recommendation, and retrieval vendor tables to
node 8, while pinning suppressions to node 1. The suppression JOIN therefore
remained cross-node by design and still passed through the bounded small-table
JOIN path. Telemetry recorded 4 JOIN candidates, 4 accepted JOINs, 0 row-cap
rejections, 0 errors, and 327 materialized rows.

### Interpretation

This closes the immediate research question from Experiment 011: Action-Outcome
operational table placement no longer has to depend on stable table-id hashing.
The commercial wedge is clearer now: ZeptoDB can offer explicit placement and
operator-visible bounded JOIN telemetry for agentic operational memory, without
claiming a full distributed SQL optimizer.

### Next Best Step

Promote runtime table placement overrides to a persisted table option or catalog
record, then add an operations/alerting example for row-cap rejections.

---

## 2026-06-21 — Research Experiment Governance Policy

### Why

The Action-Outcome research branch intentionally produced useful runtime
changes for distributed SQL replay. After Experiment 012, the main risk was
documentation drift: a validated experimental runtime path could look like a
fully promoted product feature.

### Changes

1. Added `docs/research/EXPERIMENT_GOVERNANCE.md`.
   - Defines `Research-only`, `Experimental runtime path`, and
     `Promoted product feature`.
   - Requires experiment docs to record classification, status, procedure,
     acceptance criteria, results, interpretation, and next step.
   - Defines product-promotion gates for runtime/API changes.

2. Linked the policy from `AGENTS.md`.
   - Future Codex work must apply the policy before documenting or promoting
     experiment-driven runtime behavior.

3. Reclassified current Action-Outcome runtime changes.
   - Generic SQL INSERT materialization: promoted product correctness feature.
   - String-key hash JOIN and alias-aware predicates: promoted product
     correctness feature.
   - Cluster full-data window materialization: experimental runtime path.
   - Bounded small-table distributed hash JOIN: experimental runtime path.
   - Runtime operational table placement: experimental runtime path.

4. Updated Experiment 012 docs and API references.
   - Runtime placement is now described as admin-only experimental state.
   - Bounded small-table JOIN telemetry is explicitly scoped to the narrow
     coordinator-local path.
   - Product promotion requires persisted placement, explicit limits, and
     operator guidance.

### Verification

Documentation-only governance change. No code build was required.

---

## 2026-06-23 — Experiment 013 Physical AI Action-Outcome Baseline

### Goal

Compare similar robot incident retrieval, runbook/action-prior recommendation,
reflection-only memory, and context-gated Physical AI Action-Outcome Memory.
The research question is whether a robot/fleet agent can avoid repeating unsafe
actions and select the right recovery action when past successes only worked in
different safety contexts.

### Classification

Research-only under `docs/research/EXPERIMENT_GOVERNANCE.md`.

### Changes

1. Added `docs/research/fixtures/physical_ai_action_outcome_episodes.json`.
   - Incident families: AGV dock slip, LiDAR occlusion, robot arm torque spike,
     cold-chain temperature excursion, and drone GPS drift.
   - Query episodes carry unsafe failed actions and expected safe recovery
     actions.
   - Hard distractors make the unsafe action look attractive because it
     succeeded in training/supervised/simulated/no-human contexts.

2. Added `docs/research/tools/physical_ai_action_outcome_baseline.py`.
   - Evaluates `similar_robot_incident`, `runbook_action_prior`,
     `reflection_only_memory`, and
     `context_gated_physical_ai_action_outcome`.
   - Reports Top-3 safe hit, recovery Top-1 hit, risky-repeat avoidance,
     hazardous top-action rate, retrieval quality, and suppressions.

3. Added experiment docs and generated result:
   - `docs/research/physical_ai_action_outcome_experiment_013.md`
   - `docs/research/results/physical_ai_action_outcome_013.md`

### Verification Commands

```bash
python3 -m json.tool docs/research/fixtures/physical_ai_action_outcome_episodes.json >/tmp/physical_ai_action_outcome_episodes.pretty.json

python3 -m py_compile \
  docs/research/tools/physical_ai_action_outcome_baseline.py

python3 docs/research/tools/physical_ai_action_outcome_baseline.py \
  --fixture docs/research/fixtures/physical_ai_action_outcome_episodes.json \
  --output docs/research/results/physical_ai_action_outcome_013.md
```

### Result Summary

| Variant | Recovery Top-1 Hit | Risky Repeat Avoidance | Hazardous Top Action |
| --- | ---: | ---: | ---: |
| `similar_robot_incident` | 0.00 | 0.00 | 1.00 |
| `runbook_action_prior` | 0.00 | 0.00 | 1.00 |
| `reflection_only_memory` | 0.00 | 0.00 | 1.00 |
| `context_gated_physical_ai_action_outcome` | 1.00 | 1.00 | 0.00 |

### Interpretation

The hard distractor fixture makes the Physical AI difference concrete: generic
retrieval, runbook priors, and reflection memory can all retrieve useful
evidence in the Top-3 while still selecting the hazardous action as Top-1.

The context-gated variant keeps those misleading incidents visible for audit,
but suppresses their outcome reuse before action aggregation. This is the same
core Action-Outcome claim translated from AIOps into robot/fleet safety:
do not ask only whether a prior incident is similar; ask whether the prior
outcome is safe to reuse in this temporal and physical context.

### Next Best Step

Replay Experiment 013 through native ZeptoDB SQL tables with ROS-style telemetry
windows, ASOF JOINs, action/outcome JOINs, recommendation ranking, and
suppression audit tables.

---

## 2026-06-23 — Experiment 014 Physical AI Native SQL Replay

### Goal

Validate the Physical AI Action-Outcome comparison through live ZeptoDB native
SQL, using robot-operation-shaped tables rather than only the Python baseline
harness.

### Classification

Research-only under `docs/research/EXPERIMENT_GOVERNANCE.md`.

### Completed Work

1. Added `docs/research/tools/physical_ai_sql_replay.py`.
   - Loads the Experiment 013 fixture.
   - Recomputes the four comparison variants.
   - Creates native SQL tables for incidents, expected actions, historical
     outcomes, robot state, sensor summaries, poses, recommendations,
     retrievals, and suppressions.
   - Emits the exact SQL replay file used for validation.

2. Added Experiment 014 documentation and generated outputs.
   - Procedure:
     `docs/research/physical_ai_action_outcome_sql_replay_experiment_014.md`
   - Result:
     `docs/research/results/physical_ai_action_outcome_sql_replay_014.md`
   - SQL:
     `docs/research/results/physical_ai_action_outcome_sql_replay_014.sql`

3. Updated repository tracking docs.
   - `docs/devlog/199_physical_ai_sql_replay.md`
   - `docs/COMPLETED.md`
   - `docs/BACKLOG.md`

### Verification Commands

```bash
ninja -C build -j$(nproc) zepto_http_server

python3 -m py_compile \
  docs/research/tools/physical_ai_action_outcome_baseline.py \
  docs/research/tools/physical_ai_sql_replay.py

python3 -m json.tool docs/research/fixtures/physical_ai_action_outcome_episodes.json >/tmp/physical_ai_action_outcome_episodes.pretty.json

./build/zepto_http_server --port 19341 --no-auth --storage-mode pure

python3 docs/research/tools/physical_ai_sql_replay.py \
  --url http://127.0.0.1:19341/ \
  --fixture docs/research/fixtures/physical_ai_action_outcome_episodes.json \
  --output docs/research/results/physical_ai_action_outcome_sql_replay_014.md \
  --sql-output docs/research/results/physical_ai_action_outcome_sql_replay_014.sql \
  --timeout 10
```

### Result Summary

| Check | Status |
| --- | --- |
| Row counts | pass |
| Failed-repeat JOIN | pass |
| Context top-action JOIN | pass |
| Suppression audit JOIN | pass |
| Action/outcome JOIN | pass |
| Robot state ASOF JOIN | pass |
| Sensor ASOF JOIN | pass |
| ROW_NUMBER window | pass |
| LAG window | pass |
| ST_Within geofence | pass |
| Overall SQL replay | pass |

The replay inserted 227 research rows across nine Physical AI tables and found
15 unsafe Top-1 recommendations from the three non-gated baselines. The
context-gated variant selected all five expected recovery actions and exposed
five misleading hard distractors through the suppression audit JOIN.

### Interpretation

Experiment 014 turns the Physical AI memory claim into replayable SQL evidence:
robot incidents, event-time telemetry, action outcomes, ranked recommendations,
retrieval evidence, suppressions, and pose/geofence checks can be recorded and
audited inside ZeptoDB.

The experiment remains research-only. It intentionally uses materialized
composite keys for multi-field operational relationships and numeric ASOF
projection codes because the current native ASOF projection path is
integer-oriented.

### Next Best Step

Port the replay into a two-node live topology that separates:

- edge-local robot memory for immediate unsafe-action suppression,
- fleet-global memory for slower cross-robot consolidation and audit.

---

## 2026-06-23 — Experiment 015 Physical AI Edge/Fleet Replay

### Goal

Validate a two-node Physical AI memory shape where edge-local memory suppresses
unsafe robot actions immediately, while fleet-global memory receives delayed
consolidation rows for audit and cross-robot learning.

### Classification

Research-only under `docs/research/EXPERIMENT_GOVERNANCE.md`.

### Completed Work

1. Added `docs/research/tools/physical_ai_edge_fleet_replay.py`.
   - Uses the Experiment 013 fixture and Experiment 014 helper functions.
   - Writes edge-local SQL to one live ZeptoDB endpoint.
   - Writes fleet-global SQL to a second live ZeptoDB endpoint.
   - Validates edge immediate recovery, risky-action suppression, robot ASOF,
     and sensor ASOF.
   - Validates fleet delayed consolidation, suppression audit JOIN,
     retrieval ROW_NUMBER, and consolidation-lag LAG.

2. Added Experiment 015 documentation and generated outputs.
   - Procedure:
     `docs/research/physical_ai_edge_fleet_replay_experiment_015.md`
   - Result:
     `docs/research/results/physical_ai_edge_fleet_replay_015.md`
   - Edge SQL:
     `docs/research/results/physical_ai_edge_fleet_replay_015_edge.sql`
   - Fleet SQL:
     `docs/research/results/physical_ai_edge_fleet_replay_015_fleet.sql`

3. Updated repository tracking docs.
   - `docs/devlog/200_physical_ai_edge_fleet_replay.md`
   - `docs/COMPLETED.md`
   - `docs/BACKLOG.md`

### Verification Commands

```bash
python3 -m py_compile \
  docs/research/tools/physical_ai_action_outcome_baseline.py \
  docs/research/tools/physical_ai_sql_replay.py \
  docs/research/tools/physical_ai_edge_fleet_replay.py

python3 -m json.tool docs/research/fixtures/physical_ai_action_outcome_episodes.json >/tmp/physical_ai_action_outcome_episodes.pretty.json

./build/zepto_http_server --port 19441 --node-id 1 --no-auth --storage-mode pure

./build/zepto_http_server --port 19442 --node-id 8 --no-auth --storage-mode pure

python3 docs/research/tools/physical_ai_edge_fleet_replay.py \
  --edge-url http://127.0.0.1:19441/ \
  --fleet-url http://127.0.0.1:19442/ \
  --edge-stats-url http://127.0.0.1:19441/stats \
  --fleet-stats-url http://127.0.0.1:19442/stats \
  --fixture docs/research/fixtures/physical_ai_action_outcome_episodes.json \
  --output docs/research/results/physical_ai_edge_fleet_replay_015.md \
  --edge-sql-output docs/research/results/physical_ai_edge_fleet_replay_015_edge.sql \
  --fleet-sql-output docs/research/results/physical_ai_edge_fleet_replay_015_fleet.sql \
  --timeout 10
```

### Result Summary

| Check | Status |
| --- | --- |
| Edge row counts | pass |
| Fleet row counts | pass |
| Edge immediate recovery | pass |
| Edge risky-action suppression | pass |
| Edge robot ASOF | pass |
| Edge sensor ASOF | pass |
| Fleet consolidated recovery | pass |
| Fleet suppression audit JOIN | pass |
| Fleet consolidation lag | pass |
| Fleet ROW_NUMBER | pass |
| Fleet LAG | pass |
| Overall edge/fleet replay | pass |

The edge node stored 82 research rows and the fleet node stored 87 research
rows. Edge-local SQL selected all five expected recovery actions and suppressed
all five unsafe query actions before consolidation. Fleet-global SQL received
five delayed decisions and exposed all five misleading hard distractors through
the suppression audit JOIN.

### Interpretation

The experiment validates the target memory separation: immediate robot safety
belongs at the edge, while slower cross-robot learning and audit belong at the
fleet layer.

The transfer is still harness-driven SQL copy, not product replication. This
keeps the result honest while making the next engineering boundary clear.

### Next Best Step

Replace harness-driven consolidation with an explicit bounded edge-to-fleet feed
or replication path, then test duplicate, dropped, late, outage, and restart
cases.

## 2026-06-23 — Experiment 016 Physical AI Edge/Fleet Feed Replay

### Goal

Replace the Experiment 015 direct fleet-row copy with an explicit bounded
edge-to-fleet feed path and validate dropped, duplicated, late, outage, and
restart behavior while preserving immediate edge-local robot safety.

### Classification

Research-only under `docs/research/EXPERIMENT_GOVERNANCE.md`.

### Work Completed

1. Added `docs/research/tools/physical_ai_edge_fleet_feed_replay.py`.
   - Writes edge-local incidents, expected actions, robot state, sensor
     summaries, decisions, suppressions, and a 52-row edge outbox.
   - Seeds fleet-global historical action outcomes and expected safe actions.
   - Transfers only edge-generated `decision`, `retrieval`, and `suppression`
     events through a bounded feed worker.
   - Records fleet inbox attempts, persistent ACK rows, and feed telemetry.
   - Injects outage, dropped-event retry, duplicate attempt, late delivery, and
     restart ACK reload phases.

2. Added Experiment 016 docs and generated outputs.
   - Procedure:
     `docs/research/physical_ai_edge_fleet_feed_replay_experiment_016.md`
   - Result:
     `docs/research/results/physical_ai_edge_fleet_feed_replay_016.md`
   - Edge SQL:
     `docs/research/results/physical_ai_edge_fleet_feed_replay_016_edge.sql`
   - Fleet SQL:
     `docs/research/results/physical_ai_edge_fleet_feed_replay_016_fleet.sql`

3. Updated repository tracking docs.
   - `docs/devlog/201_physical_ai_edge_fleet_feed_replay.md`
   - `docs/COMPLETED.md`
   - `docs/BACKLOG.md`
   - `docs/research/physical_ai_edge_fleet_replay_experiment_015.md`

### Verification Commands

```bash
python3 -m py_compile docs/research/tools/physical_ai_edge_fleet_feed_replay.py

python3 -m json.tool docs/research/fixtures/physical_ai_action_outcome_episodes.json >/tmp/physical_ai_action_outcome_episodes.pretty.json

./build/zepto_http_server --port 19441 --node-id 1 --no-auth --storage-mode pure

./build/zepto_http_server --port 19442 --node-id 8 --no-auth --storage-mode pure

python3 docs/research/tools/physical_ai_edge_fleet_feed_replay.py \
  --edge-url http://127.0.0.1:19441/ \
  --fleet-url http://127.0.0.1:19442/ \
  --outage-url http://127.0.0.1:1/ \
  --edge-stats-url http://127.0.0.1:19441/stats \
  --fleet-stats-url http://127.0.0.1:19442/stats \
  --fixture docs/research/fixtures/physical_ai_action_outcome_episodes.json \
  --output docs/research/results/physical_ai_edge_fleet_feed_replay_016.md \
  --edge-sql-output docs/research/results/physical_ai_edge_fleet_feed_replay_016_edge.sql \
  --fleet-sql-output docs/research/results/physical_ai_edge_fleet_feed_replay_016_fleet.sql \
  --timeout 10 \
  --batch-limit 12 \
  --max-inflight 12
```

### Result Summary

| Check | Status |
| --- | --- |
| Edge row counts | pass |
| Edge immediate recovery | pass |
| Edge risky-action suppression | pass |
| Feed ACK convergence | pass |
| Feed event-kind accounting | pass |
| Duplicate delivery handling | pass |
| Late delivery handling | pass |
| Outage retry | pass |
| Restart ACK reload | pass |
| Bounded batch limit | pass |
| Fleet final row counts | pass |
| Fleet recovery JOIN | pass |
| Fleet suppression audit JOIN | pass |
| Fleet ACK ROW_NUMBER/LAG | pass |
| Overall bounded feed replay | pass |

The edge node stored 134 research rows and the fleet node stored 198 research
rows. Edge-local memory still selected all five expected recovery actions and
suppressed all five unsafe query actions immediately. Fleet-global memory
eventually ACKed all 52 edge outbox events after one duplicate attempt, two
late attempts, an outage probe, and a feed-worker restart that reloaded prior
ACK state.

### Interpretation

Experiment 016 validates a stronger Physical AI memory architecture: edge-local
memory owns immediate suppression, while fleet-global memory can be slower,
auditable, and eventually consistent through bounded feed semantics.

The result is not product replication yet. It proves the semantics that an
experimental runtime connector must preserve: bounded batches, idempotent
delivery, late/outage/restart convergence, and visible feed telemetry.

### Next Best Step

Promote the research feed semantics into an experimental runtime connector with
persisted cursor/checkpoint state, operator-visible metrics, and documented
behavior for final-table insert success followed by ACK failure.

## 2026-06-23 — Experiment 017 Physical AI Edge/Fleet Runtime Connector

### Goal

Promote Experiment 016 bounded edge-to-fleet feed semantics from a Python
research harness into a reusable C++ experimental runtime connector.

### Classification

Experimental runtime path under `docs/research/EXPERIMENT_GOVERNANCE.md`.

### Work Completed

1. Added `EdgeFleetFeedConnector`.
   - Header: `include/zeptodb/feeds/edge_fleet_feed_connector.h`
   - Implementation: `src/feeds/edge_fleet_feed_connector.cpp`
   - Library wiring: `zepto_feeds`

2. Added runtime semantics.
   - Bounded passes with `batch_limit` and `max_inflight`.
   - ACKed event-id duplicate suppression.
   - Late-event detection by stream sequence.
   - Transient retry accounting.
   - Optional local checkpoint load/save for restart ACK reload.
   - `AppliedButAckFailed` handling for final-table success followed by ACK
     persistence failure.
   - Prometheus/OpenMetrics formatting for connector stats.

3. Added focused C++ tests.
   - `tests/unit/test_edge_fleet_feed_connector.cpp`
   - Covers config validation, bounded batch, checkpoint persistence,
     dropped/outage-style transient retry, duplicate handling, late delivery,
     restart reload, malformed input, ACK-boundary failure, and metrics.

4. Updated documentation and governance.
   - `docs/devlog/202_physical_ai_edge_fleet_runtime_connector.md`
   - `docs/research/physical_ai_edge_fleet_runtime_connector_experiment_017.md`
   - `docs/api/CPP_REFERENCE.md`
   - `docs/design/layer2_ingestion_network.md`
   - `docs/research/EXPERIMENT_GOVERNANCE.md`
   - `docs/COMPLETED.md`
   - `docs/BACKLOG.md`

### Verification Commands

```bash
cd build && ninja -j$(nproc) zepto_tests

./build/tests/zepto_tests --gtest_filter='EdgeFleetFeedConnectorTest.*'
```

### Result Summary

| Check | Status |
| --- | --- |
| `zepto_tests` build | pass |
| `EdgeFleetFeedConnectorTest.ConfigDefaultsAndValidation` | pass |
| `EdgeFleetFeedConnectorTest.ProcessesBoundedBatchAndPersistsAcks` | pass |
| `EdgeFleetFeedConnectorTest.RetriesDroppedEventAndAcceptsLateDelivery` | pass |
| `EdgeFleetFeedConnectorTest.ReloadsCheckpointAfterRestart` | pass |
| `EdgeFleetFeedConnectorTest.KeepsEventUnackedOnAckBoundaryFailure` | pass |
| `EdgeFleetFeedConnectorTest.RejectsMalformedEventsAndCanBlockLateEvents` | pass |
| `EdgeFleetFeedConnectorTest.RetriesTransientFailureWithinPass` | pass |
| `EdgeFleetFeedConnectorTest.FormatsPrometheusCounters` | pass |

### Interpretation

The core edge-to-fleet memory transport semantics are now in runtime C++ code,
not just the research harness. This narrows product risk: the remaining work is
adapter and operations integration, not the feed state machine itself.

This is still not a promoted product feature. The connector is transport
neutral and requires a concrete SQL/HTTP source/sink adapter, live two-node
runtime replay, lifecycle controls, and admin/security docs before promotion.

### Next Best Step

Build the ZeptoDB SQL/HTTP source/sink adapter for Experiment 016 tables, then
rerun the two-node Physical AI replay through `EdgeFleetFeedConnector` instead
of the Python feed worker.

## 2026-06-23 — Experiment 018 Physical AI Edge/Fleet C++ Connector Replay

### Goal

Connect `EdgeFleetFeedConnector` to two live ZeptoDB HTTP SQL nodes and prove
that it can replace the Python feed worker for the bounded Physical AI
edge-to-fleet replay.

### Classification

Experimental runtime path under `docs/research/EXPERIMENT_GOVERNANCE.md`.

### Work Completed

1. Added a standalone C++ live replay tool.
   - Tool: `tools/zepto_edge_fleet_replay.cpp`
   - Target: `zepto_edge_fleet_replay`
   - Result report:
     `docs/research/results/physical_ai_edge_fleet_cpp_connector_replay_018.md`

2. Wired concrete SQL/HTTP source and sink behavior.
   - Applies the Experiment 016 edge SQL fixture to an edge node.
   - Applies only fleet DDL/base seed statements to a fleet node.
   - Reads `physical_ai_edge_feed_outbox_016` through native SQL.
   - Converts rows to `EdgeFleetFeedEvent`.
   - Materializes fleet inbox, decision, retrieval, suppression, ACK, and
     telemetry rows through SQL inserts.

3. Validated failure and convergence phases.
   - Outage probe.
   - Bounded recovery with one dropped event and one duplicate event.
   - Restart checkpoint reload with late delivery.
   - Final bounded drain to 52/52 ACK rows.

4. Updated documentation and governance.
   - `docs/devlog/203_physical_ai_edge_fleet_cpp_connector_replay.md`
   - `docs/research/physical_ai_edge_fleet_cpp_connector_replay_experiment_018.md`
   - `docs/design/layer2_ingestion_network.md`
   - `docs/api/CPP_REFERENCE.md`
   - `docs/research/EXPERIMENT_GOVERNANCE.md`
   - `docs/COMPLETED.md`
   - `docs/BACKLOG.md`

### Verification Commands

```bash
cd build && ninja -j$(nproc) zepto_edge_fleet_replay zepto_tests

./build/tests/zepto_tests --gtest_filter='EdgeFleetFeedConnectorTest.*'

./build/zepto_edge_fleet_replay \
  --edge-url http://127.0.0.1:19441 \
  --fleet-url http://127.0.0.1:19442 \
  --outage-url http://127.0.0.1:1 \
  --edge-sql docs/research/results/physical_ai_edge_fleet_feed_replay_016_edge.sql \
  --fleet-seed-sql docs/research/results/physical_ai_edge_fleet_feed_replay_016_fleet.sql \
  --output docs/research/results/physical_ai_edge_fleet_cpp_connector_replay_018.md \
  --checkpoint /tmp/zeptodb_edge_fleet_cpp_connector_018.checkpoint \
  --batch-limit 12 \
  --max-inflight 12
```

### Result Summary

| Check | Status |
| --- | --- |
| `zepto_edge_fleet_replay` build | pass |
| `EdgeFleetFeedConnectorTest.*` | pass, 8/8 |
| Live C++ connector replay | pass |
| Fleet ACK convergence | 52/52 |
| Fleet decision rows | 5 |
| Fleet retrieval rows | 15 |
| Fleet suppression rows | 32 |
| Outage telemetry | pass |
| Duplicate telemetry | pass |
| Late telemetry | pass |
| Restart reload telemetry | pass |
| Recovery JOIN | 5 rows |
| Suppression audit JOIN | 32 rows |

### Interpretation

Experiment 018 closes the adapter validation gap from Experiment 017. The
runtime connector now has a concrete live SQL/HTTP replay path over two ZeptoDB
nodes, and the fleet audit converges after outage, dropped, duplicate, late,
and restart phases.

This remains experimental. The adapter is a standalone replay tool, not a
server-managed connector. Product promotion still requires lifecycle controls,
RBAC/admin policy, persisted feed configuration and ACK/cursor state, and
long-running operational tests.

### Next Best Step

Design the product lifecycle for this connector: how it is enabled, where ACK
state lives, which admin role controls it, and how metrics are exposed without
running the standalone experiment tool.

## 2026-06-23 — Experiment 019 Physical AI Edge/Fleet Server Lifecycle

### Goal

Move the Physical AI edge/fleet connector from standalone replay-tool ownership
toward server-managed lifecycle ownership.

### Classification

Experimental runtime path under `docs/research/EXPERIMENT_GOVERNANCE.md`.

### Work Completed

1. Added `EdgeFleetConnectorRuntime`.
   - Header: `include/zeptodb/feeds/edge_fleet_connector_runtime.h`
   - Implementation: `src/feeds/edge_fleet_connector_runtime.cpp`
   - Owns process-local connector config, enabled state, checkpoint
     start/stop behavior, lifecycle counters, snapshots, and Prometheus text.

2. Added server admin lifecycle endpoints.
   - `GET /admin/edge-fleet-connector`
   - `POST /admin/edge-fleet-connector`
   - `DELETE /admin/edge-fleet-connector`

3. Added metrics exposure.
   - `/metrics` now includes lifecycle gauges/counters and the underlying
     `EdgeFleetFeedConnector` counters.

4. Added focused tests.
   - Runtime manager start/stop/clear.
   - Invalid bounded limits.
   - Missing checkpoint startup.
   - HTTP configure/enable/delete lifecycle.
   - HTTP invalid limit rejection.
   - HTTP 401 without credentials, 403 with writer credentials, and 200 with
     admin credentials.

5. Updated documentation and governance.
   - `docs/devlog/204_physical_ai_edge_fleet_server_lifecycle.md`
   - `docs/research/physical_ai_edge_fleet_lifecycle_experiment_019.md`
   - `docs/api/CPP_REFERENCE.md`
   - `docs/api/HTTP_REFERENCE.md`
   - `docs/design/layer2_ingestion_network.md`
   - `docs/research/EXPERIMENT_GOVERNANCE.md`
   - `docs/COMPLETED.md`
   - `docs/BACKLOG.md`

### Verification Commands

```bash
cd build && ninja -j$(nproc) zepto_tests zepto_http_server zepto_edge_fleet_replay

./build/tests/zepto_tests \
  --gtest_filter='EdgeFleetFeedConnectorTest.*:EdgeFleetConnectorRuntimeTest.*'

./build/tests/zepto_tests \
  --gtest_filter='MetricsProviderTest.EdgeFleetConnector*:EdgeFleetConnectorAdminAuthTest.*'
```

### Result Summary

| Check | Status |
| --- | --- |
| Build | pass |
| Connector/runtime focused tests | pass, 12/12 |
| HTTP lifecycle/admin tests | pass, 3/3 |
| Runtime metrics | pass |
| HTTP invalid limits | pass |
| HTTP admin authorization | pass |

### Interpretation

Experiment 019 creates the server control plane for the connector. This removes
one major product blocker from Experiment 018: operators no longer need a
standalone replay tool to own connector lifecycle state and metrics.

This is still experimental. At the time of Experiment 019 the server did not
yet run a worker. Experiment 020 adds the generic worker hook/loop; the built-in
SQL outbox adapter and fleet sink execution remain open. Connector config is
process-local and not catalog-persisted.

### Next Best Step

Experiment 020 supersedes the generic worker portion of this step. The next
remaining step is the built-in SQL/HTTP adapter that reads the configured edge
outbox table, applies fleet sink rows, updates ACK state, exposes pass
telemetry, and survives restart without relying on `zepto_edge_fleet_replay`.

## 2026-06-23 — Experiment 020 Physical AI Edge/Fleet Worker Runtime

### Goal

Add the bounded server-managed worker foundation required to move the
Physical AI edge/fleet connector from lifecycle-only control toward a
production candidate.

### Classification

Experimental runtime path under `docs/research/EXPERIMENT_GOVERNANCE.md`.

### Work Completed

1. Extended `EdgeFleetConnectorRuntime`.
   - Added `EdgeFleetConnectorRuntimeHooks`.
   - Added `setWorkerHooks()`.
   - Added manual `runOnce()`.
   - Added background worker mode via `worker_enabled` and
     `worker_poll_interval_ms`.

2. Added worker telemetry.
   - Worker hook readiness.
   - Worker running state.
   - Worker start/pass/load-error/observer-error counters.
   - Last bounded pass result in the runtime snapshot.
   - Prometheus worker metrics.

3. Extended HTTP server integration.
   - `HttpServer::set_edge_fleet_connector_runtime_hooks()`.
   - `GET /admin/edge-fleet-connector` now reports worker fields.
   - `POST /admin/edge-fleet-connector` accepts `worker_enabled` and
     `worker_poll_interval_ms`.

4. Added focused tests.
   - Manual bounded worker pass.
   - Background convergence across multiple bounded batches.
   - Loader outage/error accounting and recovery.
   - Missing-hook rejection.
   - HTTP admin lifecycle with installed hooks.
   - HTTP worker metrics.

5. Added production-readiness planning.
   - `docs/research/physical_ai_edge_fleet_production_readiness_plan.md`
   - `docs/research/physical_ai_edge_fleet_worker_experiment_020.md`
   - `docs/devlog/205_physical_ai_edge_fleet_worker_runtime.md`

### Verification Commands

```bash
cd build && ninja -j$(nproc) zepto_tests zepto_http_server zepto_edge_fleet_replay

./build/tests/zepto_tests \
  --gtest_filter='EdgeFleetFeedConnectorTest.*:EdgeFleetConnectorRuntimeTest.*'

./build/tests/zepto_tests \
  --gtest_filter='MetricsProviderTest.EdgeFleetConnector*:EdgeFleetConnectorAdminAuthTest.*'
```

### Result Summary

| Check | Status |
| --- | --- |
| Build | pass |
| Connector/runtime focused tests | pass, 16/16 |
| HTTP lifecycle/admin tests | pass, 5/5 |
| Manual bounded worker pass | pass |
| Background worker convergence | pass |
| Loader outage recovery | pass |
| Missing-hook rejection | pass |
| Worker metrics | pass |

### Interpretation

Experiment 020 closes the worker-lifecycle gap from Experiment 019. The runtime
now owns bounded pass execution and worker telemetry rather than only storing
configuration and lifecycle state.

This is still experimental. The server runtime exposes a hook contract, but it
does not yet include a built-in SQL/HTTP adapter that reads the configured edge
outbox table and materializes fleet rows. Connector configuration also remains
process-local.

### Next Best Step

Build the concrete SQL/HTTP adapter into the server runtime by extracting the
reusable outbox loading and fleet sink materialization logic from
`zepto_edge_fleet_replay`, then validate the same dropped, duplicated, late,
outage, restart, recovery JOIN, and suppression audit cases through the
server-managed worker path.

## 2026-07-04 - Experiment 021 shadow supervisor A/B and durability replay

### Classification

Research-only evidence plus focused C++ durability regression under the
experimental Action-Outcome supervisor runtime path.

### Work Completed

1. Added Experiment 021.
   - `docs/research/physical_ai_shadow_supervisor_ab_experiment_021.md`
   - `docs/research/tools/physical_ai_shadow_supervisor_ab.py`
   - `docs/research/results/physical_ai_shadow_supervisor_ab_021.md`

2. Replayed A/B shadow proposals.
   - 15 hazardous proposals from non-gated baselines:
     `similar_robot_incident`, `runbook_action_prior`, and
     `reflection_only_memory`.
   - 5 safe proposals from
     `context_gated_physical_ai_action_outcome`.
   - Shadow supervisor suppresses 15/15 hazardous proposals and allows 5/5
     safe proposals.

3. Replayed D durability/idempotency.
   - First pass processes 20 proposals and writes 20 decision/evidence records.
   - Restart replay skips 20/20 proposals as already decided.
   - Restart replay writes 0 new evidence rows.

4. Added focused C++ guard.
   - `ActionOutcomeSqlAdapterTest.RestartedRuntimeSkipsPersistedDecisions`
     creates a fresh runtime object against the same SQL tables and verifies
     persisted decision rows suppress duplicate processing.

### Verification Commands

```bash
python3 -m py_compile docs/research/tools/physical_ai_shadow_supervisor_ab.py

python3 docs/research/tools/physical_ai_shadow_supervisor_ab.py \
  --fixture docs/research/fixtures/physical_ai_action_outcome_episodes.json \
  --output docs/research/results/physical_ai_shadow_supervisor_ab_021.md

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSqlAdapterTest.RestartedRuntimeSkipsPersistedDecisions'
```

### Result Summary

| Check | Status |
| --- | --- |
| Python harness compile | pass |
| Experiment 021 acceptance | pass |
| Hazardous proposal suppression | pass, 15/15 |
| Safe proposal allow | pass, 5/5 |
| Restart duplicate skip | pass, 20/20 |
| Restart evidence stability | pass, 0 new rows |
| Focused C++ durability test | pass |

### Interpretation

Experiment 021 strengthens the commercial case for ZeptoDB as a Physical AI
Action-Outcome substrate without widening the product API. The A evidence
shows why shadow supervision is more valuable than incident search; the D
evidence identifies the first runtime durability boundary: persisted decision
rows prevent duplicate work.

This does not close product promotion. The next blocker is durable supervisor
configuration so the SQL adapter can be reinstalled automatically after a full
server restart.

### Next Best Step

Devlog 209 should add persisted experimental supervisor adapter configuration,
then rerun Experiment 021 through a live HTTP server restart instead of a
research-only replay plus fresh runtime object.

## 2026-07-04 - Devlog 209 live HTTP restart config persistence

### Classification

Experimental runtime path under the Physical AI Action-Outcome supervisor.

### Goal

Close the next durability gap after Experiment 021: persist the SQL adapter
configuration itself and verify that a real HTTP server restart can reinstall
the adapter automatically.

### Work Completed

1. Added server-local durable adapter config.
   - `HttpServer::set_action_outcome_supervisor_config_persistence(path)`
     enables persistence and loads any existing config.
   - Successful admin SQL-adapter configuration writes versioned JSON through a
     temp file and rename.
   - The persisted file includes runtime fields, table names, SQL adapter
     column names, adapter limits, `enabled`, and
     `sql_adapter_create_tables`.

2. Added restart restore behavior.
   - On load, the server validates the persisted file.
   - The runtime is configured from the persisted adapter config.
   - SQL-backed hooks are reinstalled against the new server's
     `QueryExecutor`.
   - Default SQL tables are recreated idempotently when the persisted config
     requested table creation.
   - If the persisted config was enabled, the supervisor is started.

3. Tightened lifecycle semantics.
   - `HttpServer::stop()` now stops experimental background workers before the
     listener is fully torn down.
   - `DELETE /admin/action-outcome-supervisor` removes the persisted adapter
     config when persistence is enabled.

4. Added live restart verification.
   - `MetricsProviderTest.ActionOutcomeSupervisorSqlAdapterConfigPersistsAndReloadsAfterHttpRestart`
     configures the SQL adapter through HTTP, persists it, stops the server
     object, recreates a new HTTP server on the same port and executor, loads
     the persisted config, and verifies `worker_hooks_configured=true`.
   - The test then enables the supervisor without reposting
     `sql_adapter_enabled`; processing succeeds only if the hooks were already
     reinstalled from the persisted file.

### Verification Commands

```bash
cmake --build build --target zepto_tests -j$(nproc)

./build/tests/zepto_tests \
  --gtest_filter='MetricsProviderTest.ActionOutcomeSupervisorSqlAdapterConfigPersistsAndReloadsAfterHttpRestart'

./build/tests/zepto_tests \
  --gtest_filter='MetricsProviderTest.ActionOutcomeSupervisor*:ActionOutcomeSqlAdapterTest.*:ActionOutcomeSqlAdapterConfigTest.*'
```

### Result Summary

| Check | Status |
| --- | --- |
| Build | pass |
| Live HTTP restart config persistence | pass |
| Automatic SQL hook reinstall after restart | pass |
| Enable-after-reinstall proposal processing | pass |
| Focused Action-Outcome admin/SQL adapter regression | pass, 11/11 |

### Interpretation

The Action-Outcome supervisor now has both durability boundaries needed for the
next commercialization demo: decision rows prevent duplicate proposal work, and
server-local config persistence restores the SQL adapter after a real HTTP
server restart. This still does not promote the path to product status because
the config is file-local rather than catalog/cluster-owned, worker ownership is
not cluster-safe, and the decision/evidence sink is not transactional.

### Next Best Step

Move from single-server durability to cluster promotion hardening: catalog- or
control-plane-owned supervisor config, one-owner worker semantics, and
node-replacement replay validation.

## 2026-07-05 - Devlog 210 P0 production hardening

### Classification

Experimental runtime path hardening for Physical AI Action-Outcome supervisor
promotion.

### Goal

Start closing production P0 blockers in priority order:

1. Prevent evidence duplication after partial sink failure.
2. Fence stale or non-owner workers before proposal loading.
3. Add node-replacement style ownership handoff validation.

### Work Completed

1. Closed retry evidence duplication.
   - The SQL sink now checks for an existing evidence summary by proposal id
     before inserting evidence.
   - If evidence was inserted but the decision insert failed, retry skips the
     evidence insert and writes the missing decision row.

2. Added SQL worker ownership fencing.
   - `ActionOutcomeSqlAdapterConfig` can require worker ownership.
   - Ownership is read from
     `physical_ai_supervisor_ownership(supervisor_name, owner_id, owner_epoch)`.
   - Non-owner or stale-epoch workers return an idle proposal load instead of
     processing proposals.
   - The ownership row is enforced by the runtime, but still maintained by an
     external control-plane/catalog path.

3. Extended durable config and HTTP admin.
   - Ownership fields are accepted by
     `POST /admin/action-outcome-supervisor`.
   - Ownership fields are persisted in the server-local config JSON and loaded
     after HTTP restart.

4. Added focused tests.
   - Evidence-success/decision-failure retry does not duplicate evidence.
   - Non-owner and stale epoch workers idle.
   - Node-a to node-b handoff fences the replaced owner.
   - Live HTTP restart restores ownership-gated SQL adapter config.

### Verification Commands

```bash
cmake --build build --target zepto_tests -j$(nproc)

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSqlAdapterTest.*:ActionOutcomeSqlAdapterConfigTest.*'

./build/tests/zepto_tests \
  --gtest_filter='MetricsProviderTest.ActionOutcomeSupervisor*:ActionOutcomeSqlAdapterTest.*:ActionOutcomeSqlAdapterConfigTest.*'
```

### Result Summary

| Check | Status |
| --- | --- |
| Build | pass |
| SQL adapter P0 hardening tests | pass, 8/8 |
| HTTP/admin plus SQL adapter focused tests | pass, 13/13 |

### Remaining Problems

- Config is still server-local JSON rather than catalog/cluster-owned state.
- Ownership is enforced but not elected or leased by ZeptoDB.
- Decision/evidence writes are retry-idempotent for evidence summaries, but not
  one atomic transaction.
- Broader RBAC/audit coverage for mutating supervisor controls is still needed.
- Rate/backpressure limits are still basic and tied to batch/query limits.
- Long-running fault/soak and cross-architecture verification remain open.

### Next Best Step

Promote the ownership row into a catalog/control-plane owned config path, then
add lease/election semantics with stale epoch rejection across real cluster
nodes.

## 2026-07-09 - Experiments 022 and 023 closure

### Classification

Experimental runtime path validation for the Physical AI Action-Outcome
supervisor.

### Goal

Close the two remaining P0 validation questions from devlog 210:

1. Node-replacement shaped ownership handoff and stale-owner fencing.
2. Commit-ledger sink stress under repeated projection faults and fresh runtime
   repair passes.

### Work Completed

1. Added Experiment 022.
   - Node A commits one proposal under a managed SQL ownership lease.
   - Node B takes over the expired lease with a higher epoch.
   - A stale node A runtime is fenced to an idle pass.
   - Restarted node B converges the remaining proposal stream.

2. Added Experiment 023.
   - Six bounded passes insert 12 total proposals.
   - Three malformed evidence projection faults are injected.
   - Fresh runtime objects repair committed state after schema restoration.
   - Commit, decision, and evidence proposal ids stay unique after every pass.

### Verification Commands

```bash
ninja -C build -j$(nproc) zepto_tests

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSqlAdapterTest.WorkerLeaseRollingRestartNodeReplacementFencesStaleOwnerAndConverges:ActionOutcomeSqlAdapterTest.CommitLedgerStressRepairsProjectionFailuresAcrossRestarts' \
  --gtest_brief=1

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSqlAdapterTest.*:ActionOutcomeSqlAdapterConfigTest.*' \
  --gtest_brief=1

cd build && ctest -j$(nproc) -E "Benchmark\.|K8s" \
  --output-on-failure --timeout 180

./tools/run-full-matrix.sh --stages=8 --force-resync

ZEPTO_S3_TEST_BUCKET=<temporary-bucket> \
  ./build/tests/zepto_tests --gtest_filter='S3Sink.*' --gtest_brief=1
```

### Result Summary

| Check | Status |
| --- | --- |
| Build | pass |
| Experiment 022/023 focused regressions | pass, 2/2 |
| Action-Outcome SQL adapter suite | pass, 14/14 |
| x86_64 CTest | pass, 1712/1712 |
| aarch64 stage 8 CTest | pass, 1712/1712 |
| Live S3 opt-in smoke | pass, 2/2; temporary bucket cleanup verified |

### Remaining Limits

- The SQL lease is a supervisor-specific fencing guard, not consensus.
- The commit ledger is a supervisor-specific effectively-once sink boundary,
  not a generic multi-table SQL transaction primitive.
- Product promotion still needs an explicit GA/operator rollout decision.
