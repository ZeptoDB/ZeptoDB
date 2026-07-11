# ActionOutcomeReplay Experiment 012 Operational Placement Results

Generated at: 2026-06-21T10:56:16Z
Coordinator endpoint: `http://127.0.0.1:19241/`
SQL seed: `docs/research/results/action_outcome_sql_replay_006.sql`
Node IDs: `1`, `8`

## Status

- Explicit placement policy status: pass
- Seed row-count status: pass
- Vendor table row-count status: pass
- Distributed ingest status: pass
- Full SQL/JOIN/window status: pass
- Small-table JOIN telemetry status: pass
- Prometheus telemetry status: pass
- Overall Experiment 012 status: pass

## Governance Classification

Classification: experimental runtime path.

This result validates the bounded Action-Outcome operational-table placement
and JOIN replay hypothesis. It does not promote runtime table placement,
bounded small-table JOIN, or full-data window materialization into broad product
contracts. Devlog 217 adds persisted placement metadata and coordinator
restart re-apply. Promotion still requires rebalance semantics, explicit
limits, and operator guidance as defined in
`docs/research/EXPERIMENT_GOVERNANCE.md`.

## Placement Policy

| Table | Policy | Node | Purpose | API Status |
| --- | --- | ---: | --- | --- |
| `action_outcome_vendor_queries_010` | `pinned_node` | 8 | query controls co-located with recommendations | `200` / `{"node_id": 8, "ok": true, "policy": "pinned_node", "table": "action_outcome_vendor_queries_010"}` |
| `action_outcome_vendor_recommendations_010` | `pinned_node` | 8 | action priors and recommendations | `200` / `{"node_id": 8, "ok": true, "policy": "pinned_node", "table": "action_outcome_vendor_recommendations_010"}` |
| `action_outcome_vendor_retrieval_010` | `pinned_node` | 8 | retrieval evidence | `200` / `{"node_id": 8, "ok": true, "policy": "pinned_node", "table": "action_outcome_vendor_retrieval_010"}` |
| `action_outcome_vendor_suppressions_010` | `pinned_node` | 1 | bounded suppression/control table | `200` / `{"node_id": 1, "ok": true, "policy": "pinned_node", "table": "action_outcome_vendor_suppressions_010"}` |

## Node-Local Stats Delta

| Node ID | Stats URL | ticks_ingested | ticks_stored | ticks_dropped | partitions_created |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | `http://127.0.0.1:19241/stats` | 161 | 161 | 0 | 4 |
| 8 | `http://127.0.0.1:19242/stats` | 208 | 208 | 0 | 5 |

## Owner Map After Policy

| Table | Stable table_id | Owner node | Rows |
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

## JOIN/Window Checks

| Check | Left/table owner | Right owner | Rows | Expected | Status |
| --- | --- | --- | ---: | ---: | --- |
| `failed_repeat_join` | `action_outcome_vendor_recommendations_010` -> 8 | `action_outcome_vendor_queries_010` -> 8 | 5 | 5 | `pass` |
| `context_top_action_join` | `action_outcome_vendor_recommendations_010` -> 8 | `action_outcome_vendor_queries_010` -> 8 | 6 | 6 | `pass` |
| `suppression_join` | `action_outcome_vendor_suppressions_010` -> 1 | `action_outcome_vendor_recommendations_010` -> 8 | 21 | 21 | `pass` |
| `misleading_retrieval_join` | `action_outcome_vendor_retrieval_010` -> 8 | `action_outcome_vendor_queries_010` -> 8 | 23 | 23 | `pass` |
| `row_number_window` | `action_outcome_vendor_recommendations_010` -> 8 | n/a | 72 | 72 | `pass` |
| `lag_window` | `action_outcome_vendor_recommendations_010` -> 8 | n/a | 72 | 72 | `pass` |

## Small-Table JOIN Telemetry

| Counter/Gauge | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `accepted` | 0 | 4 | 4 |
| `candidates` | 0 | 4 | 4 |
| `errors` | 0 | 0 | 0 |
| `last_left_rows` | 0 | 72 | 72 |
| `last_right_rows` | 0 | 6 | 6 |
| `rejected_row_cap` | 0 | 0 | 0 |
| `rows_materialized` | 0 | 327 | 327 |

## Prometheus Metrics

| Metric | Value |
| --- | ---: |
| `zepto_small_table_join_candidates_total` | 4 |
| `zepto_small_table_join_accepted_total` | 4 |
| `zepto_small_table_join_row_cap_rejections_total` | 0 |
| `zepto_small_table_join_errors_total` | 0 |
| `zepto_small_table_join_rows_materialized_total` | 327 |
| `zepto_small_table_join_last_left_rows` | 72 |
| `zepto_small_table_join_last_right_rows` | 6 |

## Interpretation

Experiment 012 turns operational-table distribution from an implicit
table-id hash side effect into an explicit control-plane policy. The
Action-Outcome query, recommendation, and retrieval tables are pinned to
node 8 while the bounded suppression/control table is pinned to node 1.
This deliberately keeps the suppression JOIN cross-node while preserving
correctness through the bounded small-table hash JOIN path.

The commercial value is observability plus a narrow guarantee: operators
can place small Action-Outcome control tables intentionally, then verify
that coordinator-local JOIN replay stayed inside the row cap and did not
silently fall back to an unbounded distributed planner.

## Next Best Step

Devlog 217 promotes placement policy from an admin-only runtime knob to a
persisted table option/catalog record. The next best step is a row-cap
alerting example for `zepto_small_table_join_row_cap_rejections_total`.
