# ActionOutcomeReplay Ablation Results

Generated at: 2026-06-14T00:45:34Z
Fixtures:
- `docs/research/fixtures/action_outcome_episodes.json`
- `docs/research/fixtures/action_outcome_distractor_episodes.json`
- Quality labels: `docs/research/fixtures/action_outcome_retrieval_quality_labels.json`

## Summary

| Variant | Removed Signals | Top-3 Hit Rate | Failed-Action Avoidance | Cross-Family Top3 | Weak Cross-Family Top3 | Top Action Changes | Labeled Top3 Quality |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| full_guarded | none | 1.00 | 0.67 | 0 | 0 | 0 | useful:8, superficial:5, misleading:5 |
| no_symptom | symptom | 1.00 | 0.67 | 0 | 0 | 0 | useful:8, superficial:5, misleading:5 |
| no_temporal | temporal_motif | 1.00 | 0.67 | 0 | 0 | 0 | useful:8, superficial:5, misleading:5 |
| no_topology | topology | 1.00 | 0.67 | 0 | 0 | 0 | useful:8, superficial:5, misleading:5 |
| no_change | change | 1.00 | 0.50 | 0 | 0 | 1 | useful:7, superficial:6, misleading:5 |
| no_action_outcome | action_outcome | 1.00 | 1.00 | 0 | 0 | 2 | useful:8, superficial:4, misleading:6 |
| no_text | postmortem_text | 1.00 | 0.67 | 0 | 0 | 0 | useful:8, superficial:5, misleading:5 |
| no_recency | recency | 1.00 | 0.50 | 0 | 0 | 1 | useful:8, superficial:5, misleading:5 |
| no_risk | risk_penalty | 1.00 | 0.67 | 0 | 0 | 1 | useful:8, superficial:5, misleading:5 |
| no_cross_family_guardrail | cross_family_penalty | 1.00 | 0.67 | 0 | 0 | 1 | useful:8, superficial:5, misleading:5 |

## Per-Query Top Action Changes

| Query | Full Guarded | no_symptom | no_temporal | no_topology | no_change | no_action_outcome | no_text | no_recency | no_risk | no_cross_family_guardrail |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| aoe_checkout_002 | rollback | rollback | rollback | rollback | rollback | rollback | rollback | rollback | rollback | rollback |
| aoe_payment_002 | restart | restart | restart | restart | scale_out | restart | restart | scale_out | traffic_drain | restart |
| aoe_inventory_002 | config_revert | config_revert | config_revert | config_revert | config_revert | config_revert | config_revert | config_revert | config_revert | restart |
| aoe_cache_002 | restart | restart | restart | restart | restart | cache_purge | restart | restart | restart | restart |
| aoe_queue_003 | restart | restart | restart | restart | restart | scale_out | restart | restart | restart | restart |
| aoe_search_003 | restart | restart | restart | restart | restart | restart | restart | restart | restart | restart |

## Per-Variant Recommendations

### full_guarded

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | useful:2, superficial:1 |
| aoe_payment_002 | restart:1.03, scale_out:1.03, traffic_drain:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | config_revert:1.03, scale_out:1.03, restart:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | restart:1.03, rollback:1.03, cache_purge:0.30 | yes | no | 0 | useful:2, misleading:1 |
| aoe_queue_003 | restart:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | no | 0 | useful:1, superficial:1, misleading:1 |
| aoe_search_003 | restart:0.74, traffic_drain:0.48, rollback:0.24 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |

### no_symptom

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | useful:2, superficial:1 |
| aoe_payment_002 | restart:1.03, scale_out:1.03, traffic_drain:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | config_revert:1.03, restart:1.03, scale_out:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | restart:1.03, rollback:1.03, cache_purge:0.31 | yes | no | 0 | useful:2, misleading:1 |
| aoe_queue_003 | restart:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | no | 0 | useful:1, superficial:1, misleading:1 |
| aoe_search_003 | restart:0.74, traffic_drain:0.48, rollback:0.28 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |

### no_temporal

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | useful:2, superficial:1 |
| aoe_payment_002 | restart:1.03, traffic_drain:1.03, scale_out:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | config_revert:1.03, restart:1.03, scale_out:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | restart:1.03, rollback:1.03, cache_purge:0.39 | yes | no | 0 | useful:2, misleading:1 |
| aoe_queue_003 | restart:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | no | 0 | useful:1, superficial:1, misleading:1 |
| aoe_search_003 | restart:0.73, traffic_drain:0.48, rollback:0.36 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |

