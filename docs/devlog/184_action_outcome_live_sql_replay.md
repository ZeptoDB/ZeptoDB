# Devlog 184 - Action-Outcome Live SQL Replay

Date: 2026-06-18
Status: In Progress

## Summary

Validated the Action-Outcome SQL seed against a live ZeptoDB HTTP SQL endpoint.
The live endpoint accepts the generated DDL/INSERT stream and preserves table row
counts, but value-level semantic replay is blocked by the current tick-oriented
SQL INSERT path.

## Changes

- Added `docs/research/tools/action_outcome_live_sql_replay.py`.
- Added `docs/research/action_outcome_live_sql_replay_experiment_007.md`.
- Generated `docs/research/results/action_outcome_live_sql_replay_007.md`.
- Updated the SQL replay seed generator to emit live-compatible column names,
  string types, and non-NULL literal values.
- Documented the current tick-oriented SQL INSERT materialization limit in
  `docs/api/SQL_REFERENCE.md`.

## Results

- Live statements attempted: 203.
- Live statements succeeded: 203.
- Live statements failed: 0.
- Live row counts matched the local SQL control:
  - `action_outcome_episodes`: 32.
  - `action_outcome_episode_metrics`: 101.
  - `action_outcome_retrieval_quality_labels`: 26.
  - `action_outcome_replay_recommendations`: 18.
  - `action_outcome_gate_suppressions`: 21.
- Top-action semantic query remained blocked:
  - expected rows: 6.
  - live rows: 0.

## Interpretation

The live SQL endpoint currently maps INSERT rows into `TickMessage` fields
(`symbol`, `price`, `volume`, `timestamp`). Arbitrary declared columns are enough
to scope rows and row counts by table id, but they are not materialized for
projection or WHERE semantics. Action-Outcome Memory therefore needs generic
table INSERT materialization, or a deliberately tick-shaped projection schema,
before live value-level replay can match the local SQL control.

## Verification

- `python3 docs/research/tools/action_outcome_sql_replay.py --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json --output docs/research/results/action_outcome_sql_replay_006.md --sql-output docs/research/results/action_outcome_sql_replay_006.sql`
- `python3 docs/research/tools/action_outcome_live_sql_replay.py --url http://127.0.0.1:19125/ --sql-file docs/research/results/action_outcome_sql_replay_006.sql --output docs/research/results/action_outcome_live_sql_replay_007.md`
- `python3 -m py_compile docs/research/tools/action_outcome_live_sql_replay.py docs/research/tools/action_outcome_sql_replay.py`

## Next

The failing C++ regression test is tracked in devlog 185. Next, implement
generic table INSERT support for declared schema columns, then rerun Experiment
007 and promote the schema into a design note.
