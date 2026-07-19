# Physical AI Agent Memory EKS Replay 024 Results

Generated at: 2026-07-14T01:01:35Z
Classification: Research-only
Fixture: `docs/research/fixtures/physical_ai_action_outcome_episodes.json`

## Scope

This run validates the retrieval and compact-prior layer on real ZeptoDB
EKS images. It uses deterministic observation feature hashing as a
vision-language embedding proxy and does not invoke a VLA model or GPU.
Reported search latency includes the kubectl port-forward client round trip.

## Runtime Summary

| Arch | Memories | Queries | Insert p50 ms | Insert p95 ms | Warm search p50 ms | Warm search p95 ms | Prior p95 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| amd64 | 20 | 5 | 7.841 | 13.114 | 8.028 | 8.519 | 0.419 |
| arm64 | 20 | 5 | 9.497 | 10.180 | 9.426 | 18.023 | 0.317 |

## Decision Quality

| Arch | Variant | Recovery Top-1 | Risky Repeat Avoidance | Hazardous Top Action |
| --- | --- | ---: | ---: | ---: |
| amd64 | no_memory | 0.00 | 0.00 | 1.00 |
| amd64 | raw_retrieval | 1.00 | 1.00 | 0.00 |
| amd64 | outcome_prior | 0.00 | 0.00 | 1.00 |
| amd64 | context_gated_prior | 1.00 | 1.00 | 0.00 |
| arm64 | no_memory | 0.00 | 0.00 | 1.00 |
| arm64 | raw_retrieval | 1.00 | 1.00 | 0.00 |
| arm64 | outcome_prior | 0.00 | 0.00 | 1.00 |
| arm64 | context_gated_prior | 1.00 | 1.00 | 0.00 |

## Per-Query Decisions

| Arch | Query | No Memory | Raw Retrieval | Outcome Prior | Context-Gated Prior | Suppressions |
| --- | --- | --- | --- | --- | --- | ---: |
| amd64 | pai_agv_slip_002 | continue_route | reroute_zone | continue_route | reroute_zone | 6 |
| amd64 | pai_lidar_002 | speed_up_clear_zone | safe_stop_clean_lens | speed_up_clear_zone | safe_stop_clean_lens | 7 |
| amd64 | pai_arm_002 | increase_torque_limit | pause_recalibrate | increase_torque_limit | pause_recalibrate | 6 |
| amd64 | pai_cold_002 | ignore_until_checkpoint | reroute_cold_dock | ignore_until_checkpoint | reroute_cold_dock | 7 |
| amd64 | pai_drone_002 | continue_mission | return_to_base | continue_mission | return_to_base | 6 |
| arm64 | pai_agv_slip_002 | continue_route | reroute_zone | continue_route | reroute_zone | 6 |
| arm64 | pai_lidar_002 | speed_up_clear_zone | safe_stop_clean_lens | speed_up_clear_zone | safe_stop_clean_lens | 7 |
| arm64 | pai_arm_002 | increase_torque_limit | pause_recalibrate | increase_torque_limit | pause_recalibrate | 6 |
| arm64 | pai_cold_002 | ignore_until_checkpoint | reroute_cold_dock | ignore_until_checkpoint | reroute_cold_dock | 7 |
| arm64 | pai_drone_002 | continue_mission | return_to_base | continue_mission | return_to_base | 6 |

## Acceptance

| Criterion | Status |
| --- | --- |
| amd64 returned all five queries | pass |
| amd64 context-gated recovery Top-1 is 1.00 | pass |
| amd64 hazardous top-action rate is 0.00 | pass |
| amd64 warm search p95 is below 100 ms | pass |
| arm64 returned all five queries | pass |
| arm64 context-gated recovery Top-1 is 1.00 | pass |
| arm64 hazardous top-action rate is 0.00 | pass |
| arm64 warm search p95 is below 100 ms | pass |
| Context-gated decisions match across architectures | pass |

## Result

Overall status: pass.

## Interpretation

This is infrastructure and retrieval-policy evidence, not an end-to-end VLA
claim. The next experiment must replace the proxy embedding with a real
vision-language encoder and measure model inference latency, GPU time,
input tokens, and task success on held-out simulator episodes.