### no_topology

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | useful:2, superficial:1 |
| aoe_payment_002 | restart:1.03, traffic_drain:1.03, scale_out:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | config_revert:1.03, restart:1.03, scale_out:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | restart:1.03, rollback:1.03, cache_purge:0.36 | yes | no | 0 | useful:2, misleading:1 |
| aoe_queue_003 | restart:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | no | 0 | useful:1, superficial:1, misleading:1 |
| aoe_search_003 | restart:0.73, traffic_drain:0.48, rollback:0.30 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |

### no_change

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | useful:2, superficial:1 |
| aoe_payment_002 | scale_out:1.03, restart:1.03, traffic_drain:1.03 | yes | no | 0 | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | config_revert:1.03, scale_out:1.03, restart:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | restart:1.03, rollback:1.03, cache_purge:0.21 | yes | no | 0 | useful:1, superficial:1, misleading:1 |
| aoe_queue_003 | restart:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | no | 0 | useful:1, superficial:1, misleading:1 |
| aoe_search_003 | restart:0.76, traffic_drain:0.48, rollback:0.16 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |

### no_action_outcome

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, scale_out:1.03, config_revert:1.03 | yes | yes | 0 | useful:2, superficial:1 |
| aoe_payment_002 | restart:1.03, traffic_drain:1.03, scale_out:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | config_revert:1.03, scale_out:1.03, restart:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | cache_purge:1.04, restart:1.03, scale_out:1.03 | yes | yes | 0 | useful:2, misleading:1 |
| aoe_queue_003 | scale_out:1.04, restart:1.03, queue_reset:1.03 | yes | yes | 0 | useful:1, misleading:2 |
| aoe_search_003 | restart:1.04, rollback:1.04, traffic_drain:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |

### no_text

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | useful:2, superficial:1 |
| aoe_payment_002 | restart:1.03, scale_out:1.03, traffic_drain:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | config_revert:1.03, scale_out:1.03, restart:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | restart:1.03, rollback:1.03, cache_purge:0.30 | yes | no | 0 | useful:2, misleading:1 |
| aoe_queue_003 | restart:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | no | 0 | useful:1, superficial:1, misleading:1 |
| aoe_search_003 | restart:0.74, traffic_drain:0.48, rollback:0.24 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |

### no_recency

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | useful:2, superficial:1 |
| aoe_payment_002 | scale_out:1.03, restart:1.03, traffic_drain:1.03 | yes | no | 0 | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | config_revert:1.03, scale_out:1.03, restart:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | restart:1.03, rollback:1.03, cache_purge:0.29 | yes | no | 0 | useful:2, misleading:1 |
| aoe_queue_003 | restart:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | no | 0 | useful:1, superficial:1, misleading:1 |
| aoe_search_003 | restart:0.74, traffic_drain:0.48, rollback:0.24 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |

### no_risk

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | useful:2, superficial:1 |
| aoe_payment_002 | traffic_drain:1.03, scale_out:1.03, restart:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | config_revert:1.03, scale_out:1.03, restart:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | restart:1.03, rollback:1.03, cache_purge:0.28 | yes | no | 0 | useful:2, misleading:1 |
| aoe_queue_003 | restart:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | no | 0 | useful:1, superficial:1, misleading:1 |
| aoe_search_003 | restart:0.74, traffic_drain:0.48, rollback:0.20 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |

### no_cross_family_guardrail

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.06, config_revert:1.03, restart:0.75 | yes | yes | 0 | useful:2, superficial:1 |
| aoe_payment_002 | restart:1.04, traffic_drain:1.03, rollback:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | restart:1.06, config_revert:1.04, scale_out:1.03 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | restart:1.06, rollback:1.03, cache_purge:0.30 | yes | no | 0 | useful:2, misleading:1 |
| aoe_queue_003 | restart:1.04, queue_reset:1.03, traffic_drain:0.48 | yes | no | 0 | useful:1, superficial:1, misleading:1 |
| aoe_search_003 | restart:0.81, scale_out:0.48, traffic_drain:0.48 | yes | yes | 0 | useful:1, superficial:1, misleading:1 |

## Labeled Top-3 Retrieval Details

