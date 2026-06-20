# 190: String-Key Hash JOIN Materialization

Date: 2026-06-18
Status: Complete

## Context

Experiment 009 recorded that native Action-Outcome string-key JOINs did not
return semantic string rows. The numeric projection proved the replay decision
surface could be checked with SQL JOIN/window operators, but the native schema
still needed hash JOIN support for declared `STRING` keys and joined string
result columns.

## Changes

- Added `ActionOutcomeSQLReplay.StringKeyHashJoinPreservesDecodedStringColumns`.
  - Creates two declared-schema tables with `STRING` join keys.
  - Inserts one matching and one non-matching Action-Outcome-style row.
  - Executes a hash JOIN over `r.query_id = e.episode_id`.
  - Asserts one matched row and decoded semantic `STRING` cells for query id,
    recommended action, and human outcome.

- Updated `QueryExecutor::exec_hash_join()`.
  - Reads join keys through `ColumnVector` type-aware access instead of
    casting all key buffers to `int64_t*`.
  - Preserves selected source column types in hash JOIN results.
  - Reads joined result cells through the same type-aware column accessor so
    `STRING`/`SYMBOL` dictionary codes are not corrupted.
  - Attaches the pipeline string dictionary to JOIN results for local and HTTP
    decoding.

- Updated Experiment 009 output and documentation.
  - Native string-key JOIN now returns semantic rows directly from the research
    schema.
  - Numeric projection remains for top-action and outcome-avoidance checks
    until alias-aware JOIN `WHERE` predicate handling is implemented.

## Verification

Build:

```bash
ninja -C build -j$(nproc) zepto_tests
ninja -C build -j$(nproc) zepto_http_server
```

Focused tests:

```bash
./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSQLReplay.StringKeyHashJoinPreservesDecodedStringColumns'

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSQLReplay.*:GenericInsertSQL.*:HashJoin*:*Join_Executes:*Join_ConcatenatesRows:*Join_HasColumns'
```

Live research harness:

```bash
python3 -m py_compile docs/research/tools/action_outcome_join_window_replay.py

python3 docs/research/tools/action_outcome_join_window_replay.py \
  --url http://127.0.0.1:19342/ \
  --output docs/research/results/action_outcome_join_window_replay_009.md \
  --timeout 10
```

Results:

- New string-key JOIN regression passed.
- 24 related JOIN/generic-insert tests passed.
- Experiment 009 passed seed row counts, native string window, numeric
  JOIN/window acceptance, and native string JOIN.

Full local C++ suite and aarch64 verification were not rerun in this step.

## Follow-ups

- Add alias-aware `WHERE` predicate handling for hash JOIN queries so native
  top-action JOIN checks can replace the numeric projection path.
- Port Experiment 009 into the two-node live harness after distributed
  JOIN/window planner limits are scoped.
