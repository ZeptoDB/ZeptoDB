# ActionOutcomeReplay Experiment 010 SQL/JOIN/Window Replay Results

Generated at: 2026-06-18T15:03:38Z
Endpoint: `http://127.0.0.1:19343/`
SQL seed: `docs/research/results/action_outcome_sql_replay_006.sql`

## Load Summary

- Seed statements attempted: 203
- Seed statements succeeded: 203
- Seed statements failed: 0
- Seed row-count status: pass
- Vendor table row-count status: pass
- Failed-repeat JOIN status: pass
- Context top-action JOIN status: pass
- Suppression JOIN status: pass
- Misleading retrieval JOIN status: pass
- Window ROW_NUMBER status: pass
- Window LAG status: pass
- Overall SQL/JOIN/window status: pass

## Seed Row Counts

| Table | Expected Inserts | Live Rows | Status |
| --- | ---: | ---: | --- |
| `action_outcome_episode_metrics` | 101 | 101 | pass |
| `action_outcome_episodes` | 32 | 32 | pass |
| `action_outcome_gate_suppressions` | 21 | 21 | pass |
| `action_outcome_replay_recommendations` | 18 | 18 | pass |
| `action_outcome_retrieval_quality_labels` | 26 | 26 | pass |

## Vendor Replay Table Counts

| Table | Expected Rows | Live Rows | Status |
| --- | ---: | ---: | --- |
| `action_outcome_vendor_queries_010` | 6 | 6 | pass |
| `action_outcome_vendor_recommendations_010` | 72 | 72 | pass |
| `action_outcome_vendor_retrieval_010` | 72 | 72 | pass |
| `action_outcome_vendor_suppressions_010` | 21 | 21 | pass |

## Failed-Repeat JOIN

This query joins top recommendations to query episodes and uses alias-qualified
`WHERE` predicates on the joined result:

```sql
SELECT r.variant, r.query_id, r.action_class, q.true_failed_action
FROM action_outcome_vendor_recommendations_010 r
JOIN action_outcome_vendor_queries_010 q ON r.query_id = q.query_id
WHERE r.recommendation_rank = 1 AND r.avoids_failed = 0
```

- Status: pass

| Variant | Query | Recommended Action | Historical Failed Action |
| --- | --- | --- | --- |
| `reflection_only_memory` | `aoe_cache_002` | `restart` | `restart` |
| `runbook_action_prior` | `aoe_cache_002` | `restart` | `restart` |
| `runbook_action_prior` | `aoe_queue_003` | `restart` | `restart` |
| `similar_incident` | `aoe_cache_002` | `restart` | `restart` |
| `similar_incident` | `aoe_payment_002` | `scale_out` | `scale_out` |

## Context-Gated Top Actions

- Status: pass

| Query | Context-Gated Top Action | Historical Failed Action |
| --- | --- | --- |
| `aoe_cache_002` | `cache_purge` | `restart` |
| `aoe_checkout_002` | `rollback` | `restart` |
| `aoe_inventory_002` | `config_revert` | `scale_out` |
| `aoe_payment_002` | `traffic_drain` | `scale_out` |
| `aoe_queue_003` | `scale_out` | `restart` |
| `aoe_search_003` | `rollback` | `scale_out` |

## JOIN/Window Acceptance

- Suppression JOIN rows: 21
- Suppression JOIN status: pass
- Misleading retrieval JOIN rows: 23
- Misleading retrieval JOIN status: pass
- ROW_NUMBER rows: 72
- ROW_NUMBER status: pass
- LAG rows: 72
- LAG status: pass

## Interpretation

Experiment 010 now has a live ZeptoDB SQL acceptance path. The comparison
is no longer only a Python fixture report: recommendations, retrieval
evidence, query controls, and context suppressions are materialized into
declared ZeptoDB tables and audited with native hash JOIN plus window
queries.

The key product result remains intact: vendor-inspired baselines have
failed-repeat top actions, while `context_gated_action_outcome` returns
six safe top actions and uses suppressions to keep mismatched historical
outcomes from dominating the ranking.

## Next Best Step

Port this SQL/JOIN/window replay into the two-node live harness to record
which parts are single-node SQL-complete and which parts still depend on
distributed JOIN/window planner work.
