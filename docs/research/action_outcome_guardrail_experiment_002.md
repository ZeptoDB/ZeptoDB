# ActionOutcomeReplay Experiment 002: Retrieval Guardrails

Date: 2026-06-13
Branch: `codex/aiops-time-series-memory-research`

## Purpose

Experiment 001 showed that action-outcome retrieval can avoid repeating failed
actions in the clean synthetic fixture. It also exposed a weakness:

> A candidate from a different incident family can enter the top-3 when action
> class and change context are similar enough.

Experiment 002 tests a guarded retrieval and action aggregation strategy that
reduces weak cross-family candidates without removing potentially useful
cross-domain lessons.

## Baseline

Baseline is the Experiment 001 action-outcome scorer:

- symptom similarity,
- temporal motif similarity,
- topology similarity,
- change similarity,
- action-outcome score,
- text similarity,
- recency,
- risk penalty.

Weakness:

- It can overvalue action outcome and change context when incident family or
  topology do not match.
- It aggregates action recommendations by averaging `score * outcome_value`
  across the top candidates, which can let one weak positive candidate influence
  the recommendation too much.

## Guarded V2 Strategy

### Retrieval Guardrail

Apply an additional penalty to cross-family candidates:

| Condition | Penalty |
| --- | ---: |
| Same incident family | 0.00 |
| Cross-family but strong topology and symptom match | 0.05 |
| Cross-family with moderate topology match | 0.12 |
| Cross-family weakly related | 0.22 |

The goal is not to forbid cross-family transfer. The goal is to make cross-family
transfer earn its place through topology and symptom evidence.

### Robust Action Aggregation

Baseline action aggregation averages candidate scores weighted by outcome value.
Guarded V2 instead:

- sums positive evidence from successful outcomes,
- subtracts stronger negative evidence from failed or rollback-required outcomes,
- normalizes by support,
- adds a small support bonus for multiple successful examples,
- subtracts a repeated-failure penalty.

This should reduce recommendations based on one weak positive candidate while
preserving strong repeated successes.

## Evaluation Queries

Use the same six query episodes as Experiment 001:

- `aoe_checkout_002`
- `aoe_payment_002`
- `aoe_inventory_002`
- `aoe_cache_002`
- `aoe_queue_003`
- `aoe_search_003`

## Metrics

| Metric | Meaning |
| --- | --- |
| `top3_successful_action_hit` | Top-3 recommendations include a known useful action for the incident family. |
| `failed_action_avoidance` | If the historical query action failed, the recommender does not repeat it as top-1. |
| `cross_family_top3_count` | Number of cross-family candidates in top-3 action-outcome retrieval. |
| `weak_cross_family_top3_count` | Cross-family top-3 candidates with topology similarity below 0.55. |
| `top_action_changed` | Whether guarded V2 changes the top action from baseline. |

## Expected Result

Guarded V2 should:

- keep top-3 successful action hit rate high,
- keep failed-action avoidance high,
- reduce weak cross-family candidates,
- expose any cases where a useful cross-family analogy was wrongly suppressed.

## Output

The comparison report should be written to:

- `docs/research/results/action_outcome_guardrail_002.md`

The process log should record:

- implementation summary,
- verification commands,
- result summary,
- next best step.
