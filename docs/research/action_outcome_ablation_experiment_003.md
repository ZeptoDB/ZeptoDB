# ActionOutcomeReplay Experiment 003: Signal Ablation

Date: 2026-06-13
Branch: `codex/aiops-time-series-memory-research`

## Purpose

Experiment 002 showed that guarded retrieval can reduce weak cross-family
candidates while preserving successful-action hit rate and failed-action
avoidance on the current fixture.

Experiment 003 asks:

> Which signal families actually drive the recommendation?

This matters because the research claim is not merely "similar incident
retrieval works". The claim is that **time-series action-outcome memory** adds
value beyond text, topology, symptoms, or change context alone.

## Baseline For Ablation

Use Guarded V2 from Experiment 002:

- symptom similarity,
- temporal motif similarity,
- topology similarity,
- change similarity,
- action-outcome evidence,
- postmortem/text similarity,
- recency,
- risk penalty,
- cross-family guardrail,
- robust action aggregation.

## Ablation Variants

| Variant | Removed Signal | Reason |
| --- | --- | --- |
| `full_guarded` | none | Reference guarded V2. |
| `no_symptom` | symptom similarity | Tests whether alert/log/trace token overlap dominates. |
| `no_temporal` | temporal motif similarity | Tests value of metric shape/severity. |
| `no_topology` | topology similarity | Tests dependency/entity context importance. |
| `no_change` | deploy/config/flag context | Tests whether change context overpowers other signals. |
| `no_action_outcome` | action outcome evidence | Tests the core research claim. |
| `no_text` | postmortem/reflection text similarity | Tests whether text-only memories dominate. |
| `no_recency` | recency | Tests whether ordering bias matters. |
| `no_risk` | risk penalty | Tests safety penalty contribution. |
| `no_cross_family_guardrail` | cross-family penalty | Tests Experiment 002 guardrail contribution. |

## Metrics

| Metric | Meaning |
| --- | --- |
| `top3_successful_action_hit_rate` | Whether top-3 actions include known useful actions. |
| `failed_action_avoidance_rate` | Whether failed query actions are not repeated as top-1. |
| `cross_family_top3_count` | Cross-family candidates in retrieval top-3. |
| `weak_cross_family_top3_count` | Cross-family top-3 candidates with weak topology similarity. |
| `top_action_change_count` | Number of query top actions that differ from `full_guarded`. |

## Interpretation Rules

- If `no_action_outcome` performs the same as `full_guarded`, the current fixture
  does not prove action-outcome memory is necessary.
- If `no_topology` or `no_cross_family_guardrail` increases weak cross-family
  retrieval, the system needs structural guardrails before noisy data.
- If `no_change` changes many top actions, deploy/config context is a dominant
  factor and should be carefully calibrated.
- If `no_text` has little effect, the current value comes from structured
  telemetry rather than postmortem text.

## Output

Write the result report to:

- `docs/research/results/action_outcome_ablation_003.md`

Record findings in:

- `docs/research/action_outcome_research_process_log.md`
