# ActionOutcomeReplay Experiment 008 Distributed Live ZeptoDB Results

Generated at: 2026-06-18T05:12:29Z
Coordinator endpoint: `http://127.0.0.1:19241/`
SQL seed: `docs/research/results/action_outcome_sql_replay_006.sql`
Node IDs: `1`, `8`

## Load Summary

- Statements attempted: 203
- Statements succeeded: 203
- Statements failed: 0
- Load status: pass
- Row-count status: pass
- Semantic status: pass
- Distributed ingest status: pass
- Remote decoded string status: pass

## Node-Local Stats Delta

| Node ID | Stats URL | ticks_ingested | ticks_stored | ticks_dropped | partitions_created |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | `http://127.0.0.1:19241/stats` | 140 | 140 | 0 | 3 |
| 8 | `http://127.0.0.1:19242/stats` | 58 | 58 | 0 | 2 |

## Owner Split

| Table | Stable table_id | Owner node | Expected inserts |
| --- | ---: | ---: | ---: |
| `action_outcome_episode_metrics` | 57172 | 1 | 101 |
| `action_outcome_episodes` | 3679 | 8 | 32 |
| `action_outcome_gate_suppressions` | 16692 | 1 | 21 |
| `action_outcome_replay_recommendations` | 9749 | 1 | 18 |
| `action_outcome_retrieval_quality_labels` | 37786 | 8 | 26 |

## Row Count Verification

| Table | Expected Inserts | Live Rows | Status |
| --- | ---: | ---: | --- |
| `action_outcome_episode_metrics` | 101 | 101 | pass |
| `action_outcome_episodes` | 32 | 32 | pass |
| `action_outcome_gate_suppressions` | 21 | 21 | pass |
| `action_outcome_replay_recommendations` | 18 | 18 | pass |
| `action_outcome_retrieval_quality_labels` | 26 | 26 | pass |

## Semantic Query Verification

- Expected top-action rows: 6
- Live top-action rows: 6
- Failed-action avoidance rows: 6

| Query | Expected Top Action | Live Top Action |
| --- | --- | --- |
| `aoe_checkout_002` | `rollback` | `rollback` |
| `aoe_payment_002` | `traffic_drain` | `traffic_drain` |
| `aoe_inventory_002` | `config_revert` | `config_revert` |
| `aoe_cache_002` | `cache_purge` | `cache_purge` |
| `aoe_queue_003` | `scale_out` | `scale_out` |
| `aoe_search_003` | `rollback` | `rollback` |

## Remote String Query Verification

`action_outcome_episodes` is owned by node 8 in the 1/8 ring, so this
query verifies that remote RPC results preserve decoded `STRING` values:

```json
{
  "columns": [
    "episode_id",
    "action_class"
  ],
  "data": [
    [
      "aoe_checkout_001",
      "rollback"
    ],
    [
      "aoe_checkout_002",
      "restart"
    ],
    [
      "aoe_checkout_003",
      "scale_out"
    ]
  ],
  "execution_time_us": 0.0,
  "rows": 3,
  "rows_scanned": 0
}
```

- Remote decoded string status: pass

## Interpretation

Experiment 008 validates the Action-Outcome replay seed through a real
two-node ZeptoDB HTTP/RPC topology. The node-local stats prove rows landed
on both nodes, while cluster SELECT row counts and semantic top-action
queries still match the single-node replay contract.

This run also covers the distributed string-result boundary: `STRING` and
`SYMBOL` columns must survive RPC serialization and concat merge as decoded
values, not node-local dictionary codes. Devlog 188 adds the CI-sized C++
regression and typed-row string-payload hardening for this boundary.

## Next Steps

1. Extend Experiment 008 with JOIN/window replay queries across
   `action_outcome_episodes` and recommendation tables.
2. Add a shard-key policy for symbol-less operational tables so production
   two-node splits do not depend on node-id choice.
