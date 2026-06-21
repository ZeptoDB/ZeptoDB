# ActionOutcomeReplay Experiment 007 Live ZeptoDB SQL Endpoint Results

Generated at: 2026-06-18T02:44:50Z
Endpoint: `http://127.0.0.1:19023/`
SQL seed: `docs/research/results/action_outcome_sql_replay_006.sql`

## Load Summary

- Statements attempted: 203
- Statements succeeded: 203
- Statements failed: 0
- Load status: pass

## Row Count Verification

| Table | Expected Inserts | Live Rows | Status |
| --- | ---: | ---: | --- |
| action_outcome_episode_metrics | 101 | 101 | pass |
| action_outcome_episodes | 32 | 32 | pass |
| action_outcome_gate_suppressions | 21 | 21 | pass |
| action_outcome_replay_recommendations | 18 | 18 | pass |
| action_outcome_retrieval_quality_labels | 26 | 26 | pass |

## Semantic Query Verification

- Count status: pass
- Semantic status: pass
- Expected top-action rows: 6
- Live top-action rows: 6
- Failed-action avoidance rows from live WHERE query: 6

| Query | Expected Top Action | Live Top Action |
| --- | --- | --- |
| aoe_checkout_002 | rollback | rollback |
| aoe_payment_002 | traffic_drain | traffic_drain |
| aoe_inventory_002 | config_revert | config_revert |
| aoe_cache_002 | cache_purge | cache_purge |
| aoe_queue_003 | scale_out | scale_out |
| aoe_search_003 | rollback | rollback |

## Diagnostic Query

```json
{
  "columns": [
    "query_id",
    "action_class",
    "top_action",
    "recommendation_rank"
  ],
  "data": [
    [
      "aoe_checkout_002",
      "rollback",
      1,
      1
    ],
    [
      "aoe_checkout_002",
      "scale_out",
      0,
      2
    ],
    [
      "aoe_checkout_002",
      "config_revert",
      0,
      3
    ],
    [
      "aoe_payment_002",
      "traffic_drain",
      1,
      1
    ],
    [
      "aoe_payment_002",
      "restart",
      0,
      2
    ],
    [
      "aoe_payment_002",
      "scale_out",
      0,
      3
    ]
  ],
  "execution_time_us": 20.25,
  "rows": 6,
  "rows_scanned": 18
}
```

## Interpretation

Experiment 007 validates live parser, ingestion, projection, and WHERE
compatibility for the Action-Outcome SQL seed: all generated DDL/INSERT
statements execute through ZeptoDB's HTTP SQL endpoint, table row counts
match the local SQL control, and the value-level top-action replay query
returns the expected recommendations.

Generic table INSERT materialization is now sufficient for the
Action-Outcome replay contract. Declared `STRING`, `DOUBLE`/`FLOAT64`, and `INT64`
columns are queryable through the live SQL endpoint without reshaping the
research schema into tick-only fields.

## Next Steps

1. Keep this live report as the acceptance harness for future SQL ingest changes.
2. Add a distributed two-node replay run to exercise typed-row cluster routing.
3. Extend the replay contract with JOIN/window queries once the Action-Outcome schema grows.
