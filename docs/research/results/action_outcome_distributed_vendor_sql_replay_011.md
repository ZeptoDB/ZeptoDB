# ActionOutcomeReplay Experiment 011 Distributed SQL/JOIN/Window Replay Results

Generated at: 2026-06-20T02:21:32Z
Coordinator endpoint: `http://127.0.0.1:19241/`
SQL seed: `docs/research/results/action_outcome_sql_replay_006.sql`
Node IDs: `1`, `8`

## Load Summary

- Seed statements attempted: 203
- Seed statements succeeded: 203
- Seed statements failed: 0
- Seed row-count status: pass
- Vendor table row-count status: pass
- Distributed ingest status: pass
- Full distributed SQL/JOIN/window status: partial
- Boundary classification status: pass

## Node-Local Stats Delta

| Node ID | Stats URL | ticks_ingested | ticks_stored | ticks_dropped | partitions_created |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | `http://127.0.0.1:19241/stats` | 161 | 161 | 0 | 4 |
| 8 | `http://127.0.0.1:19242/stats` | 208 | 208 | 0 | 5 |

## Owner Split

| Table | Stable table_id | Owner node | Expected/live rows |
| --- | ---: | ---: | ---: |
| `action_outcome_episode_metrics` | 57172 | 1 | 101 |
| `action_outcome_episodes` | 3679 | 8 | 32 |
| `action_outcome_gate_suppressions` | 16692 | 1 | 21 |
| `action_outcome_replay_recommendations` | 9749 | 1 | 18 |
| `action_outcome_retrieval_quality_labels` | 37786 | 8 | 26 |
| `action_outcome_vendor_queries_010` | 58879 | 8 | 6 |
| `action_outcome_vendor_recommendations_010` | 32465 | 8 | 72 |
| `action_outcome_vendor_retrieval_010` | 52359 | 8 | 72 |
| `action_outcome_vendor_suppressions_010` | 58120 | 1 | 21 |

## Row Count Verification

### Seed Tables

| Table | Expected Inserts | Live Rows | Status |
| --- | ---: | ---: | --- |
| `action_outcome_episode_metrics` | 101 | 101 | pass |
| `action_outcome_episodes` | 32 | 32 | pass |
| `action_outcome_gate_suppressions` | 21 | 21 | pass |
| `action_outcome_replay_recommendations` | 18 | 18 | pass |
| `action_outcome_retrieval_quality_labels` | 26 | 26 | pass |

### Vendor Tables

| Table | Expected Rows | Live Rows | Status |
| --- | ---: | ---: | --- |
| `action_outcome_vendor_queries_010` | 6 | 6 | pass |
| `action_outcome_vendor_recommendations_010` | 72 | 72 | pass |
| `action_outcome_vendor_retrieval_010` | 72 | 72 | pass |
| `action_outcome_vendor_suppressions_010` | 21 | 21 | pass |

## Distributed JOIN/Window Classification

| Check | Left/table owner | Right owner | Rows | Expected | Status |
| --- | --- | --- | ---: | ---: | --- |
| `failed_repeat_join` | `action_outcome_vendor_recommendations_010` -> 8 | `action_outcome_vendor_queries_010` -> 8 | 5 | 5 | `pass` |
| `context_top_action_join` | `action_outcome_vendor_recommendations_010` -> 8 | `action_outcome_vendor_queries_010` -> 8 | 6 | 6 | `pass` |
| `suppression_join` | `action_outcome_vendor_suppressions_010` -> 1 | `action_outcome_vendor_recommendations_010` -> 8 | 0 | 21 | `expected_gap_cross_node_join` |
| `misleading_retrieval_join` | `action_outcome_vendor_retrieval_010` -> 8 | `action_outcome_vendor_queries_010` -> 8 | 23 | 23 | `pass` |
| `row_number_window` | `action_outcome_vendor_recommendations_010` -> 8 | n/a | 72 | 72 | `pass` |
| `lag_window` | `action_outcome_vendor_recommendations_010` -> 8 | n/a | 72 | 72 | `pass` |

## Interpretation

Experiment 011 separates distributed-safe replay checks from the current
distributed planner boundary. Under the default 1/8 two-node ring, the
query, recommendation, and retrieval vendor tables are co-located on node
8, so failed-repeat JOIN, context top-action JOIN, and misleading
retrieval JOIN all pass through the coordinator.

The suppression table is owned by node 1 while recommendations are owned by
node 8. The suppression JOIN therefore records the expected current gap:
ZeptoDB can scatter-gather supported SELECT and window shapes, but it does
not yet execute arbitrary cross-node hash JOINs by shipping one side or
broadcasting small dimension tables.

The window checks now pass in cluster mode. The coordinator
fetch-and-compute path preserves declared operational-table values
when it materializes the temporary full-data table, so ROW_NUMBER
and LAG match the single-node/vendor baseline.

## Next Best Step

Implement a narrow distributed hash JOIN strategy for small
operational tables. The JOIN work can start with
broadcast/replicated dimension-table joins, which would turn the
suppression JOIN from `expected_gap_cross_node_join` into `pass`
without requiring a full cost-based distributed SQL optimizer.
