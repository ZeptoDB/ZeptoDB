# Action-Outcome Vendor Baseline Experiment 010

Date: 2026-06-18

## Purpose

Experiments 016 and 017 identified the industry-adjacent baselines around
Action-Outcome Memory: similar incident recommendation, runbook/action-prior
automation, reflection-style agent memory, and closed-loop remediation.

Experiment 010 turns that positioning into a direct fixture comparison. The
goal is to test whether ZeptoDB's context-gated Action-Outcome Memory shows a
measurable safety advantage over vendor-inspired baselines on the existing noisy
AIOps fixture.

## Inputs

- Base fixture: `docs/research/fixtures/action_outcome_episodes.json`
- Noisy distractor fixture:
  `docs/research/fixtures/action_outcome_distractor_episodes.json`
- Retrieval quality labels:
  `docs/research/fixtures/action_outcome_retrieval_quality_labels.json`

## Baselines

| Variant | Industry pattern approximated | What it can miss |
| --- | --- | --- |
| `similar_incident` | Similar historical incident recommendation. | Retrieves superficially similar evidence without learning whether the action worked in the right context. |
| `runbook_action_prior` | Runbook or next-best-action prior per incident family. | Reuses historically successful actions even when the current topology/change/metric context differs. |
| `reflection_only_memory` | Agent experiential memory from reflections/postmortems. | Can recall success/failure text but lacks explicit structured context gates. |
| `context_gated_action_outcome` | ZeptoDB Action-Outcome Memory target. | Still heuristic, but explicitly suppresses outcome reuse when context is incompatible. |

## Command

```bash
python3 docs/research/tools/action_outcome_vendor_baseline.py \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_vendor_baseline_010.md
```

## Result Summary

| Variant | Top-3 Hit Rate | Failed-Action Avoidance | Misleading Top3 | Suppressions |
| --- | ---: | ---: | ---: | ---: |
| `similar_incident` | 1.00 | 0.67 | 6 | 0 |
| `runbook_action_prior` | 1.00 | 0.67 | 5 | 0 |
| `reflection_only_memory` | 1.00 | 0.83 | 6 | 0 |
| `context_gated_action_outcome` | 1.00 | 1.00 | 6 | 21 |

## Interpretation

The headline Top-3 hit metric is too easy: every variant finds at least one
historically successful action for each query. The stronger commercial signal is
failed-action avoidance under noisy same-family distractors.

`context_gated_action_outcome` is the only variant that reaches perfect
failed-action avoidance while preserving Top-3 useful-action coverage. It does
not eliminate every misleading retrieval from the top-3 evidence list; instead,
it prevents misleading positive or negative outcomes from dominating the final
action aggregation by suppressing 21 incompatible outcome reuses.

This is the real differentiation:

```text
not "we find similar incidents"
not "we have a runbook prior"
not "we remember reflections"
but "we decide whether a past outcome is safe to reuse in this context"
```

## Next Best Step

Experiment 010 has now been replayed through native ZeptoDB SQL tables in
`docs/research/action_outcome_vendor_sql_replay_experiment_010.md`. The next
step is to port that SQL/JOIN/window replay into a two-node live topology and
separate single-node-complete semantics from distributed planner gaps.
