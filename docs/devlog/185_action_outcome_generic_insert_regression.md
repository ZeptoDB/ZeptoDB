# Devlog 185 - Action-Outcome Generic INSERT Regression Test

Date: 2026-06-18
Status: Complete

## Summary

Added an active red C++ regression test for arbitrary CREATE TABLE + INSERT +
SELECT projection. The test captures the live Experiment 007 blocker: SQL INSERT
counts rows for wider schemas, but non-tick columns are not materialized for
projection or WHERE semantics.

## Changes

- Added `ActionOutcomeSQLReplay.GenericInsertProjectionMaterializesDeclaredColumns`
  in `tests/unit/test_sql.cpp`.
- The test creates an Action-Outcome projection table with declared `STRING` and
  `INT64` columns, verifies empty-table count, inserts one replay recommendation
  row, verifies table row count, and then expects `WHERE top_action = 1` to
  return the inserted `score_micros`.

## Initial Result

The regression is intentionally failing until generic SQL INSERT materialization
is implemented:

```text
Expected equality of these values:
  replay.rows.size()
    Which is: 0
  1u
    Which is: 1
```

## Verification

- `cd build && ninja -j$(nproc) zepto_tests`
- `cd build && ./tests/zepto_tests --gtest_filter='ActionOutcomeSQLReplay.GenericInsertProjectionMaterializesDeclaredColumns'`

The build succeeded. The filtered test failed as expected before devlog 186.

## Resolution

Devlog 186 implements generic table INSERT materialization for declared schema
columns. The regression now passes and Experiment 007 live replay returns the
expected top-action rows.

## Next

Use the live Experiment 007 report as the acceptance harness for future SQL
ingest changes, then add a distributed typed-row replay run.
