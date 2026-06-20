# ActionOutcomeReplay Experiment 002 Guardrail Results

Generated at: 2026-06-13T21:07:32Z
Fixture: `docs/research/fixtures/action_outcome_episodes.json`

## Summary

- Query episodes: 6
- Baseline top-3 successful action hit rate: 1.00
- Guarded top-3 successful action hit rate: 1.00
- Baseline failed-action avoidance rate: 1.00
- Guarded failed-action avoidance rate: 1.00
- Baseline cross-family top-3 candidates: 1
- Guarded cross-family top-3 candidates: 0
- Baseline weak cross-family top-3 candidates: 1
- Guarded weak cross-family top-3 candidates: 0

## Recommendation Comparison

| Query | True Action | True Outcome | Useful Actions | Baseline Recs | Guarded Recs | Top Action Changed | Baseline Cross Top3 | Guarded Cross Top3 |
| --- | --- | --- | --- | --- | --- | --- | ---: | ---: |
| aoe_checkout_002 | restart | failure | config_revert, rollback, scale_out | rollback:0.55, config_revert:0.50, scale_out:0.23 | rollback:1.03, config_revert:1.03, scale_out:0.48 | no | 0 | 0 |
| aoe_payment_002 | scale_out | failure | restart, rollback, traffic_drain | traffic_drain:0.54, restart:0.46, rollback:0.41 | rollback:1.03, traffic_drain:1.03, restart:1.03 | yes | 0 | 0 |
| aoe_inventory_002 | scale_out | rollback_required | config_revert, restart, traffic_drain | restart:0.54, config_revert:0.52, traffic_drain:0.42 | traffic_drain:1.03, restart:1.03, config_revert:1.03 | yes | 0 | 0 |
| aoe_cache_002 | restart | failure | cache_purge, rollback | cache_purge:0.75, rollback:0.47, restart:0.27 | cache_purge:1.03, rollback:1.03, scale_out:-0.95 | no | 0 | 0 |
| aoe_queue_003 | restart | failure | queue_reset, scale_out, traffic_drain | scale_out:0.70, queue_reset:0.52, restart:0.25 | queue_reset:1.03, scale_out:1.03, traffic_drain:0.48 | yes | 0 | 0 |
| aoe_search_003 | scale_out | failure | restart, rollback, traffic_drain | rollback:0.52, restart:0.31, scale_out:0.20 | rollback:1.03, traffic_drain:0.48, restart:0.48 | no | 1 | 0 |

## Guarded Top-3 Retrieval Details

| Query | Rank | Episode | Candidate Family | Action | Outcome | Guarded Score | Base Total | Cross-Family Penalty | Symptom | Topology |
| --- | ---: | --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.698 | 0.698 | 0.000 | 0.600 | 0.667 |
| aoe_checkout_002 | 2 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.506 | 0.506 | 0.000 | 0.355 | 0.600 |
| aoe_checkout_002 | 3 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.496 | 0.496 | 0.000 | 0.433 | 0.714 |
| aoe_payment_002 | 1 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.571 | 0.571 | 0.000 | 0.258 | 0.625 |
| aoe_payment_002 | 2 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.540 | 0.540 | 0.000 | 0.435 | 0.733 |
| aoe_payment_002 | 3 | aoe_payment_004 | payment_dependency_timeout | rollback | success | 0.414 | 0.414 | 0.000 | 0.276 | 0.667 |
| aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.638 | 0.638 | 0.000 | 0.696 | 0.786 |
| aoe_inventory_002 | 2 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.544 | 0.544 | 0.000 | 0.324 | 0.562 |
| aoe_inventory_002 | 3 | aoe_inventory_004 | inventory_db_connection_pool | traffic_drain | success | 0.420 | 0.420 | 0.000 | 0.379 | 0.600 |
| aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.754 | 0.754 | 0.000 | 0.667 | 0.765 |
| aoe_cache_002 | 2 | aoe_cache_004 | recommendation_cache_stale | scale_out | failure | 0.514 | 0.514 | 0.000 | 0.593 | 0.688 |
| aoe_cache_002 | 3 | aoe_cache_003 | recommendation_cache_stale | rollback | success | 0.466 | 0.466 | 0.000 | 0.382 | 0.632 |
| aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.700 | 0.700 | 0.000 | 0.552 | 0.938 |
| aoe_queue_003 | 2 | aoe_queue_002 | order_queue_backlog | queue_reset | success | 0.523 | 0.523 | 0.000 | 0.441 | 1.000 |
| aoe_queue_003 | 3 | aoe_queue_004 | order_queue_backlog | traffic_drain | partial_success | 0.357 | 0.357 | 0.000 | 0.314 | 0.667 |
| aoe_search_003 | 1 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.683 | 0.683 | 0.000 | 0.640 | 0.769 |
| aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.628 | 0.628 | 0.000 | 0.500 | 0.769 |
| aoe_search_003 | 3 | aoe_search_004 | search_index_memory_leak | traffic_drain | partial_success | 0.364 | 0.364 | 0.000 | 0.387 | 0.625 |

## Interpretation

Guarded V2 should be judged by whether it reduces weak cross-family retrieval
without lowering successful-action hit rate or failed-action avoidance.
If it changes top actions, inspect whether the change is safer or simply
more conservative.
Guarded recommendation scores are ranking utilities, not probabilities;
their absolute scale is not comparable to baseline recommendation scores.

## Next Steps

1. Add an ablation report that removes one signal family at a time.
2. Add noisy distractor episodes to test whether guardrails survive less-clean data.
3. Map the fixture into ZeptoDB tables so replay can use SQL/time-window queries.
