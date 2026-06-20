# 191: Action-Outcome Vendor Baseline Experiment

Date: 2026-06-18
Status: Complete

## Context

The industry scans showed that "action recommendation" and "closed-loop
remediation" are already commercially validated categories. The open research
question for ZeptoDB is narrower: whether a structured, time-series-native
Action-Outcome Memory layer with context gates can outperform simpler
vendor-inspired baselines on safety-oriented replay metrics.

## Changes

- Added `docs/research/tools/action_outcome_vendor_baseline.py`.
  - Compares `similar_incident`, `runbook_action_prior`,
    `reflection_only_memory`, and `context_gated_action_outcome`.
  - Reuses the existing noisy AIOps fixture and retrieval quality labels.
  - Records Top-3 hit rate, failed-action avoidance, labeled retrieval quality,
    top-action changes versus the context gate, and suppression count.

- Added `docs/research/results/action_outcome_vendor_baseline_010.md`.
  - Records Experiment 010 results.
  - Shows `context_gated_action_outcome` preserves Top-3 hit rate at 1.00 and
    improves failed-action avoidance to 1.00, versus 0.67 for
    `similar_incident` and `runbook_action_prior`, and 0.83 for
    `reflection_only_memory`.

- Added `docs/research/action_outcome_vendor_baseline_experiment_010.md`.
  - Documents purpose, baselines, command, results, interpretation, and next
    step.

- Updated research process, backlog, and completed-feature docs.

## Verification

```bash
python3 -m py_compile docs/research/tools/action_outcome_vendor_baseline.py

python3 docs/research/tools/action_outcome_vendor_baseline.py \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_vendor_baseline_010.md
```

Results:

- `similar_incident`: Top-3 hit 1.00, failed-action avoidance 0.67.
- `runbook_action_prior`: Top-3 hit 1.00, failed-action avoidance 0.67.
- `reflection_only_memory`: Top-3 hit 1.00, failed-action avoidance 0.83.
- `context_gated_action_outcome`: Top-3 hit 1.00, failed-action avoidance
  1.00, suppressions 21.

No C++ build was run because this change adds a Python research harness and
documentation only.

## Follow-ups

- Replay Experiment 010 through SQL tables after alias-aware hash JOIN `WHERE`
  predicate handling lands.
- Add an external/lab benchmark mapping, such as AIOpsLab-style incidents, once
  the fixture-level comparison is stable.
