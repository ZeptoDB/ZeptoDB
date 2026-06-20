# ActionOutcomeReplay Experiment 006 SQL-Backed Replay Results

Generated at: 2026-06-18T02:14:23Z
Fixtures:
- `docs/research/fixtures/action_outcome_episodes.json`
- `docs/research/fixtures/action_outcome_distractor_episodes.json`
- Quality labels: `docs/research/fixtures/action_outcome_retrieval_quality_labels.json`

## SQL Table Counts

| Table | Rows |
| --- | ---: |
| action_outcome_episodes | 32 |
| action_outcome_episode_metrics | 101 |
| action_outcome_retrieval_quality_labels | 26 |
| action_outcome_replay_recommendations | 18 |
| action_outcome_gate_suppressions | 21 |

## Replay Summary

| Variant | Source | Top-3 Hit Rate | Failed-Action Avoidance | Cross-Family Top3 | Weak Cross-Family Top3 | Gate Suppressions | Labeled Top3 Quality |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| full_guarded | SQL | 1.00 | 0.67 | 0 | 0 | 0 | useful:8, superficial:5, misleading:5 |
| context_gated | SQL | 1.00 | 1.00 | 0 | 0 | 21 | useful:8, superficial:4, misleading:6 |
| context_gated | JSON control | 1.00 | 1.00 | 0 | 0 | 21 | useful:8, superficial:4, misleading:6 |

## SQL vs JSON Control

- Match status: pass
- Top-action mismatches: 0

## Per-Query SQL Context-Gated Actions

| Query | SQL Top Action | JSON Top Action | Match | SQL Top Actions | Avoids Failed Repeat |
| --- | --- | --- | --- | --- | --- |
| aoe_checkout_002 | rollback | rollback | yes | rollback:1.03, scale_out:0.31, config_revert:0.16 | yes |
| aoe_payment_002 | traffic_drain | traffic_drain | yes | traffic_drain:0.67, restart:0.16, scale_out:0.16 | yes |
| aoe_inventory_002 | config_revert | config_revert | yes | config_revert:0.36, traffic_drain:0.36, scale_out:0.16 | yes |
| aoe_cache_002 | cache_purge | cache_purge | yes | cache_purge:0.59, restart:0.16, rollback:0.16 | yes |
| aoe_queue_003 | scale_out | scale_out | yes | scale_out:0.54, traffic_drain:0.17, restart:0.16 | yes |
| aoe_search_003 | rollback | rollback | yes | rollback:0.56, restart:0.36, traffic_drain:0.07 | yes |

## Example SQL Queries

```sql
SELECT episode_id, incident_type, action_class, human_outcome
FROM action_outcome_episodes
WHERE incident_type = 'order_queue_backlog'
ORDER BY action_ts_ns;

SELECT episode_id, metric_name, metric_value
FROM action_outcome_episode_metrics
WHERE metric_name IN ('cpu_pct', 'db_conn_used_pct', 'consumer_error_pct')
ORDER BY episode_id, metric_name;

SELECT query_id, candidate_id, action_class, reasons
FROM action_outcome_gate_suppressions
WHERE multiplier_micros < 1000000
ORDER BY query_id, candidate_id;
```

## Interpretation

Experiment 006 validates the first SQL-backed research contract. The replay
logic no longer consumes raw JSON fixtures directly; it reconstructs episodes
from SQL tables and matches the JSON control result.

This is still a local SQL harness, not a live ZeptoDB server run. The next
step is to execute the generated SQL through ZeptoDB's HTTP SQL endpoint and
compare live query results against this report.

## Next Steps

1. Run the generated SQL seed against a live ZeptoDB HTTP server.
2. Add live SQL query checks for top actions and suppression rows.
3. Promote the table schema into a design note if the live replay matches.
