# ActionOutcomeReplay Experiment 010 Vendor Baseline Results

Generated at: 2026-06-18T14:41:54Z
Fixtures:
- `docs/research/fixtures/action_outcome_episodes.json`
- `docs/research/fixtures/action_outcome_distractor_episodes.json`
- Quality labels: `docs/research/fixtures/action_outcome_retrieval_quality_labels.json`

## Purpose

Experiment 010 compares the current context-gated Action-Outcome Memory
against vendor-inspired baselines that approximate common industry
patterns: similar-incident retrieval, runbook/action-prior
recommendation, and reflection-only experiential memory.

## Summary

| Variant | Top-3 Hit Rate | Failed-Action Avoidance | Useful Top3 | Superficial Top3 | Misleading Top3 | Cross-Family Top3 | Top Action Changes vs Context Gate | Suppressions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| similar_incident | 1.00 | 0.67 | 8 | 4 | 6 | 0 | 4 | 0 |
| runbook_action_prior | 1.00 | 0.67 | 7 | 6 | 5 | 0 | 4 | 0 |
| reflection_only_memory | 1.00 | 0.83 | 8 | 4 | 6 | 0 | 3 | 0 |
| context_gated_action_outcome | 1.00 | 1.00 | 8 | 4 | 6 | 0 | 0 | 21 |

## Variant Definitions

| Variant | Interpretation |
| --- | --- |
| `similar_incident` | Similar-incident retrieval without outcome-aware action learning. |
| `runbook_action_prior` | Same-incident-family action prior from historical outcomes. |
| `reflection_only_memory` | Reflection/postmortem-style memory using textual experience and outcome recall. |
| `context_gated_action_outcome` | Structured Action-Outcome Memory with context-conditioned outcome suppression. |

## Per-Query Action Comparison

| Query | True Failed Action | similar_incident | runbook_action_prior | reflection_only_memory | context_gated_action_outcome |
| --- | --- | --- | --- | --- | --- |
| aoe_checkout_002 | restart | rollback | rollback | rollback | rollback |
| aoe_payment_002 | scale_out | scale_out | restart | traffic_drain | traffic_drain |
| aoe_inventory_002 | scale_out | restart | restart | config_revert | config_revert |
| aoe_cache_002 | restart | restart | restart | restart | cache_purge |
| aoe_queue_003 | restart | scale_out | restart | queue_reset | scale_out |
| aoe_search_003 | scale_out | restart | rollback | restart | rollback |

## Per-Variant Recommendations

### similar_incident

| Query | Top Actions | Hit | Avoids Failed Repeat | Labeled Top3 Quality |
| --- | --- | --- | --- | --- |
| aoe_checkout_002 | rollback:1.03, scale_out:1.02, restart:1.02 | yes | yes | useful:2, superficial:1 |
| aoe_payment_002 | scale_out:1.03, restart:1.02, traffic_drain:1.01 | yes | no | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | restart:1.03, config_revert:1.02, scale_out:1.01 | yes | yes | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | restart:1.03, cache_purge:1.02, scale_out:1.01 | yes | no | useful:2, misleading:1 |
| aoe_queue_003 | scale_out:1.03, restart:1.02, queue_reset:1.01 | yes | yes | useful:1, misleading:2 |
| aoe_search_003 | restart:1.03, rollback:1.03, scale_out:1.01 | yes | yes | useful:1, superficial:1, misleading:1 |

### runbook_action_prior

| Query | Top Actions | Hit | Avoids Failed Repeat | Labeled Top3 Quality |
| --- | --- | --- | --- | --- |
| aoe_checkout_002 | rollback:1.01, config_revert:1.01, scale_out:0.46 | yes | yes | useful:2, superficial:1 |
| aoe_payment_002 | restart:1.01, traffic_drain:1.01, scale_out:1.01 | yes | yes | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | restart:1.01, config_revert:1.01, scale_out:1.01 | yes | yes | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | restart:1.01, rollback:1.01, cache_purge:1.01 | yes | no | useful:1, superficial:1, misleading:1 |
| aoe_queue_003 | restart:1.01, scale_out:1.01, queue_reset:1.01 | yes | no | useful:1, superficial:1, misleading:1 |
| aoe_search_003 | rollback:1.01, restart:0.85, traffic_drain:0.46 | yes | yes | useful:1, superficial:1, misleading:1 |

### reflection_only_memory

