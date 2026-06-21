# ActionOutcomeReplay Ablation Results

Generated at: 2026-06-14T00:45:34Z
Fixtures:
- `docs/research/fixtures/action_outcome_episodes.json`
- Quality labels: none

## Summary

| Variant | Removed Signals | Top-3 Hit Rate | Failed-Action Avoidance | Cross-Family Top3 | Weak Cross-Family Top3 | Top Action Changes | Labeled Top3 Quality |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| full_guarded | none | 1.00 | 1.00 | 0 | 0 | 0 | unlabeled:18 |
| no_symptom | symptom | 1.00 | 1.00 | 0 | 0 | 0 | unlabeled:18 |
| no_temporal | temporal_motif | 1.00 | 1.00 | 0 | 0 | 0 | unlabeled:18 |
| no_topology | topology | 1.00 | 1.00 | 0 | 0 | 0 | unlabeled:18 |
| no_change | change | 1.00 | 1.00 | 0 | 0 | 0 | unlabeled:18 |
| no_action_outcome | action_outcome | 1.00 | 1.00 | 0 | 0 | 1 | unlabeled:18 |
| no_text | postmortem_text | 1.00 | 1.00 | 0 | 0 | 0 | unlabeled:18 |
| no_recency | recency | 1.00 | 1.00 | 0 | 0 | 0 | unlabeled:18 |
| no_risk | risk_penalty | 1.00 | 1.00 | 0 | 0 | 1 | unlabeled:18 |
| no_cross_family_guardrail | cross_family_penalty | 1.00 | 0.83 | 1 | 1 | 1 | unlabeled:18 |

## Per-Query Top Action Changes

| Query | Full Guarded | no_symptom | no_temporal | no_topology | no_change | no_action_outcome | no_text | no_recency | no_risk | no_cross_family_guardrail |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| aoe_checkout_002 | rollback | rollback | rollback | rollback | rollback | rollback | rollback | rollback | rollback | rollback |
| aoe_payment_002 | restart | restart | restart | restart | restart | restart | restart | restart | traffic_drain | restart |
| aoe_inventory_002 | config_revert | config_revert | config_revert | config_revert | config_revert | config_revert | config_revert | config_revert | config_revert | config_revert |
| aoe_cache_002 | cache_purge | cache_purge | cache_purge | cache_purge | cache_purge | cache_purge | cache_purge | cache_purge | cache_purge | cache_purge |
| aoe_queue_003 | scale_out | scale_out | scale_out | scale_out | scale_out | scale_out | scale_out | scale_out | scale_out | restart |
| aoe_search_003 | rollback | rollback | rollback | rollback | rollback | restart | rollback | rollback | rollback | rollback |

## Per-Variant Recommendations

### full_guarded

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_payment_002 | restart:1.03, traffic_drain:1.03, rollback:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_inventory_002 | config_revert:1.03, restart:1.03, traffic_drain:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_cache_002 | cache_purge:1.03, rollback:1.03, scale_out:-0.95 | yes | yes | 0 | unlabeled:3 |
| aoe_queue_003 | scale_out:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_search_003 | rollback:1.03, restart:0.48, traffic_drain:0.48 | yes | yes | 0 | unlabeled:3 |

### no_symptom

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_payment_002 | restart:1.03, traffic_drain:1.03, rollback:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_inventory_002 | config_revert:1.03, restart:1.03, traffic_drain:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_cache_002 | cache_purge:1.03, rollback:1.03, scale_out:-0.95 | yes | yes | 0 | unlabeled:3 |
| aoe_queue_003 | scale_out:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_search_003 | rollback:1.03, traffic_drain:0.48, restart:0.48 | yes | yes | 0 | unlabeled:3 |

### no_temporal

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_payment_002 | restart:1.03, traffic_drain:1.03, rollback:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_inventory_002 | config_revert:1.03, restart:1.03, traffic_drain:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_cache_002 | cache_purge:1.03, rollback:1.03, scale_out:-0.95 | yes | yes | 0 | unlabeled:3 |
| aoe_queue_003 | scale_out:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_search_003 | rollback:1.03, traffic_drain:0.48, restart:0.48 | yes | yes | 0 | unlabeled:3 |

