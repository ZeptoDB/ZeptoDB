# 186: Action-Outcome Generic INSERT Materialization

Date: 2026-06-18
Status: Complete

## Context

Experiment 007 showed that ZeptoDB accepted the Action-Outcome SQL seed and
matched table row counts, but SQL `INSERT` only materialized tick fields. Wider
declared table columns were not queryable through projection or `WHERE`
semantics, blocking live value-level replay.

## Changes

- `src/sql/executor.cpp`
  - Added schema-aware `INSERT ... VALUES` materialization for tables registered
    in `SchemaRegistry`.
  - Converts `INT32`, `INT64`, `TIMESTAMP_NS`, `FLOAT32`, `FLOAT64`, `SYMBOL`,
    `STRING`, and `BOOL` literals into `TypedColumnValue` instances.
  - Preserves the legacy tick-shaped path for tables without a matching
    `CREATE TABLE`.
  - Uses inserted `symbol` and `timestamp`/`timestamp_ns` columns for partition
    routing when present, otherwise falls back to symbol `0` and current time.
- `src/core/pipeline.cpp`
  - Allows schema-aware typed rows without a symbol column by permitting
    `symbol_id = 0`.
  - Registers newly created typed-row partitions in `partition_index_` so
    `total_stored_rows()` and direct symbol/table accounting stay consistent.
- `include/zeptodb/cluster/cluster_node_base.h`
  - Added the virtual `ingest_typed_row()` route used by schema-aware SQL
    INSERT.
- `include/zeptodb/cluster/cluster_node.h`
  - Routes typed rows through the same owner and migration logic as tick ingest,
    using TCP RPC `TYPED_ROW_INGEST` for remote owners.
- `tests/unit/test_sql.cpp`
  - Promoted the Action-Outcome regression to green.
  - Added schema-order, `STRING`/`FLOAT64`/`BOOL`, epoch-zero timestamp, and
    `INT32` overflow rejection coverage.
- `docs/research/tools/action_outcome_live_sql_replay.py`
  - Updated the report interpretation for passing semantic replay.
  - Returns non-zero when row counts or semantic replay checks fail.

## Verification

- `cd build && ninja -j$(nproc) zepto_tests`
- `cd build && ninja -j$(nproc) zepto_http_server`
- `cd build && ./tests/zepto_tests --gtest_filter='ActionOutcomeSQLReplay.*:GenericInsertSQL.*:CatalogSQL.ShowTablesRowCountAfterInsert:SqlExecutorTest.SpatialDistanceAndWithinPredicates'`
- `cd build && ./tests/zepto_tests --gtest_brief=1 --gtest_filter='*SQL*:*Sql*:*DDL*:*DML*:*TableScoped*:*Catalog*'`
- `python3 docs/research/tools/action_outcome_live_sql_replay.py --url http://127.0.0.1:19023/ --output docs/research/results/action_outcome_live_sql_replay_007.md`

Live Experiment 007 result:

- Statements attempted: 203
- Statements succeeded: 203
- Row-count verification: pass
- Semantic top-action replay: pass
- Failed-action avoidance rows: 6

aarch64 verification was not run in this local pass.

## Follow-ups

- Add a distributed two-node Action-Outcome live replay to exercise typed-row
  cluster routing.
- Extend the replay acceptance contract with JOIN/window queries over
  recommendations and episode metadata.