| Query | Top Actions | Hit | Avoids Failed Repeat | Labeled Top3 Quality |
| --- | --- | --- | --- | --- |
| aoe_checkout_002 | rollback:1.02, config_revert:1.02, restart:1.02 | yes | yes | useful:2, superficial:1 |
| aoe_payment_002 | traffic_drain:1.01, rollback:1.01, restart:0.50 | yes | yes | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | config_revert:1.02, traffic_drain:1.01, rollback:1.01 | yes | yes | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | restart:1.03, rollback:1.01, cache_purge:0.34 | yes | no | useful:2, misleading:1 |
| aoe_queue_003 | queue_reset:1.01, rollback:1.01, restart:0.60 | yes | yes | useful:1, misleading:2 |
| aoe_search_003 | restart:0.57, traffic_drain:0.46, scale_out:0.46 | yes | yes | useful:1, superficial:1, misleading:1 |

### context_gated_action_outcome

| Query | Top Actions | Hit | Avoids Failed Repeat | Labeled Top3 Quality |
| --- | --- | --- | --- | --- |
| aoe_checkout_002 | rollback:1.03, scale_out:0.31, config_revert:0.16 | yes | yes | useful:2, superficial:1 |
| aoe_payment_002 | traffic_drain:0.67, restart:0.16, scale_out:0.16 | yes | yes | useful:1, superficial:1, misleading:1 |
| aoe_inventory_002 | config_revert:0.36, traffic_drain:0.36, scale_out:0.16 | yes | yes | useful:1, superficial:1, misleading:1 |
| aoe_cache_002 | cache_purge:0.59, restart:0.16, rollback:0.16 | yes | yes | useful:2, misleading:1 |
| aoe_queue_003 | scale_out:0.54, traffic_drain:0.17, restart:0.16 | yes | yes | useful:1, misleading:2 |
| aoe_search_003 | rollback:0.56, restart:0.36, traffic_drain:0.07 | yes | yes | useful:1, superficial:1, misleading:1 |

## Labeled Top-3 Retrieval Details