### no_topology

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_payment_002 | restart:1.03, traffic_drain:1.03, rollback:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_inventory_002 | config_revert:1.03, restart:1.03, traffic_drain:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_cache_002 | cache_purge:1.03, rollback:1.03, scale_out:-0.95 | yes | yes | 0 | unlabeled:3 |
| aoe_queue_003 | scale_out:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_search_003 | rollback:1.03, restart:0.48, traffic_drain:0.48 | yes | yes | 0 | unlabeled:3 |

### no_change

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_payment_002 | restart:1.03, traffic_drain:1.03, rollback:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_inventory_002 | config_revert:1.03, restart:1.03, traffic_drain:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_cache_002 | cache_purge:1.03, rollback:1.03, scale_out:-0.95 | yes | yes | 0 | unlabeled:3 |
| aoe_queue_003 | scale_out:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_search_003 | rollback:1.03, restart:0.48, traffic_drain:0.48 | yes | yes | 0 | unlabeled:3 |

### no_action_outcome

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, scale_out:1.03, config_revert:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_payment_002 | restart:1.03, traffic_drain:1.03, rollback:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_inventory_002 | config_revert:1.03, restart:1.03, traffic_drain:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_cache_002 | cache_purge:1.03, scale_out:1.03, rollback:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_queue_003 | scale_out:1.03, queue_reset:1.03, traffic_drain:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_search_003 | restart:1.03, rollback:1.03, traffic_drain:1.03 | yes | yes | 0 | unlabeled:3 |

### no_text

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_payment_002 | restart:1.03, traffic_drain:1.03, rollback:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_inventory_002 | config_revert:1.03, restart:1.03, traffic_drain:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_cache_002 | cache_purge:1.03, rollback:1.03, scale_out:-0.95 | yes | yes | 0 | unlabeled:3 |
| aoe_queue_003 | scale_out:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_search_003 | rollback:1.03, traffic_drain:0.48, restart:0.48 | yes | yes | 0 | unlabeled:3 |

### no_recency

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_payment_002 | restart:1.03, traffic_drain:1.03, rollback:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_inventory_002 | config_revert:1.03, restart:1.03, traffic_drain:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_cache_002 | cache_purge:1.03, rollback:1.03, scale_out:-0.95 | yes | yes | 0 | unlabeled:3 |
| aoe_queue_003 | scale_out:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_search_003 | rollback:1.03, restart:0.48, traffic_drain:0.48 | yes | yes | 0 | unlabeled:3 |

### no_risk

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.03, config_revert:1.03, scale_out:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_payment_002 | traffic_drain:1.03, restart:1.03, rollback:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_inventory_002 | config_revert:1.03, restart:1.03, traffic_drain:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_cache_002 | cache_purge:1.03, rollback:1.03, scale_out:-0.95 | yes | yes | 0 | unlabeled:3 |
| aoe_queue_003 | scale_out:1.03, queue_reset:1.03, traffic_drain:0.48 | yes | yes | 0 | unlabeled:3 |
| aoe_search_003 | rollback:1.03, restart:0.48, traffic_drain:0.48 | yes | yes | 0 | unlabeled:3 |

### no_cross_family_guardrail

| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |
| --- | --- | --- | --- | ---: | --- |
| aoe_checkout_002 | rollback:1.06, config_revert:1.03, restart:0.75 | yes | yes | 0 | unlabeled:3 |
| aoe_payment_002 | restart:1.04, rollback:1.04, config_revert:1.04 | yes | yes | 0 | unlabeled:3 |
| aoe_inventory_002 | config_revert:1.04, restart:1.04, traffic_drain:1.04 | yes | yes | 0 | unlabeled:3 |
| aoe_cache_002 | cache_purge:1.03, rollback:1.03, config_revert:1.03 | yes | yes | 0 | unlabeled:3 |
| aoe_queue_003 | restart:1.04, scale_out:1.03, queue_reset:1.03 | yes | no | 0 | unlabeled:3 |
| aoe_search_003 | rollback:1.06, scale_out:0.48, traffic_drain:0.48 | yes | yes | 1 | unlabeled:3 |

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
