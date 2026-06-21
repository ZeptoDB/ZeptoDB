# 182: Action-Outcome Context Gate Research

Date: 2026-06-14
Status: In Progress

## Context

Action-Outcome Memory research needed a harder AIOps replay benchmark. The base
fixture was too clean, so action recommendations stayed successful even when
action-outcome evidence was removed. The next research step added noisy
same-family distractors and a context-conditioned gate for reusing historical
outcomes.

## Changes

- Added noisy distractor episodes and retrieval quality labels under
  `docs/research/fixtures/`.
- Extended the ablation script to support extra fixtures and labeled top-3
  retrieval quality.
- Added a context-gated comparison script that suppresses historical outcomes
  when blast radius, metric discriminators, or change context are incompatible.
- Updated the research process log with Experiment 004 and Experiment 005
  results.

## Verification

- `python3 -m json.tool docs/research/fixtures/action_outcome_distractor_episodes.json`
- `python3 -m json.tool docs/research/fixtures/action_outcome_retrieval_quality_labels.json`
- `python3 docs/research/tools/validate_action_outcome_fixture.py --output docs/research/results/action_outcome_fixture_validation_001.md`
- `python3 docs/research/tools/validate_action_outcome_fixture.py --fixture docs/research/fixtures/action_outcome_distractor_episodes.json --min-family-size 1 --output docs/research/results/action_outcome_distractor_validation_004.md`
- `python3 docs/research/tools/action_outcome_ablation.py --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json --output docs/research/results/action_outcome_ablation_004_noisy.md`
- `python3 docs/research/tools/action_outcome_context_gate.py --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json --output docs/research/results/action_outcome_context_gate_005.md`
- `python3 -m py_compile docs/research/tools/action_outcome_replay.py docs/research/tools/action_outcome_guardrail_compare.py docs/research/tools/action_outcome_ablation.py docs/research/tools/action_outcome_context_gate.py docs/research/tools/validate_action_outcome_fixture.py`

## Result

On the noisy fixture, `context_gated` preserved top-3 hit rate at 1.00 and
improved failed-action avoidance from 0.67 to 1.00 versus `full_guarded`.

## Follow-ups

- Map action-outcome episodes into ZeptoDB SQL tables and replay Experiment 005
  through database-backed retrieval.
- Add calibrated or learned context gates after collecting real incident traces.
