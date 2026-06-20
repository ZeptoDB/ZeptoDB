# ActionOutcomeReplay Experiment 005 Context Gate Results

Generated at: 2026-06-14T00:45:34Z
Fixtures:
- `docs/research/fixtures/action_outcome_episodes.json`
- `docs/research/fixtures/action_outcome_distractor_episodes.json`
- Quality labels: `docs/research/fixtures/action_outcome_retrieval_quality_labels.json`

## Summary

| Variant | Top-3 Hit Rate | Failed-Action Avoidance | Cross-Family Top3 | Weak Cross-Family Top3 | Labeled Top3 Quality | Gate Suppressions |
| --- | ---: | ---: | ---: | ---: | --- | ---: |
| full_guarded | 1.00 | 0.67 | 0 | 0 | useful:8, superficial:5, misleading:5 | 0 |
| context_gated | 1.00 | 1.00 | 0 | 0 | useful:8, superficial:4, misleading:6 | 21 |

## Per-Query Comparison

| Query | Full Guarded Top Action | Context Gated Top Action | Changed | Full Avoids Failed Repeat | Gated Avoids Failed Repeat | Gated Top Actions |
| --- | --- | --- | --- | --- | --- | --- |
| aoe_checkout_002 | rollback | rollback | no | yes | yes | rollback:1.03, scale_out:0.31, config_revert:0.16 |
| aoe_payment_002 | restart | traffic_drain | yes | yes | yes | traffic_drain:0.67, restart:0.16, scale_out:0.16 |
| aoe_inventory_002 | config_revert | config_revert | no | yes | yes | config_revert:0.36, traffic_drain:0.36, scale_out:0.16 |
| aoe_cache_002 | restart | cache_purge | yes | no | yes | cache_purge:0.59, restart:0.16, rollback:0.16 |
| aoe_queue_003 | restart | scale_out | yes | no | yes | scale_out:0.54, traffic_drain:0.17, restart:0.16 |
| aoe_search_003 | restart | rollback | yes | yes | yes | rollback:0.56, restart:0.36, traffic_drain:0.07 |

## Context-Gated Top-3 Retrieval Details

| Query | Rank | Candidate | Action | Outcome | Score | Quality | Context Multiplier | Context Score | Reasons |
| --- | ---: | --- | --- | --- | ---: | --- | ---: | ---: | --- |
| aoe_checkout_002 | 1 | aoe_checkout_001 | rollback | success | 0.698 | useful | 1.00 | 0.697 | none |
| aoe_checkout_002 | 2 | aoe_checkout_003 | scale_out | partial_success | 0.494 | superficial | 0.65 | 0.507 | none |
| aoe_checkout_002 | 3 | aoe_checkout_004 | config_revert | success | 0.432 | useful | 0.15 | 0.243 | change_type_mismatch:deploy->config |
| aoe_payment_002 | 1 | aoe_payment_001 | traffic_drain | success | 0.514 | useful | 0.65 | 0.571 | none |
| aoe_payment_002 | 2 | aoe_payment_003 | restart | success | 0.508 | superficial | 0.15 | 0.083 | single_entity_vs_service_wide |
| aoe_payment_002 | 3 | aoe_distractor_payment_001 | scale_out | success | 0.484 | misleading | 0.15 | 0.196 | candidate_cpu_saturated_query_not |
| aoe_inventory_002 | 1 | aoe_inventory_001 | config_revert | success | 0.589 | useful | 0.35 | 0.421 | change_type_mismatch:none->config |
| aoe_inventory_002 | 2 | aoe_distractor_inventory_001 | scale_out | success | 0.496 | misleading | 0.15 | 0.217 | query_db_saturated_candidate_not |
| aoe_inventory_002 | 3 | aoe_inventory_003 | restart | success | 0.481 | superficial | 0.15 | 0.048 | single_entity_vs_service_wide |
| aoe_cache_002 | 1 | aoe_cache_001 | cache_purge | success | 0.754 | useful | 1.00 | 0.774 | none |
| aoe_cache_002 | 2 | aoe_distractor_cache_001 | restart | success | 0.658 | misleading | 0.15 | 0.309 | single_entity_vs_service_wide |
| aoe_cache_002 | 3 | aoe_cache_004 | scale_out | failure | 0.532 | useful | 0.65 | 0.635 | none |
| aoe_queue_003 | 1 | aoe_queue_001 | scale_out | success | 0.700 | useful | 1.00 | 0.707 | none |
| aoe_queue_003 | 2 | aoe_distractor_queue_001 | restart | success | 0.585 | misleading | 0.15 | 0.200 | single_entity_vs_service_wide |
| aoe_queue_003 | 3 | aoe_distractor_queue_002 | scale_out | rollback_required | 0.486 | misleading | 0.15 | 0.008 | candidate_consumer_errors_query_healthy, change_type_mismatch:traffic_spike->schema_change |
| aoe_search_003 | 1 | aoe_search_001 | restart | partial_success | 0.683 | superficial | 1.00 | 0.738 | none |
| aoe_search_003 | 2 | aoe_search_002 | rollback | success | 0.628 | useful | 1.00 | 0.683 | none |
| aoe_search_003 | 3 | aoe_distractor_search_001 | restart | success | 0.470 | misleading | 0.15 | 0.000 | single_entity_vs_service_wide, change_type_mismatch:deploy->none |

## Suppressed Outcome Evidence

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

The context gate should be judged by whether it improves failed-action
avoidance under noisy same-family distractors without losing top-3
successful-action coverage. Gate suppressions are expected: they show that
the engine is refusing to reuse outcomes from incompatible contexts.

## Next Steps

1. Add a SQL-backed episode table and replay this comparison through ZeptoDB.
2. Add a learned or calibrated gate after collecting real incident traces.
3. Add operator-facing explanations for suppressed historical outcomes.
