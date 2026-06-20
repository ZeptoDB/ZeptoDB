# ActionOutcomeReplay Experiment 001 Results

Generated at: 2026-06-13T12:30:06Z
Fixture: `docs/research/fixtures/action_outcome_episodes.json`

## Summary

- Query episodes: 6
- Top-3 successful action hit rate: 1.00
- Failed-action avoidance rate: 1.00

## Query Results

| Query | Incident Type | True Action | True Outcome | Useful Actions | Keyword Recs | Text Recs | Time-Series Recs | Action-Outcome Recs | Hit | Avoid Failed Repeat |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| aoe_checkout_002 | checkout_latency_after_deploy | restart | failure | config_revert, rollback, scale_out | restart:0.27, rollback:0.24, config_revert:0.18 | rollback:0.29, restart:0.23, config_revert:0.22 | rollback:0.58, config_revert:0.46, scale_out:0.24 | rollback:0.55, config_revert:0.50, scale_out:0.23 | yes | yes |
| aoe_payment_002 | payment_dependency_timeout | scale_out | failure | restart, rollback, traffic_drain | traffic_drain:0.56, rollback:0.25, restart:0.17 | traffic_drain:0.29, rollback:0.18, restart:0.17 | traffic_drain:0.59, restart:0.51, rollback:0.44 | traffic_drain:0.54, restart:0.46, rollback:0.41 | yes | yes |
| aoe_inventory_002 | inventory_db_connection_pool | scale_out | rollback_required | config_revert, restart, traffic_drain | config_revert:0.60, traffic_drain:0.36, restart:0.15 | config_revert:0.26, traffic_drain:0.25, restart:0.21 | config_revert:0.51, restart:0.46, traffic_drain:0.40 | restart:0.54, config_revert:0.52, traffic_drain:0.42 | yes | yes |
| aoe_cache_002 | recommendation_cache_stale | restart | failure | cache_purge, rollback | cache_purge:0.56, rollback:0.31, restart:-0.12 | cache_purge:0.36, rollback:0.26, restart:0.16 | cache_purge:0.80, rollback:0.41, restart:0.06 | cache_purge:0.75, rollback:0.47, restart:0.27 | yes | yes |
| aoe_queue_003 | order_queue_backlog | restart | failure | queue_reset, scale_out, traffic_drain | scale_out:0.56, queue_reset:0.45, traffic_drain:0.19 | queue_reset:0.30, scale_out:0.30, rollback:0.17 | queue_reset:0.57, scale_out:0.39, traffic_drain:0.18 | scale_out:0.70, queue_reset:0.52, restart:0.25 | yes | yes |
| aoe_search_003 | search_index_memory_leak | scale_out | failure | restart, rollback, traffic_drain | rollback:0.50, restart:0.23, traffic_drain:0.08 | rollback:0.34, restart:0.16, traffic_drain:0.12 | rollback:0.71, scale_out:0.21, traffic_drain:0.18 | rollback:0.52, restart:0.31, scale_out:0.20 | yes | yes |

## Top-3 Action-Outcome Retrieval Breakdowns

