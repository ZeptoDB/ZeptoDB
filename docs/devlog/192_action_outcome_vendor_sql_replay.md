# 192: Action-Outcome Vendor SQL Replay

Date: 2026-06-18
Status: Complete

## Context

Experiment 010 proved that context-gated Action-Outcome Memory outperforms
vendor-inspired baselines on failed-action avoidance, but the first comparison
was Python-only. To make the result useful as a ZeptoDB product proof, the same
comparison needs to be materialized into SQL tables and audited through native
JOIN/window queries.

Experiment 009 also left one engine gap: hash JOIN result filtering did not
apply alias-qualified `WHERE` predicates such as `r.top_action = 1 AND
e.human_outcome = 'failure'`.

## Changes

- Updated `src/sql/executor.cpp`.
  - Hash JOIN result assembly now evaluates post-join `WHERE` predicates over
    the joined left/right row cells.
  - Predicate resolution supports table aliases from both sides of the JOIN.
  - Declared `STRING` and `SYMBOL` values are decoded before string comparisons
    and `LIKE`; numeric `IN` and `BETWEEN` predicates keep the existing
    integer-value semantics.

- Updated `tests/unit/test_sql.cpp`.
  - Strengthened the Action-Outcome string-key JOIN regression with both
    left-side numeric alias filters and right-side string alias filters.
  - The test now catches the previous failure mode where aliased `WHERE`
    predicates were not applied after hash JOIN planning.

- Added `docs/research/tools/action_outcome_vendor_sql_replay.py`.
  - Loads the existing Action-Outcome SQL seed into a live ZeptoDB HTTP server.
  - Materializes Experiment 010 query, recommendation, retrieval, and
    suppression tables.
  - Validates failed-repeat JOIN, context top-action JOIN, suppression JOIN,
    misleading retrieval JOIN, ROW_NUMBER, and LAG checks.

- Added `docs/research/results/action_outcome_vendor_sql_replay_010.md`.
  - Records the live SQL/JOIN/window replay result.

- Added `docs/research/action_outcome_vendor_sql_replay_experiment_010.md`.
  - Documents the replay purpose, command, materialized tables, acceptance
    checks, result summary, interpretation, and next step.

- Updated SQL reference, backlog, completed-feature docs, and research process
  log.

## Verification

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

Results:

- Focused SQL/JOIN regression suite: 24/24 pass.
- Live Experiment 010 SQL/JOIN/window replay: pass.
- Seed statements: 203/203 succeeded.
- Vendor table counts: 6 query rows, 72 recommendation rows, 72 retrieval rows,
  and 21 suppression rows.
- Acceptance checks: failed-repeat JOIN, context top-action JOIN, suppression
  JOIN, misleading retrieval JOIN, ROW_NUMBER, and LAG all pass.

## Follow-ups

- Port Experiment 010 SQL/JOIN/window replay into the two-node live harness and
  classify distributed-safe checks versus distributed JOIN/window planner gaps.
