# ActionOutcomeReplay Experiment 009 JOIN/Window Live Acceptance Results

Generated at: 2026-06-18T07:55:36Z
Endpoint: `http://127.0.0.1:19342/`
SQL seed: `docs/research/results/action_outcome_sql_replay_006.sql`

## Load Summary

- Seed statements attempted: 203
- Seed statements succeeded: 203
- Seed statements failed: 0
- Seed row-count status: pass
- Projection row-count status: pass
- Native string window status: pass
- Numeric JOIN/window acceptance status: pass
- Native string JOIN status: pass

## Seed Row Counts

| Table | Expected Inserts | Live Rows | Status |
| --- | ---: | ---: | --- |
| `action_outcome_episode_metrics` | 101 | 101 | pass |
| `action_outcome_episodes` | 32 | 32 | pass |
| `action_outcome_gate_suppressions` | 21 | 21 | pass |
| `action_outcome_replay_recommendations` | 18 | 18 | pass |
| `action_outcome_retrieval_quality_labels` | 26 | 26 | pass |

## Acceptance Projection

| Table | Expected Rows | Live Rows | Status |
| --- | ---: | ---: | --- |
| `action_outcome_acceptance_queries_009` | 6 | 6 | pass |
| `action_outcome_acceptance_recommendations_009` | 18 | 18 | pass |

Action codes:

| Code | Action |
| ---: | --- |
| 1 | `rollback` |
| 2 | `scale_out` |
| 3 | `config_revert` |
| 4 | `traffic_drain` |
| 5 | `cache_purge` |
| 6 | `restart` |

## Query-Level Control Rows

| Seq | Query | Observed Episode Action | Expected Top Action | Human Failure Flag |
| ---: | --- | --- | --- | ---: |
| 1 | `aoe_checkout_002` | `restart` (6) | `rollback` (1) | 1 |
| 2 | `aoe_payment_002` | `scale_out` (2) | `traffic_drain` (4) | 1 |
| 3 | `aoe_inventory_002` | `scale_out` (2) | `config_revert` (3) | 0 |
| 4 | `aoe_cache_002` | `restart` (6) | `cache_purge` (5) | 1 |
| 5 | `aoe_queue_003` | `restart` (6) | `scale_out` (2) | 1 |
| 6 | `aoe_search_003` | `scale_out` (2) | `rollback` (1) | 1 |

## Native String JOIN Boundary

The direct research-shape JOIN checks whether string join keys and
joined string result columns are preserved by the hash join executor.

```json
{
  "columns": [
    "query_id",
    "recommendation_rank",
    "action_class",
    "action_class",
    "human_outcome"
  ],
  "data": [
    [
      "aoe_checkout_002",
      1,
      "rollback",
      "restart",
      "failure"
    ],
    [
      "aoe_checkout_002",
      2,
      "scale_out",
      "restart",
      "failure"
    ],
    [
      "aoe_checkout_002",
      3,
      "config_revert",
      "restart",
      "failure"
    ],
    [
      "aoe_payment_002",
      1,
      "traffic_drain",
      "scale_out",
      "failure"
    ],
    [
      "aoe_payment_002",
      2,
      "restart",
      "scale_out",
      "failure"
    ],
    [
      "aoe_payment_002",
      3,
      "scale_out",
      "scale_out",
      "failure"
    ],
    [
      "aoe_inventory_002",
      1,
      "config_revert",
      "scale_out",
      "rollback_required"
    ],
    [
      "aoe_inventory_002",
      2,
      "traffic_drain",
      "scale_out",
      "rollback_required"
    ],
    [
      "aoe_inventory_002",
      3,
      "scale_out",
      "scale_out",
      "rollback_required"
    ],
    [
      "aoe_cache_002",
      1,
      "cache_purge",
      "restart",
      "failure"
    ],
    [
      "aoe_cache_002",
      2,
      "restart",
      "restart",
      "failure"
    ],
    [
      "aoe_cache_002",
      3,
      "rollback",
      "restart",
      "failure"
    ],
    [
      "aoe_queue_003",
      1,
      "scale_out",
      "restart",
      "failure"
    ],
    [
      "aoe_queue_003",
      2,
      "traffic_drain",
      "restart",
      "failure"
    ],
    [
      "aoe_queue_003",
      3,
      "restart",
      "restart",
      "failure"
    ],
    [
      "aoe_search_003",
      1,
      "rollback",
      "scale_out",
      "failure"
    ],
    [
      "aoe_search_003",
      2,
      "restart",
      "scale_out",
      "failure"
    ],
    [
      "aoe_search_003",
      3,
      "traffic_drain",
      "scale_out",
      "failure"
    ]
  ],
  "execution_time_us": 547.5,
  "rows": 18,
  "rows_scanned": 50
}
```

- Native string JOIN status: pass

## Native String Window