| Variant | Query | Rank | Candidate | Family | Action | Outcome | Score | Quality |
| --- | --- | ---: | --- | --- | --- | --- | ---: | --- |
| similar_incident | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.658 | useful |
| similar_incident | aoe_checkout_002 | 2 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.472 | superficial |
| similar_incident | aoe_checkout_002 | 3 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.435 | useful |
| similar_incident | aoe_payment_002 | 1 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.511 | misleading |
| similar_incident | aoe_payment_002 | 2 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.498 | superficial |
| similar_incident | aoe_payment_002 | 3 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.482 | useful |
| similar_incident | aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.609 | useful |
| similar_incident | aoe_inventory_002 | 2 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.526 | misleading |
| similar_incident | aoe_inventory_002 | 3 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.462 | superficial |
| similar_incident | aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.723 | useful |
| similar_incident | aoe_cache_002 | 2 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 0.677 | misleading |
| similar_incident | aoe_cache_002 | 3 | aoe_cache_004 | recommendation_cache_stale | scale_out | failure | 0.589 | useful |
| similar_incident | aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.671 | useful |
| similar_incident | aoe_queue_003 | 2 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 0.597 | misleading |
| similar_incident | aoe_queue_003 | 3 | aoe_distractor_queue_002 | order_queue_backlog | scale_out | rollback_required | 0.535 | misleading |
| similar_incident | aoe_search_003 | 1 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.693 | superficial |
| similar_incident | aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.587 | useful |
| similar_incident | aoe_search_003 | 3 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 0.483 | misleading |
| runbook_action_prior | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 1.034 | useful |
| runbook_action_prior | aoe_checkout_002 | 2 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 1.028 | useful |
| runbook_action_prior | aoe_checkout_002 | 3 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.485 | superficial |
| runbook_action_prior | aoe_payment_002 | 1 | aoe_payment_003 | payment_dependency_timeout | restart | success | 1.074 | superficial |
| runbook_action_prior | aoe_payment_002 | 2 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.974 | useful |
| runbook_action_prior | aoe_payment_002 | 3 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.973 | misleading |
| runbook_action_prior | aoe_inventory_002 | 1 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 1.075 | superficial |
| runbook_action_prior | aoe_inventory_002 | 2 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 1.034 | useful |
| runbook_action_prior | aoe_inventory_002 | 3 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.977 | misleading |
| runbook_action_prior | aoe_cache_002 | 1 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 1.036 | misleading |
| runbook_action_prior | aoe_cache_002 | 2 | aoe_cache_003 | recommendation_cache_stale | rollback | success | 1.036 | superficial |
| runbook_action_prior | aoe_cache_002 | 3 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 1.034 | useful |
| runbook_action_prior | aoe_queue_003 | 1 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 1.045 | misleading |
| runbook_action_prior | aoe_queue_003 | 2 | aoe_queue_001 | order_queue_backlog | scale_out | success | 1.029 | useful |
| runbook_action_prior | aoe_queue_003 | 3 | aoe_queue_002 | order_queue_backlog | queue_reset | success | 0.975 | superficial |
| runbook_action_prior | aoe_search_003 | 1 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 1.012 | misleading |
| runbook_action_prior | aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.974 | useful |
| runbook_action_prior | aoe_search_003 | 3 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.479 | superficial |
| reflection_only_memory | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.490 | useful |
| reflection_only_memory | aoe_checkout_002 | 2 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.339 | useful |
| reflection_only_memory | aoe_checkout_002 | 3 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.303 | superficial |
| reflection_only_memory | aoe_payment_002 | 1 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.328 | useful |
| reflection_only_memory | aoe_payment_002 | 2 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.318 | superficial |
| reflection_only_memory | aoe_payment_002 | 3 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.312 | misleading |
| reflection_only_memory | aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.508 | useful |
| reflection_only_memory | aoe_inventory_002 | 2 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.341 | misleading |
| reflection_only_memory | aoe_inventory_002 | 3 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.339 | superficial |
| reflection_only_memory | aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.509 | useful |
| reflection_only_memory | aoe_cache_002 | 2 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 0.467 | misleading |
| reflection_only_memory | aoe_cache_002 | 3 | aoe_cache_004 | recommendation_cache_stale | scale_out | failure | 0.425 | useful |
| reflection_only_memory | aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.440 | useful |
| reflection_only_memory | aoe_queue_003 | 2 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 0.412 | misleading |
| reflection_only_memory | aoe_queue_003 | 3 | aoe_distractor_queue_002 | order_queue_backlog | scale_out | rollback_required | 0.368 | misleading |
| reflection_only_memory | aoe_search_003 | 1 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.495 | superficial |
| reflection_only_memory | aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.395 | useful |
| reflection_only_memory | aoe_search_003 | 3 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 0.385 | misleading |
| context_gated_action_outcome | aoe_checkout_002 | 1 | aoe_checkout_001 | checkout_latency_after_deploy | rollback | success | 0.698 | useful |
| context_gated_action_outcome | aoe_checkout_002 | 2 | aoe_checkout_003 | checkout_latency_after_deploy | scale_out | partial_success | 0.494 | superficial |
| context_gated_action_outcome | aoe_checkout_002 | 3 | aoe_checkout_004 | checkout_latency_after_deploy | config_revert | success | 0.432 | useful |
| context_gated_action_outcome | aoe_payment_002 | 1 | aoe_payment_001 | payment_dependency_timeout | traffic_drain | success | 0.514 | useful |
| context_gated_action_outcome | aoe_payment_002 | 2 | aoe_payment_003 | payment_dependency_timeout | restart | success | 0.508 | superficial |
| context_gated_action_outcome | aoe_payment_002 | 3 | aoe_distractor_payment_001 | payment_dependency_timeout | scale_out | success | 0.484 | misleading |
| context_gated_action_outcome | aoe_inventory_002 | 1 | aoe_inventory_001 | inventory_db_connection_pool | config_revert | success | 0.589 | useful |
| context_gated_action_outcome | aoe_inventory_002 | 2 | aoe_distractor_inventory_001 | inventory_db_connection_pool | scale_out | success | 0.496 | misleading |
| context_gated_action_outcome | aoe_inventory_002 | 3 | aoe_inventory_003 | inventory_db_connection_pool | restart | success | 0.481 | superficial |
| context_gated_action_outcome | aoe_cache_002 | 1 | aoe_cache_001 | recommendation_cache_stale | cache_purge | success | 0.754 | useful |
| context_gated_action_outcome | aoe_cache_002 | 2 | aoe_distractor_cache_001 | recommendation_cache_stale | restart | success | 0.658 | misleading |
| context_gated_action_outcome | aoe_cache_002 | 3 | aoe_cache_004 | recommendation_cache_stale | scale_out | failure | 0.532 | useful |
| context_gated_action_outcome | aoe_queue_003 | 1 | aoe_queue_001 | order_queue_backlog | scale_out | success | 0.700 | useful |
| context_gated_action_outcome | aoe_queue_003 | 2 | aoe_distractor_queue_001 | order_queue_backlog | restart | success | 0.585 | misleading |
| context_gated_action_outcome | aoe_queue_003 | 3 | aoe_distractor_queue_002 | order_queue_backlog | scale_out | rollback_required | 0.486 | misleading |
| context_gated_action_outcome | aoe_search_003 | 1 | aoe_search_001 | search_index_memory_leak | restart | partial_success | 0.683 | superficial |
| context_gated_action_outcome | aoe_search_003 | 2 | aoe_search_002 | search_index_memory_leak | rollback | success | 0.628 | useful |
| context_gated_action_outcome | aoe_search_003 | 3 | aoe_distractor_search_001 | search_index_memory_leak | restart | success | 0.470 | misleading |