| Query | Rank | Episode | Action | Outcome | Total | Symptom | Temporal | Topology | Change | Action Outcome | Text | Recency | Risk Penalty |
| --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| aoe_checkout_002 | 1 | aoe_checkout_001 | rollback | success | 0.698 | 0.600 | 0.928 | 0.667 | 0.670 | 1.000 | 0.354 | 0.927 | 0.040 |
| aoe_checkout_002 | 2 | aoe_checkout_003 | scale_out | partial_success | 0.506 | 0.355 | 0.546 | 0.600 | 0.670 | 0.725 | 0.192 | 0.939 | 0.040 |
| aoe_checkout_002 | 3 | aoe_checkout_004 | config_revert | success | 0.496 | 0.433 | 0.635 | 0.714 | 0.000 | 1.000 | 0.222 | 0.854 | 0.040 |
| aoe_payment_002 | 1 | aoe_payment_003 | restart | success | 0.571 | 0.258 | 0.649 | 0.625 | 0.550 | 1.000 | 0.175 | 0.925 | 0.000 |
| aoe_payment_002 | 2 | aoe_payment_001 | traffic_drain | success | 0.540 | 0.435 | 0.677 | 0.733 | 0.550 | 1.000 | 0.288 | 0.928 | 0.100 |
| aoe_payment_002 | 3 | aoe_payment_004 | rollback | success | 0.414 | 0.276 | 0.737 | 0.667 | 0.000 | 1.000 | 0.188 | 0.854 | 0.100 |
| aoe_inventory_002 | 1 | aoe_inventory_001 | config_revert | success | 0.638 | 0.696 | 0.951 | 0.786 | 0.000 | 1.000 | 0.339 | 0.928 | 0.040 |
| aoe_inventory_002 | 2 | aoe_inventory_003 | restart | success | 0.544 | 0.324 | 0.473 | 0.562 | 0.550 | 1.000 | 0.212 | 0.938 | 0.000 |
| aoe_inventory_002 | 3 | aoe_inventory_004 | traffic_drain | success | 0.420 | 0.379 | 0.684 | 0.600 | 0.000 | 1.000 | 0.250 | 0.841 | 0.100 |
| aoe_cache_002 | 1 | aoe_cache_001 | cache_purge | success | 0.754 | 0.667 | 0.934 | 0.765 | 0.850 | 1.000 | 0.356 | 0.928 | 0.040 |
| aoe_cache_002 | 2 | aoe_cache_004 | scale_out | failure | 0.514 | 0.593 | 0.683 | 0.688 | 0.670 | 0.150 | 0.297 | 0.856 | 0.040 |
| aoe_cache_002 | 3 | aoe_cache_003 | rollback | success | 0.466 | 0.382 | 0.559 | 0.632 | 0.000 | 1.000 | 0.261 | 0.946 | 0.040 |
| aoe_queue_003 | 1 | aoe_queue_001 | scale_out | success | 0.700 | 0.552 | 0.918 | 0.938 | 0.550 | 1.000 | 0.297 | 0.869 | 0.040 |
| aoe_queue_003 | 2 | aoe_queue_002 | queue_reset | success | 0.523 | 0.441 | 0.790 | 1.000 | 0.000 | 1.000 | 0.302 | 0.935 | 0.100 |
| aoe_queue_003 | 3 | aoe_queue_004 | traffic_drain | partial_success | 0.357 | 0.314 | 0.573 | 0.667 | 0.000 | 0.725 | 0.242 | 0.934 | 0.100 |
| aoe_search_003 | 1 | aoe_search_001 | restart | partial_success | 0.683 | 0.640 | 0.963 | 0.769 | 0.670 | 0.725 | 0.350 | 0.860 | 0.040 |
| aoe_search_003 | 2 | aoe_search_002 | rollback | success | 0.628 | 0.500 | 0.911 | 0.769 | 0.670 | 1.000 | 0.339 | 0.927 | 0.100 |
| aoe_search_003 | 3 | aoe_checkout_003 | scale_out | partial_success | 0.440 | 0.212 | 0.594 | 0.438 | 0.670 | 0.825 | 0.162 | 0.243 | 0.040 |

## Interpretation

This first result is a fixture-level sanity check, not a benchmark claim.
A useful next step is to inspect cases where the recommended action is
generic or where retrieval is dominated by incident type instead of
specific symptoms, topology, change context, and recovery outcomes.

## Next Steps

1. Tune action aggregation so one weak positive candidate does not dominate several strong negative cases.
2. Add noisy distractor episodes to reduce fixture cleanliness bias.
3. Map fixture rows into ZeptoDB tables for replay through SQL.
4. Add a result-diff report that shows how recommendations change when each signal family is removed.