```json
{
  "columns": [
    "query_id",
    "recommendation_rank",
    "rank_check"
  ],
  "data": [
    [
      "aoe_checkout_002",
      1,
      1
    ],
    [
      "aoe_checkout_002",
      2,
      2
    ],
    [
      "aoe_checkout_002",
      3,
      3
    ],
    [
      "aoe_payment_002",
      1,
      1
    ],
    [
      "aoe_payment_002",
      2,
      2
    ],
    [
      "aoe_payment_002",
      3,
      3
    ],
    [
      "aoe_inventory_002",
      1,
      1
    ],
    [
      "aoe_inventory_002",
      2,
      2
    ],
    [
      "aoe_inventory_002",
      3,
      3
    ],
    [
      "aoe_cache_002",
      1,
      1
    ],
    [
      "aoe_cache_002",
      2,
      2
    ],
    [
      "aoe_cache_002",
      3,
      3
    ],
    [
      "aoe_queue_003",
      1,
      1
    ],
    [
      "aoe_queue_003",
      2,
      2
    ],
    [
      "aoe_queue_003",
      3,
      3
    ],
    [
      "aoe_search_003",
      1,
      1
    ],
    [
      "aoe_search_003",
      2,
      2
    ],
    [
      "aoe_search_003",
      3,
      3
    ]
  ],
  "execution_time_us": 26.25,
  "rows": 18,
  "rows_scanned": 18
}
```

- Native string window status: pass

## Numeric JOIN Acceptance

```json
{
  "columns": [
    "query_seq",
    "rank_num",
    "action_code",
    "observed_action_code",
    "expected_top_action_code",
    "avoid_num"
  ],
  "data": [
    [
      1,
      1,
      1,
      6,
      1,
      1
    ],
    [
      1,
      2,
      2,
      6,
      1,
      1
    ],
    [
      1,
      3,
      3,
      6,
      1,
      1
    ],
    [
      2,
      1,
      4,
      2,
      4,
      1
    ],
    [
      2,
      2,
      6,
      2,
      4,
      1
    ],
    [
      2,
      3,
      2,
      2,
      4,
      1
    ],
    [
      3,
      1,
      3,
      2,
      3,
      1
    ],
    [
      3,
      2,
      4,
      2,
      3,
      1
    ],
    [
      3,
      3,
      2,
      2,
      3,
      1
    ],
    [
      4,
      1,
      5,
      6,
      5,
      1
    ],
    [
      4,
      2,
      6,
      6,
      5,
      1
    ],
    [
      4,
      3,
      1,
      6,
      5,
      1
    ],
    [
      5,
      1,
      2,
      6,
      2,
      1
    ],
    [
      5,
      2,
      4,
      6,
      2,
      1
    ],
    [
      5,
      3,
      6,
      6,
      2,
      1
    ],
    [
      6,
      1,
      1,
      2,
      1,
      1
    ],
    [
      6,
      2,
      6,
      2,
      1,
      1
    ],
    [
      6,
      3,
      4,
      2,
      1,
      1
    ]
  ],
  "execution_time_us": 531.0,
  "rows": 18,
  "rows_scanned": 24
}
```

- Numeric JOIN status: pass

## Numeric Window Acceptance

```json
{
  "columns": [
    "query_seq",
    "rank_num",
    "action_code",
    "score_num",
    "rank_check",
    "prev_score"
  ],
  "data": [
    [
      1,
      1,
      1,
      1027725,
      1,
      0
    ],
    [
      1,
      2,
      2,
      312531,
      2,
      1027725
    ],
    [
      1,
      3,
      3,
      155590,
      3,
      312531
    ],
    [
      2,
      1,
      4,
      670031,
      1,
      0
    ],
    [
      2,
      2,
      6,
      155590,
      2,
      670031
    ],
    [
      2,
      3,
      2,
      155590,
      3,
      155590
    ],
    [
      3,
      1,
      3,
      362004,
      1,
      0
    ],
    [
      3,
      2,
      4,
      362004,
      2,
      362004
    ],
    [
      3,
      3,
      2,
      155590,
      3,
      362004
    ],
    [
      4,
      1,
      5,
      594912,
      1,
      0
    ],
    [
      4,
      2,
      6,
      155590,
      2,
      594912
    ],
    [
      4,
      3,
      1,
      155590,
      3,
      155590
    ],
    [
      5,
      1,
      2,
      537068,
      1,
      0
    ],
    [
      5,
      2,
      4,
      169504,
      2,
      537068
    ],
    [
      5,
      3,
      6,
      155590,
      3,
      169504
    ],
    [
      6,
      1,
      1,
      558102,
      1,
      0
    ],
    [
      6,
      2,
      6,
      358316,
      2,
      558102
    ],
    [
      6,
      3,
      4,
      73090,
      3,
      358316
    ]
  ],
  "execution_time_us": 30.0,
  "rows": 18,
  "rows_scanned": 18
}
```

- Numeric window status: pass

## Interpretation

Experiment 009 adds the JOIN/window replay acceptance layer after the
single-node and distributed load/replay harnesses. The native string window
query proves the replay table can partition by `query_id` and preserve
rank order. The numeric projection proves the Action-Outcome replay
decision surface can be checked with a real ZeptoDB hash JOIN plus
ROW_NUMBER/LAG window chain.

The native string-key JOIN now produces semantic string rows for the
original research schema. The numeric projection remains useful for the
top-action and outcome-avoidance checks because hash JOIN predicate
pushdown for aliased WHERE clauses is tracked separately.

## Next Steps

1. Add alias-aware WHERE predicate handling for hash JOIN queries so
   native top-action JOIN checks can replace the projection path.
2. Port Experiment 009 to the two-node live harness so distributed
   JOIN/window limitations are recorded separately from single-node
   executor limits.
3. Add an Action-Outcome shard-key policy so operational replay tables
   can distribute by `query_id` or incident id instead of default symbol 0.
