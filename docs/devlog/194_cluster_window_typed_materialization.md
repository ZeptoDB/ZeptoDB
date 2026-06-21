# 194: Cluster-Mode Window Typed Materialization

Date: 2026-06-20
Status: Complete

## Context

Experiment 011 showed that cluster-mode `ROW_NUMBER` and `LAG` over the
declared operational table `action_outcome_vendor_recommendations_010`
returned the expected row count but zeroed projected value columns. A plain
distributed projection over the same table returned correct values, so the bug
was isolated to the coordinator's full-data window replay path.

The coordinator fetched `SELECT *` from all nodes and replayed the merged rows
into a temporary local pipeline before executing the original window query.
That replay only used legacy `TickMessage` fields (`symbol`, `price`, `volume`,
`timestamp`), which erased declared generic-table columns such as `group_id`,
`recommendation_rank`, and `score_micros`.

## Changes

- `src/cluster/query_coordinator.cpp`
  - Added schema-aware temporary-table materialization for full-data
    distributed window/FIRST/LAST/CTE replay.
  - Reuses local schema snapshots when available, otherwise infers a temporary
    schema from the merged `SELECT *` result metadata.
  - Replays rows through `TypedRowMessage` so declared generic-table integer,
    timestamp, float, bool, `STRING`, and `SYMBOL` cells are preserved.
  - Keeps the legacy tick-shaped fallback only when no table schema can be
    built.
- `tests/unit/test_distributed_insert.cpp`
  - Added `DistributedInsert.ClusterWindowMaterializesGenericTableValues`,
    a two-pipeline TCP/RPC regression that routes declared Action-Outcome rows
    to a remote owner and verifies cluster `ROW_NUMBER`/`LAG` preserve value
    columns.
- `docs/research/tools/action_outcome_distributed_vendor_sql_replay.py`
  - Updated the report interpretation so Experiment 011 records window checks
    as pass when values match.

## Verification

```bash
ninja -C build -j$(nproc) zepto_tests

./build/tests/zepto_tests \
  --gtest_filter='QueryCoordinator.TwoNodeRemote_DistributedWindowFunction:QueryCoordinator.TwoNodeRemote_DistributedFirstLast:QueryCoordinator.TwoNodeRemote_DistributedDistinct:DistributedInsert.ClusterWindowMaterializesGenericTableValues'

ninja -C build -j$(nproc) zepto_http_server

python3 docs/research/tools/action_outcome_distributed_vendor_sql_replay.py \
  --coordinator-url http://127.0.0.1:19241/ \
  --node-a-id 1 \
  --node-b-id 8 \
  --node-a-stats-url http://127.0.0.1:19241/stats \
  --node-b-stats-url http://127.0.0.1:19242/stats \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_distributed_vendor_sql_replay_011.md \
  --timeout 10
```

Results:

- Focused C++ regression set: 4/4 pass.
- Experiment 011 seed statements: 203/203 succeeded.
- Experiment 011 `row_number_window`: pass, 72/72 rows.
- Experiment 011 `lag_window`: pass, 72/72 rows.
- Remaining Experiment 011 boundary: `suppression_join` is still
  `expected_gap_cross_node_join`.

## Follow-ups

- Implement small-table distributed hash JOIN for operational tables, starting
  with broadcast/replicated dimension-table joins for the suppression table.
