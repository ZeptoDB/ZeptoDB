# 195: Small-Table Distributed Hash JOIN

Date: 2026-06-21
Status: Complete

## Context

Experiment 011 had one remaining strict SQL replay gap after cluster-mode
window materialization was fixed: `action_outcome_vendor_suppressions_010`
is owned by node 1, while `action_outcome_vendor_recommendations_010` is owned
by node 8. Co-located vendor JOINs passed, but the cross-node suppression JOIN
returned zero rows because the coordinator only scatter-gathered the original
JOIN SQL to each node.

The goal was not to build a full distributed SQL optimizer. It was to add the
smallest product-shaped path for bounded operational tables such as runbooks,
recommendations, suppressions, and query controls.

## Changes

- `src/cluster/query_coordinator.cpp`
  - Added a Tier A-3 small-table hash JOIN path for `INNER`, `LEFT`, `RIGHT`,
    and `FULL` equi JOINs.
  - Fetches both base tables with a strict coordinator row cap, concatenates
    node results, materializes declared schemas into a temporary typed
    `ZeptoPipeline`, and executes the original JOIN SQL locally.
  - Preserves decoded `STRING`/`SYMBOL` values from local and remote result
    sets before temporary materialization and before returning temp-pipeline
    results.
- `include/zeptodb/cluster/query_coordinator.h`
  - Documented the bounded coordinator-local hash JOIN helper and schema
    snapshot helper.
- `tests/unit/test_distributed_insert.cpp`
  - Added `DistributedInsert.SmallTableBroadcastJoinMaterializesCrossNodeOperationalTables`.
  - The regression uses the Experiment 011 table names so suppressions route
    to node 1 and recommendations route to node 8, then verifies the
    coordinator returns the expected joined suppression row with decoded
    strings.
  - Added `DistributedInsert.SmallTableBroadcastJoinRejectsRowsOverLimit` to
    verify the bounded path rejects tables that exceed the coordinator
    small-table row cap.
- `docs/research/tools/action_outcome_distributed_vendor_sql_replay.py`
  - Updated the generated Experiment 011 interpretation and next step text for
    the new passing suppression JOIN state.
- Documentation updated:
  - `docs/research/results/action_outcome_distributed_vendor_sql_replay_011.md`
  - `docs/research/action_outcome_distributed_vendor_sql_replay_experiment_011.md`
  - `docs/design/phase_c_distributed.md`
  - `docs/api/SQL_REFERENCE.md`
  - `docs/COMPLETED.md`
  - `docs/BACKLOG.md`
  - `docs/research/action_outcome_research_process_log.md`

## Verification

```bash
ninja -C build -j$(nproc) zepto_tests

./build/tests/zepto_tests \
  --gtest_filter='QueryCoordinator.TwoNodeRemote_DistributedWindowFunction:QueryCoordinator.TwoNodeRemote_DistributedFirstLast:QueryCoordinator.TwoNodeRemote_DistributedDistinct:QueryCoordinator.TwoNodeRemote_DistributedAvg_Correct:QueryCoordinator.TwoNodeRemote_DistributedAvg_MixedAggs:QueryCoordinator.TwoNodeRemote_GroupBy_CrossNode_XbarMerge:QueryCoordinator.TwoNodeRemote_DistributedVwap:QueryCoordinator.TwoNodeRemote_OrderByLimit:QueryCoordinator.TwoNodeRemote_DistributedHaving:QueryCoordinator.TwoNodeRemote_MultiColumnOrderBy:DistributedInsert.ClusterWindowMaterializesGenericTableValues:DistributedInsert.SmallTableBroadcastJoinMaterializesCrossNodeOperationalTables:DistributedInsert.SmallTableBroadcastJoinRejectsRowsOverLimit'

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
  --timeout 10 \
  --strict-full-sql
```

Result summary:

| Check | Status |
| --- | --- |
| Focused C++ regression set | 13/13 pass |
| Seed statements | 203/203 succeeded |
| Seed row counts | pass |
| Vendor table counts | pass |
| Distributed ingest | pass |
| Failed-repeat JOIN | pass |
| Context top-action JOIN | pass |
| Suppression JOIN | pass, 21/21 |
| Misleading retrieval JOIN | pass |
| ROW_NUMBER | pass |
| LAG | pass |
| Strict full distributed SQL/JOIN/window | pass |

## Follow-ups

- Define a shard-key or table-level distribution policy for symbol-less
  operational tables before promoting Action-Outcome replay beyond research.
- Add telemetry for small-table JOIN row-cap hits and fallback/error rates.
- Keep broad, large-table cross-node hash JOINs on the cost-based distributed
  planner roadmap.