## Context-Gate Suppressions

| Query | Candidate | Action | Outcome | Raw Value | Gated Value | Multiplier | Context Score | Reasons |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| aoe_checkout_002 | aoe_checkout_003 | scale_out | partial_success | 0.45 | 0.29 | 0.65 | 0.507 | none |
| aoe_checkout_002 | aoe_checkout_004 | config_revert | success | 1.00 | 0.15 | 0.15 | 0.243 | change_type_mismatch:deploy->config |
| aoe_payment_002 | aoe_payment_001 | traffic_drain | success | 1.00 | 0.65 | 0.65 | 0.571 | none |
| aoe_payment_002 | aoe_payment_003 | restart | success | 1.00 | 0.15 | 0.15 | 0.083 | single_entity_vs_service_wide |
| aoe_payment_002 | aoe_distractor_payment_001 | scale_out | success | 1.00 | 0.15 | 0.15 | 0.196 | candidate_cpu_saturated_query_not |
| aoe_payment_002 | aoe_payment_004 | rollback | success | 1.00 | 0.15 | 0.15 | 0.210 | change_type_mismatch:none->deploy |
| aoe_inventory_002 | aoe_inventory_001 | config_revert | success | 1.00 | 0.35 | 0.35 | 0.421 | change_type_mismatch:none->config |
| aoe_inventory_002 | aoe_distractor_inventory_001 | scale_out | success | 1.00 | 0.15 | 0.15 | 0.217 | query_db_saturated_candidate_not |
| aoe_inventory_002 | aoe_inventory_003 | restart | success | 1.00 | 0.15 | 0.15 | 0.048 | single_entity_vs_service_wide |
| aoe_inventory_002 | aoe_inventory_004 | traffic_drain | success | 1.00 | 0.35 | 0.35 | 0.417 | none |
| aoe_cache_002 | aoe_distractor_cache_001 | restart | success | 1.00 | 0.15 | 0.15 | 0.309 | single_entity_vs_service_wide |
| aoe_cache_002 | aoe_cache_004 | scale_out | failure | -0.70 | -0.45 | 0.65 | 0.635 | none |
| aoe_cache_002 | aoe_distractor_cache_002 | cache_purge | failure | -0.70 | -0.10 | 0.15 | 0.060 | cache_symptom_from_deploy_not_flag, change_type_mismatch:feature_flag->deploy |
| aoe_cache_002 | aoe_cache_003 | rollback | success | 1.00 | 0.15 | 0.15 | 0.000 | cache_symptom_from_deploy_not_flag, change_type_mismatch:feature_flag->deploy |
| aoe_queue_003 | aoe_distractor_queue_001 | restart | success | 1.00 | 0.15 | 0.15 | 0.200 | single_entity_vs_service_wide |
| aoe_queue_003 | aoe_distractor_queue_002 | scale_out | rollback_required | -0.90 | -0.14 | 0.15 | 0.008 | candidate_consumer_errors_query_healthy, change_type_mismatch:traffic_spike->schema_change |
| aoe_queue_003 | aoe_queue_002 | queue_reset | success | 1.00 | 0.15 | 0.15 | 0.000 | candidate_consumer_errors_query_healthy, change_type_mismatch:traffic_spike->schema_change |
| aoe_queue_003 | aoe_queue_004 | traffic_drain | partial_success | 0.45 | 0.16 | 0.35 | 0.383 | none |
| aoe_search_003 | aoe_distractor_search_001 | restart | success | 1.00 | 0.15 | 0.15 | 0.000 | single_entity_vs_service_wide, change_type_mismatch:deploy->none |
| aoe_search_003 | aoe_distractor_search_002 | rollback | failure | -0.70 | -0.10 | 0.15 | 0.350 | change_type_mismatch:deploy->index_refresh |
| aoe_search_003 | aoe_search_004 | traffic_drain | partial_success | 0.45 | 0.07 | 0.15 | 0.194 | change_type_mismatch:deploy->index_refresh |

## Interpretation

The key comparison is not whether every approach finds at least one
historically successful action. On this fixture, that metric is too easy.
The stronger commercial signal is failed-action avoidance under noisy
same-family distractors.

`context_gated_action_outcome` is the only variant that both preserves
top-3 useful-action coverage and reaches perfect failed-action avoidance
on the noisy fixture. The suppressions show why: it refuses to reuse
positive or negative outcomes from mismatched topology, change, and metric
contexts.

## Next Best Step

Replay Experiment 010 through SQL tables after alias-aware hash JOIN
`WHERE` predicates are implemented, so the comparison can be audited with
native ZeptoDB JOIN/window queries instead of only Python fixtures.