| Variant | Query | Rank | Candidate | Candidate Family | Action | Outcome | Score | Quality Label |
| --- | --- | ---: | --- | --- | --- | --- | ---: | --- |
| full_guarded | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.698 | useful |
| full_guarded | aoe_checkout_002 | 2 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.506 | superficial |
| full_guarded | aoe_checkout_002 | 3 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.496 | useful |
| full_guarded | aoe_payment_002 | 1 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.571 | superficial |
| full_guarded | aoe_payment_002 | 2 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.548 | misleading |
| full_guarded | aoe_payment_002 | 3 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.540 | useful |
| full_guarded | aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.638 | useful |
| full_guarded | aoe_inventory_002 | 2 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.560 | misleading |
| full_guarded | aoe_inventory_002 | 3 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.544 | superficial |
| full_guarded | aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.754 | useful |
| full_guarded | aoe_cache_002 | 2 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 0.722 | misleading |
| full_guarded | aoe_cache_002 | 3 | aoe_cache_004 | recommendation_cache_stale | scale_out | failure | 0.514 | useful |
| full_guarded | aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.700 | useful |
| full_guarded | aoe_queue_003 | 2 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 0.649 | misleading |
| full_guarded | aoe_queue_003 | 3 | aoe_queue_002 | order_queue_backlog | queue_reset | success | 0.523 | superficial |
| full_guarded | aoe_search_003 | 1 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.683 | superficial |
| full_guarded | aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.628 | useful |
| full_guarded | aoe_search_003 | 3 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 0.534 | misleading |
| no_symptom | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.732 | useful |
| no_symptom | aoe_checkout_002 | 2 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.553 | superficial |
| no_symptom | aoe_checkout_002 | 3 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.521 | useful |
| no_symptom | aoe_payment_002 | 1 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.650 | superficial |
| no_symptom | aoe_payment_002 | 2 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.606 | misleading |
| no_symptom | aoe_payment_002 | 3 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.591 | useful |
| no_symptom | aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.633 | useful |
| no_symptom | aoe_inventory_002 | 2 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.600 | superficial |
| no_symptom | aoe_inventory_002 | 3 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.581 | misleading |
| no_symptom | aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.786 | useful |
| no_symptom | aoe_cache_002 | 2 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 0.755 | misleading |
| no_symptom | aoe_cache_002 | 3 | aoe_cache_004 | recommendation_cache_stale | scale_out | failure | 0.504 | useful |
| no_symptom | aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.747 | useful |
| no_symptom | aoe_queue_003 | 2 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 0.712 | misleading |
| no_symptom | aoe_queue_003 | 3 | aoe_queue_002 | order_queue_backlog | queue_reset | success | 0.569 | superficial |
| no_symptom | aoe_search_003 | 1 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.704 | superficial |
| no_symptom | aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.685 | useful |
| no_symptom | aoe_search_003 | 3 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 0.563 | misleading |
| no_temporal | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.650 | useful |
| no_temporal | aoe_checkout_002 | 2 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.505 | superficial |
| no_temporal | aoe_checkout_002 | 3 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.471 | useful |
| no_temporal | aoe_payment_002 | 1 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.552 | superficial |
| no_temporal | aoe_payment_002 | 2 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.531 | useful |
| no_temporal | aoe_payment_002 | 3 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.470 | misleading |
| no_temporal | aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.569 | useful |
| no_temporal | aoe_inventory_002 | 2 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.562 | superficial |
| no_temporal | aoe_inventory_002 | 3 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.512 | misleading |
| no_temporal | aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.719 | useful |
| no_temporal | aoe_cache_002 | 2 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 0.687 | misleading |
| no_temporal | aoe_cache_002 | 3 | aoe_cache_004 | recommendation_cache_stale | scale_out | failure | 0.481 | useful |
| no_temporal | aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.656 | useful |
| no_temporal | aoe_queue_003 | 2 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 0.599 | misleading |
| no_temporal | aoe_queue_003 | 3 | aoe_queue_002 | order_queue_backlog | queue_reset | success | 0.481 | superficial |
| no_temporal | aoe_search_003 | 1 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.623 | superficial |
| no_temporal | aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.583 | useful |
| no_temporal | aoe_search_003 | 3 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 0.464 | misleading |
| no_topology | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.710 | useful |
| no_topology | aoe_checkout_002 | 2 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.496 | superficial |
| no_topology | aoe_checkout_002 | 3 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.464 | useful |
| no_topology | aoe_payment_002 | 1 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.562 | superficial |
| no_topology | aoe_payment_002 | 2 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.524 | useful |
| no_topology | aoe_payment_002 | 3 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.476 | misleading |
| no_topology | aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.618 | useful |
| no_topology | aoe_inventory_002 | 2 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.541 | superficial |
| no_topology | aoe_inventory_002 | 3 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.490 | misleading |
| no_topology | aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.760 | useful |
| no_topology | aoe_cache_002 | 2 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 0.734 | misleading |
| no_topology | aoe_cache_002 | 3 | aoe_cache_004 | recommendation_cache_stale | scale_out | failure | 0.490 | useful |
| no_topology | aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.665 | useful |
| no_topology | aoe_queue_003 | 2 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 0.636 | misleading |
| no_topology | aoe_queue_003 | 3 | aoe_queue_002 | order_queue_backlog | queue_reset | success | 0.457 | superficial |
| no_topology | aoe_search_003 | 1 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.675 | superficial |
| no_topology | aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.621 | useful |
| no_topology | aoe_search_003 | 3 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 0.518 | misleading |
| no_change | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.710 | useful |
| no_change | aoe_checkout_002 | 2 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.590 | useful |
| no_change | aoe_checkout_002 | 3 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.484 | superficial |
| no_change | aoe_payment_002 | 1 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.652 | misleading |
| no_change | aoe_payment_002 | 2 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.575 | superficial |
| no_change | aoe_payment_002 | 3 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.556 | useful |
| no_change | aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.757 | useful |
| no_change | aoe_inventory_002 | 2 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.666 | misleading |
| no_change | aoe_inventory_002 | 3 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.543 | superficial |
| no_change | aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.745 | useful |
| no_change | aoe_cache_002 | 2 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 0.699 | misleading |
| no_change | aoe_cache_002 | 3 | aoe_cache_003 | recommendation_cache_stale | rollback | success | 0.556 | superficial |
| no_change | aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.734 | useful |
| no_change | aoe_queue_003 | 2 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 0.666 | misleading |
| no_change | aoe_queue_003 | 3 | aoe_queue_002 | order_queue_backlog | queue_reset | success | 0.633 | superficial |
| no_change | aoe_search_003 | 1 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.693 | superficial |
| no_change | aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.639 | useful |
| no_change | aoe_search_003 | 3 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 0.635 | misleading |
| no_action_outcome | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.652 | useful |
| no_action_outcome | aoe_checkout_002 | 2 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.474 | superficial |
| no_action_outcome | aoe_checkout_002 | 3 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.414 | useful |
| no_action_outcome | aoe_payment_002 | 1 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.496 | superficial |
| no_action_outcome | aoe_payment_002 | 2 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.477 | useful |
| no_action_outcome | aoe_payment_002 | 3 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.476 | misleading |
| no_action_outcome | aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.581 | useful |
| no_action_outcome | aoe_inventory_002 | 2 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.490 | misleading |
| no_action_outcome | aoe_inventory_002 | 3 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.464 | superficial |
| no_action_outcome | aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.718 | useful |
| no_action_outcome | aoe_cache_002 | 2 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 0.673 | misleading |
| no_action_outcome | aoe_cache_002 | 3 | aoe_cache_004 | recommendation_cache_stale | scale_out | failure | 0.585 | useful |
| no_action_outcome | aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.654 | useful |
| no_action_outcome | aoe_queue_003 | 2 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 0.587 | misleading |
| no_action_outcome | aoe_queue_003 | 3 | aoe_distractor_queue_002 | order_queue_backlog | scale_out | rollback_required | 0.502 | misleading |
| no_action_outcome | aoe_search_003 | 1 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.683 | superficial |
| no_action_outcome | aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.580 | useful |
| no_action_outcome | aoe_search_003 | 3 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 0.459 | misleading |
| no_text | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.740 | useful |
| no_text | aoe_checkout_002 | 2 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.545 | superficial |
| no_text | aoe_checkout_002 | 3 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.531 | useful |
| no_text | aoe_payment_002 | 1 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.615 | superficial |
| no_text | aoe_payment_002 | 2 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.582 | misleading |
| no_text | aoe_payment_002 | 3 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.579 | useful |
| no_text | aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.675 | useful |
| no_text | aoe_inventory_002 | 2 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.596 | misleading |
| no_text | aoe_inventory_002 | 3 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.581 | superficial |
| no_text | aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.803 | useful |
| no_text | aoe_cache_002 | 2 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 0.764 | misleading |
| no_text | aoe_cache_002 | 3 | aoe_cache_004 | recommendation_cache_stale | scale_out | failure | 0.542 | useful |
| no_text | aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.749 | useful |
| no_text | aoe_queue_003 | 2 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 0.686 | misleading |
| no_text | aoe_queue_003 | 3 | aoe_queue_002 | order_queue_backlog | queue_reset | success | 0.559 | superficial |
| no_text | aoe_search_003 | 1 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.725 | superficial |
| no_text | aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.672 | useful |
| no_text | aoe_search_003 | 3 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 0.566 | misleading |
| no_recency | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.688 | useful |
| no_recency | aoe_checkout_002 | 2 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.485 | superficial |
| no_recency | aoe_checkout_002 | 3 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.479 | useful |
| no_recency | aoe_payment_002 | 1 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.570 | misleading |
| no_recency | aoe_payment_002 | 2 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.553 | superficial |
| no_recency | aoe_payment_002 | 3 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.525 | useful |
| no_recency | aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.624 | useful |
| no_recency | aoe_inventory_002 | 2 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.581 | misleading |
| no_recency | aoe_inventory_002 | 3 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.524 | superficial |
| no_recency | aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.747 | useful |
| no_recency | aoe_cache_002 | 2 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 0.736 | misleading |
| no_recency | aoe_cache_002 | 3 | aoe_cache_004 | recommendation_cache_stale | scale_out | failure | 0.498 | useful |
| no_recency | aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.693 | useful |
| no_recency | aoe_queue_003 | 2 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 0.653 | misleading |
| no_recency | aoe_queue_003 | 3 | aoe_queue_002 | order_queue_backlog | queue_reset | success | 0.507 | superficial |
| no_recency | aoe_search_003 | 1 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.676 | superficial |
| no_recency | aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.618 | useful |
| no_recency | aoe_search_003 | 3 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 0.530 | misleading |
| no_risk | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.738 | useful |
| no_risk | aoe_checkout_002 | 2 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.546 | superficial |
| no_risk | aoe_checkout_002 | 3 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.536 | useful |
| no_risk | aoe_payment_002 | 1 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.640 | useful |
| no_risk | aoe_payment_002 | 2 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.588 | misleading |
| no_risk | aoe_payment_002 | 3 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.571 | superficial |
| no_risk | aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.678 | useful |
| no_risk | aoe_inventory_002 | 2 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.600 | misleading |
| no_risk | aoe_inventory_002 | 3 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.544 | superficial |
| no_risk | aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.794 | useful |
| no_risk | aoe_cache_002 | 2 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 0.722 | misleading |
| no_risk | aoe_cache_002 | 3 | aoe_cache_004 | recommendation_cache_stale | scale_out | failure | 0.554 | useful |
| no_risk | aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.740 | useful |
| no_risk | aoe_queue_003 | 2 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 0.649 | misleading |
| no_risk | aoe_queue_003 | 3 | aoe_queue_002 | order_queue_backlog | queue_reset | success | 0.623 | superficial |
| no_risk | aoe_search_003 | 1 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.728 | useful |
| no_risk | aoe_search_003 | 2 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.723 | superficial |
| no_risk | aoe_search_003 | 3 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 0.574 | misleading |
| no_cross_family_guardrail | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.698 | useful |
| no_cross_family_guardrail | aoe_checkout_002 | 2 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.506 | superficial |
| no_cross_family_guardrail | aoe_checkout_002 | 3 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.496 | useful |
| no_cross_family_guardrail | aoe_payment_002 | 1 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.571 | superficial |
| no_cross_family_guardrail | aoe_payment_002 | 2 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.548 | misleading |
| no_cross_family_guardrail | aoe_payment_002 | 3 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.540 | useful |
| no_cross_family_guardrail | aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.638 | useful |
| no_cross_family_guardrail | aoe_inventory_002 | 2 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.560 | misleading |
| no_cross_family_guardrail | aoe_inventory_002 | 3 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.544 | superficial |
| no_cross_family_guardrail | aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.754 | useful |
| no_cross_family_guardrail | aoe_cache_002 | 2 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 0.722 | misleading |
| no_cross_family_guardrail | aoe_cache_002 | 3 | aoe_cache_004 | recommendation_cache_stale | scale_out | failure | 0.514 | useful |
| no_cross_family_guardrail | aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.700 | useful |
| no_cross_family_guardrail | aoe_queue_003 | 2 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 0.649 | misleading |
| no_cross_family_guardrail | aoe_queue_003 | 3 | aoe_queue_002 | order_queue_backlog | queue_reset | success | 0.523 | superficial |
| no_cross_family_guardrail | aoe_search_003 | 1 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.683 | superficial |
| no_cross_family_guardrail | aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.628 | useful |
| no_cross_family_guardrail | aoe_search_003 | 3 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 0.534 | misleading |

## Interpretation

Ablation is useful only as a directional fixture-level signal. The current
fixture is synthetic and clean, so any variant that performs perfectly
still needs to be challenged with noisy distractor episodes and public or
lab-generated data.

The most important comparison is `full_guarded` versus
`no_action_outcome`. If removing action-outcome evidence does not change
recommendations or safety metrics, the fixture is not yet proving the core
research claim strongly enough.

## Next Steps

1. Add or refine a context-conditioned outcome gate for noisy same-family distractors.
2. Map fixture episodes into ZeptoDB tables for SQL-backed replay.
3. Add operator-facing explanations for misleading or suppressed historical evidence.
