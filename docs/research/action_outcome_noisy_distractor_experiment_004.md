# Action-Outcome Noisy Distractor Experiment 004

Date: 2026-06-13

## Purpose

Experiment 003 showed that the base fixture is too clean. Removing the
`action_outcome` signal changed one top action after the ablation correction,
but headline metrics still stayed perfect. This makes the core research claim
too weak: simple family, symptom, topology, and change matching can still recover
good recommendations.

Experiment 004 adds noisy distractor episodes and retrieval quality labels. The
goal is to test whether Action-Outcome Memory can separate useful historical
evidence from superficially similar evidence.

## Hypothesis

When distractors share the same service, symptom keywords, action class, and
time-series shape, variants without action-outcome evidence should retrieve more
misleading candidates and should change top actions more often than the full
guarded engine.

## Inputs

- Base fixture: `docs/research/fixtures/action_outcome_episodes.json`
- Distractor fixture:
  `docs/research/fixtures/action_outcome_distractor_episodes.json`
- Retrieval quality labels:
  `docs/research/fixtures/action_outcome_retrieval_quality_labels.json`

## Evaluation

Run the ablation report with the base fixture plus distractor fixture:

```bash
python3 docs/research/tools/action_outcome_ablation.py \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_ablation_004_noisy.md
```

Metrics:

- Top-3 successful action hit rate.
- Failed-action avoidance rate.
- Cross-family and weak cross-family top-3 retrieval count.
- Top-action changes versus `full_guarded`.
- Labeled top-3 retrieval quality counts:
  - `useful`: candidate contains evidence that should improve the decision.
  - `superficial`: candidate is similar but does not materially change the
    action decision.
  - `misleading`: candidate could push the engine toward a known bad or
    contextually wrong action.

## Success Criteria

The experiment is useful if at least one ablation variant shows a measurable
increase in misleading retrievals, top-action changes, or failed-action repeats.

The experiment is not sufficient if every variant remains perfect on all
headline metrics and labeled retrieval quality. In that case the next step is to
add stronger distractors or replay against public/lab-generated incident traces.

## Expected Interpretation

`full_guarded` should not be judged only by whether it returns a known successful
action. The higher-value signal is whether it avoids misleading evidence when a
similar action had different outcomes under a subtly different root cause.

## Next Best Step

After this experiment, map the fixture into SQL tables so the same comparison can
run through ZeptoDB-backed retrieval instead of in-memory Python fixtures.
