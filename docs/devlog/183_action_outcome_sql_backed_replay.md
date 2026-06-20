# 183: Action-Outcome SQL-Backed Replay Research

Date: 2026-06-18
Status: In Progress

## Context

Action-Outcome Memory research had proven the context-gated replay logic against
JSON fixtures. The next step was to connect that work to ZeptoDB's SQL/time-series
positioning by defining SQL tables, generating seed rows, and replaying the same
experiment from a SQL-backed episode store.

## Changes

- Added Experiment 006 documentation for SQL-backed replay.
- Added `action_outcome_sql_replay.py`, a local SQL replay harness that emits
  ZeptoDB-compatible DDL/INSERT statements.
- The generated seed uses `STRING`, `INT64`, and `DOUBLE` columns, avoids the
  live parser keyword collision on `rank` by emitting `recommendation_rank`, and
  avoids `NULL` literals for ZeptoDB HTTP SQL compatibility.
- Materialized action-outcome episodes, episode metrics, retrieval quality
  labels, replay recommendations, and context-gate suppressions as SQL tables.
- Generated SQL-backed replay results and seed/result SQL under
  `docs/research/results/`.
- Updated the research process log with row counts, replay metrics, and the next
  live-ZeptoDB validation step.

## Verification

- `python3 docs/research/tools/action_outcome_sql_replay.py --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json --output docs/research/results/action_outcome_sql_replay_006.md --sql-output docs/research/results/action_outcome_sql_replay_006.sql`
- `python3 -m py_compile docs/research/tools/action_outcome_replay.py docs/research/tools/action_outcome_guardrail_compare.py docs/research/tools/action_outcome_ablation.py docs/research/tools/action_outcome_context_gate.py docs/research/tools/action_outcome_sql_replay.py docs/research/tools/validate_action_outcome_fixture.py`

## Result

The SQL-backed replay matched the JSON control for all six query episodes.
`context_gated` preserved top-3 hit rate at 1.00 and failed-action avoidance at
1.00, while materializing 21 gate suppression rows for audit.

## Follow-ups

- Live parser and row-count replay are covered by devlog 184.
- Add generic table INSERT projection support, or define a tick-shaped
  Action-Outcome projection, before promoting the schema into a design note.
