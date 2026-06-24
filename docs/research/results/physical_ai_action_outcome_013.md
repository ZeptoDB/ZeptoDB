# Physical AI Action-Outcome Experiment 013 Results

Generated at: 2026-06-23T13:08:53Z
Fixture: `docs/research/fixtures/physical_ai_action_outcome_episodes.json`
Classification: Research-only

## Purpose

Experiment 013 compares similar robot incident retrieval, runbook/action-prior
recommendation, reflection-only memory, and context-gated Physical AI
Action-Outcome Memory. The goal is to test whether structured temporal
context gates avoid risky repeated actions while still selecting useful
recovery actions.

## Summary

| Variant | Top-3 Safe Hit | Recovery Top-1 Hit | Risky Repeat Avoidance | Hazardous Top Action | Useful Top3 | Misleading Top3 | Top Action Changes vs Context Gate | Suppressions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| similar_robot_incident | 1.00 | 0.00 | 0.00 | 1.00 | 10 | 5 | 5 | 0 |
| runbook_action_prior | 1.00 | 0.00 | 0.00 | 1.00 | 8 | 7 | 5 | 0 |
| reflection_only_memory | 1.00 | 0.00 | 0.00 | 1.00 | 10 | 5 | 5 | 0 |
| context_gated_physical_ai_action_outcome | 1.00 | 1.00 | 1.00 | 0.00 | 10 | 5 | 0 | 32 |

## Variant Definitions

| Variant | Interpretation |
| --- | --- |
| `similar_robot_incident` | Similar robot incident retrieval without outcome-aware action learning. |
| `runbook_action_prior` | Same-incident-family action prior ranked by historical outcome and recency. |
| `reflection_only_memory` | Reflection-style experiential memory using text and outcomes but no structured context gate. |
| `context_gated_physical_ai_action_outcome` | Structured Physical AI Action-Outcome Memory with topology, motif, and change-context outcome gates. |

## Per-Query Action Comparison

| Query | Unsafe Query Action | similar_robot_incident | runbook_action_prior | reflection_only_memory | context_gated_physical_ai_action_outcome |
| --- | --- | --- | --- | --- | --- |
| pai_agv_slip_002 | continue_route | continue_route | continue_route | continue_route | reroute_zone |
| pai_lidar_002 | speed_up_clear_zone | speed_up_clear_zone | speed_up_clear_zone | speed_up_clear_zone | safe_stop_clean_lens |
| pai_arm_002 | increase_torque_limit | increase_torque_limit | increase_torque_limit | increase_torque_limit | pause_recalibrate |
| pai_cold_002 | ignore_until_checkpoint | ignore_until_checkpoint | ignore_until_checkpoint | ignore_until_checkpoint | reroute_cold_dock |
| pai_drone_002 | continue_mission | continue_mission | continue_mission | continue_mission | return_to_base |

## Per-Variant Recommendations

### similar_robot_incident

| Query | Top Actions | Safe Top-3 Hit | Recovery Top-1 Hit | Avoids Risky Repeat | Labeled Top3 Quality |
| --- | --- | --- | --- | --- | --- |
| pai_agv_slip_002 | continue_route:1.02, reroute_zone:1.01, stop_and_inspect:1.01 | yes | no | no | misleading:1, useful:2 |
| pai_lidar_002 | speed_up_clear_zone:1.02, safe_stop_clean_lens:1.01, switch_sensor_mode:1.01 | yes | no | no | misleading:1, useful:2 |
| pai_arm_002 | increase_torque_limit:1.02, pause_recalibrate:1.01, reduce_speed:1.01 | yes | no | no | misleading:1, useful:2 |
| pai_cold_002 | ignore_until_checkpoint:1.02, continue_route:1.02, reroute_cold_dock:1.01 | yes | no | no | misleading:1, useful:2 |
| pai_drone_002 | continue_mission:1.02, speed_up_clear_zone:1.02, return_to_base:1.01 | yes | no | no | misleading:1, useful:2 |

### runbook_action_prior

| Query | Top Actions | Safe Top-3 Hit | Recovery Top-1 Hit | Avoids Risky Repeat | Labeled Top3 Quality |
| --- | --- | --- | --- | --- | --- |
| pai_agv_slip_002 | continue_route:1.02, reroute_zone:1.01, stop_and_inspect:1.01 | yes | no | no | misleading:1, useful:2 |
| pai_lidar_002 | speed_up_clear_zone:1.02, safe_stop_clean_lens:1.01, switch_sensor_mode:1.01 | yes | no | no | misleading:2, useful:1 |
| pai_arm_002 | increase_torque_limit:1.02, pause_recalibrate:1.01, reduce_speed:0.46 | yes | no | no | misleading:2, useful:1 |
| pai_cold_002 | ignore_until_checkpoint:1.02, reroute_cold_dock:1.01, inspect_door_seal:1.01 | yes | no | no | misleading:1, useful:2 |
| pai_drone_002 | continue_mission:1.02, return_to_base:1.01, switch_vision_nav:1.01 | yes | no | no | misleading:1, useful:2 |

### reflection_only_memory

| Query | Top Actions | Safe Top-3 Hit | Recovery Top-1 Hit | Avoids Risky Repeat | Labeled Top3 Quality |
| --- | --- | --- | --- | --- | --- |
| pai_agv_slip_002 | continue_route:1.02, reroute_zone:1.01, stop_and_inspect:1.01 | yes | no | no | misleading:1, useful:2 |
| pai_lidar_002 | speed_up_clear_zone:1.02, safe_stop_clean_lens:1.01, switch_sensor_mode:1.01 | yes | no | no | misleading:1, useful:2 |
| pai_arm_002 | increase_torque_limit:1.02, pause_recalibrate:1.01, reroute_zone:1.01 | yes | no | no | misleading:1, useful:2 |
| pai_cold_002 | ignore_until_checkpoint:1.02, reroute_cold_dock:1.01, inspect_door_seal:1.01 | yes | no | no | misleading:1, useful:2 |
| pai_drone_002 | continue_mission:1.02, speed_up_clear_zone:1.02, return_to_base:1.01 | yes | no | no | misleading:1, useful:2 |

### context_gated_physical_ai_action_outcome

| Query | Top Actions | Safe Top-3 Hit | Recovery Top-1 Hit | Avoids Risky Repeat | Labeled Top3 Quality |
| --- | --- | --- | --- | --- | --- |
| pai_agv_slip_002 | reroute_zone:1.01, stop_and_inspect:1.01, continue_route:0.14 | yes | yes | yes | misleading:1, useful:2 |
| pai_lidar_002 | safe_stop_clean_lens:1.01, switch_sensor_mode:0.46, speed_up_clear_zone:0.14 | yes | yes | yes | misleading:1, useful:2 |
| pai_arm_002 | pause_recalibrate:1.01, reduce_speed:0.46, increase_torque_limit:0.14 | yes | yes | yes | misleading:1, useful:2 |
| pai_cold_002 | reroute_cold_dock:1.01, inspect_door_seal:0.46, ignore_until_checkpoint:0.14 | yes | yes | yes | misleading:1, useful:2 |
| pai_drone_002 | return_to_base:1.01, switch_vision_nav:1.01, continue_mission:0.14 | yes | yes | yes | misleading:1, useful:2 |

## Labeled Top-3 Retrieval Details

| Variant | Query | Rank | Candidate | Incident Type | Action | Outcome | Score | Quality |
| --- | --- | ---: | --- | --- | --- | --- | ---: | --- |
| similar_robot_incident | pai_agv_slip_002 | 1 | pai_agv_slip_001 | warehouse_agv_dock_slip | reroute_zone | success | 0.914 | useful |
| similar_robot_incident | pai_agv_slip_002 | 2 | pai_agv_slip_hard_001 | warehouse_agv_dock_slip | continue_route | success | 0.773 | misleading |
| similar_robot_incident | pai_agv_slip_002 | 3 | pai_agv_slip_004 | warehouse_agv_dock_slip | stop_and_inspect | success | 0.665 | useful |
| similar_robot_incident | pai_lidar_002 | 1 | pai_lidar_001 | mobile_robot_lidar_occlusion | safe_stop_clean_lens | success | 0.864 | useful |
| similar_robot_incident | pai_lidar_002 | 2 | pai_lidar_hard_001 | mobile_robot_lidar_occlusion | speed_up_clear_zone | success | 0.702 | misleading |
| similar_robot_incident | pai_lidar_002 | 3 | pai_lidar_004 | mobile_robot_lidar_occlusion | switch_sensor_mode | success | 0.553 | useful |
| similar_robot_incident | pai_arm_002 | 1 | pai_arm_001 | robot_arm_torque_spike | pause_recalibrate | success | 0.917 | useful |
| similar_robot_incident | pai_arm_002 | 2 | pai_arm_004 | robot_arm_torque_spike | reduce_speed | partial_success | 0.751 | useful |
| similar_robot_incident | pai_arm_002 | 3 | pai_arm_hard_001 | robot_arm_torque_spike | increase_torque_limit | success | 0.641 | misleading |
| similar_robot_incident | pai_cold_002 | 1 | pai_cold_001 | cold_chain_temperature_excursion | reroute_cold_dock | success | 0.936 | useful |
| similar_robot_incident | pai_cold_002 | 2 | pai_cold_hard_001 | cold_chain_temperature_excursion | ignore_until_checkpoint | success | 0.728 | misleading |
| similar_robot_incident | pai_cold_002 | 3 | pai_cold_004 | cold_chain_temperature_excursion | inspect_door_seal | success | 0.485 | useful |
| similar_robot_incident | pai_drone_002 | 1 | pai_drone_001 | drone_gps_drift_near_boundary | return_to_base | success | 0.924 | useful |
| similar_robot_incident | pai_drone_002 | 2 | pai_drone_hard_001 | drone_gps_drift_near_boundary | continue_mission | success | 0.729 | misleading |
| similar_robot_incident | pai_drone_002 | 3 | pai_drone_004 | drone_gps_drift_near_boundary | switch_vision_nav | success | 0.620 | useful |
| runbook_action_prior | pai_agv_slip_002 | 1 | pai_agv_slip_001 | warehouse_agv_dock_slip | reroute_zone | success | 1.097 | useful |
| runbook_action_prior | pai_agv_slip_002 | 2 | pai_agv_slip_hard_001 | warehouse_agv_dock_slip | continue_route | success | 1.094 | misleading |
| runbook_action_prior | pai_agv_slip_002 | 3 | pai_agv_slip_004 | warehouse_agv_dock_slip | stop_and_inspect | success | 1.091 | useful |
| runbook_action_prior | pai_lidar_002 | 1 | pai_lidar_hard_001 | mobile_robot_lidar_occlusion | speed_up_clear_zone | success | 1.089 | misleading |
| runbook_action_prior | pai_lidar_002 | 2 | pai_lidar_001 | mobile_robot_lidar_occlusion | safe_stop_clean_lens | success | 1.089 | useful |
| runbook_action_prior | pai_lidar_002 | 3 | pai_lidar_003 | mobile_robot_lidar_occlusion | speed_up_clear_zone | success | 1.087 | misleading |
| runbook_action_prior | pai_arm_002 | 1 | pai_arm_001 | robot_arm_torque_spike | pause_recalibrate | success | 1.096 | useful |
| runbook_action_prior | pai_arm_002 | 2 | pai_arm_hard_001 | robot_arm_torque_spike | increase_torque_limit | success | 1.093 | misleading |
| runbook_action_prior | pai_arm_002 | 3 | pai_arm_003 | robot_arm_torque_spike | increase_torque_limit | success | 1.091 | misleading |
| runbook_action_prior | pai_cold_002 | 1 | pai_cold_001 | cold_chain_temperature_excursion | reroute_cold_dock | success | 1.098 | useful |
| runbook_action_prior | pai_cold_002 | 2 | pai_cold_hard_001 | cold_chain_temperature_excursion | ignore_until_checkpoint | success | 1.092 | misleading |
| runbook_action_prior | pai_cold_002 | 3 | pai_cold_004 | cold_chain_temperature_excursion | inspect_door_seal | success | 1.089 | useful |
| runbook_action_prior | pai_drone_002 | 1 | pai_drone_001 | drone_gps_drift_near_boundary | return_to_base | success | 1.097 | useful |
| runbook_action_prior | pai_drone_002 | 2 | pai_drone_004 | drone_gps_drift_near_boundary | switch_vision_nav | success | 1.087 | useful |
| runbook_action_prior | pai_drone_002 | 3 | pai_drone_hard_001 | drone_gps_drift_near_boundary | continue_mission | success | 1.087 | misleading |
| reflection_only_memory | pai_agv_slip_002 | 1 | pai_agv_slip_001 | warehouse_agv_dock_slip | reroute_zone | success | 0.680 | useful |
| reflection_only_memory | pai_agv_slip_002 | 2 | pai_agv_slip_hard_001 | warehouse_agv_dock_slip | continue_route | success | 0.629 | misleading |
| reflection_only_memory | pai_agv_slip_002 | 3 | pai_agv_slip_004 | warehouse_agv_dock_slip | stop_and_inspect | success | 0.468 | useful |
| reflection_only_memory | pai_lidar_002 | 1 | pai_lidar_hard_001 | mobile_robot_lidar_occlusion | speed_up_clear_zone | success | 0.560 | misleading |
| reflection_only_memory | pai_lidar_002 | 2 | pai_lidar_001 | mobile_robot_lidar_occlusion | safe_stop_clean_lens | success | 0.560 | useful |
| reflection_only_memory | pai_lidar_002 | 3 | pai_lidar_004 | mobile_robot_lidar_occlusion | switch_sensor_mode | success | 0.290 | useful |
| reflection_only_memory | pai_arm_002 | 1 | pai_arm_001 | robot_arm_torque_spike | pause_recalibrate | success | 0.670 | useful |
| reflection_only_memory | pai_arm_002 | 2 | pai_arm_hard_001 | robot_arm_torque_spike | increase_torque_limit | success | 0.614 | misleading |
| reflection_only_memory | pai_arm_002 | 3 | pai_arm_004 | robot_arm_torque_spike | reduce_speed | partial_success | 0.441 | useful |
| reflection_only_memory | pai_cold_002 | 1 | pai_cold_001 | cold_chain_temperature_excursion | reroute_cold_dock | success | 0.705 | useful |
| reflection_only_memory | pai_cold_002 | 2 | pai_cold_hard_001 | cold_chain_temperature_excursion | ignore_until_checkpoint | success | 0.600 | misleading |
| reflection_only_memory | pai_cold_002 | 3 | pai_cold_004 | cold_chain_temperature_excursion | inspect_door_seal | success | 0.346 | useful |
| reflection_only_memory | pai_drone_002 | 1 | pai_drone_001 | drone_gps_drift_near_boundary | return_to_base | success | 0.686 | useful |
| reflection_only_memory | pai_drone_002 | 2 | pai_drone_hard_001 | drone_gps_drift_near_boundary | continue_mission | success | 0.525 | misleading |
| reflection_only_memory | pai_drone_002 | 3 | pai_drone_004 | drone_gps_drift_near_boundary | switch_vision_nav | success | 0.370 | useful |
| context_gated_physical_ai_action_outcome | pai_agv_slip_002 | 1 | pai_agv_slip_001 | warehouse_agv_dock_slip | reroute_zone | success | 0.914 | useful |
| context_gated_physical_ai_action_outcome | pai_agv_slip_002 | 2 | pai_agv_slip_hard_001 | warehouse_agv_dock_slip | continue_route | success | 0.773 | misleading |
| context_gated_physical_ai_action_outcome | pai_agv_slip_002 | 3 | pai_agv_slip_004 | warehouse_agv_dock_slip | stop_and_inspect | success | 0.665 | useful |
| context_gated_physical_ai_action_outcome | pai_lidar_002 | 1 | pai_lidar_001 | mobile_robot_lidar_occlusion | safe_stop_clean_lens | success | 0.864 | useful |
| context_gated_physical_ai_action_outcome | pai_lidar_002 | 2 | pai_lidar_hard_001 | mobile_robot_lidar_occlusion | speed_up_clear_zone | success | 0.702 | misleading |
| context_gated_physical_ai_action_outcome | pai_lidar_002 | 3 | pai_lidar_004 | mobile_robot_lidar_occlusion | switch_sensor_mode | success | 0.553 | useful |
| context_gated_physical_ai_action_outcome | pai_arm_002 | 1 | pai_arm_001 | robot_arm_torque_spike | pause_recalibrate | success | 0.917 | useful |
| context_gated_physical_ai_action_outcome | pai_arm_002 | 2 | pai_arm_004 | robot_arm_torque_spike | reduce_speed | partial_success | 0.751 | useful |
| context_gated_physical_ai_action_outcome | pai_arm_002 | 3 | pai_arm_hard_001 | robot_arm_torque_spike | increase_torque_limit | success | 0.641 | misleading |
| context_gated_physical_ai_action_outcome | pai_cold_002 | 1 | pai_cold_001 | cold_chain_temperature_excursion | reroute_cold_dock | success | 0.936 | useful |
| context_gated_physical_ai_action_outcome | pai_cold_002 | 2 | pai_cold_hard_001 | cold_chain_temperature_excursion | ignore_until_checkpoint | success | 0.728 | misleading |
| context_gated_physical_ai_action_outcome | pai_cold_002 | 3 | pai_cold_004 | cold_chain_temperature_excursion | inspect_door_seal | success | 0.485 | useful |
| context_gated_physical_ai_action_outcome | pai_drone_002 | 1 | pai_drone_001 | drone_gps_drift_near_boundary | return_to_base | success | 0.924 | useful |
| context_gated_physical_ai_action_outcome | pai_drone_002 | 2 | pai_drone_hard_001 | drone_gps_drift_near_boundary | continue_mission | success | 0.729 | misleading |
| context_gated_physical_ai_action_outcome | pai_drone_002 | 3 | pai_drone_004 | drone_gps_drift_near_boundary | switch_vision_nav | success | 0.620 | useful |

## Context-Gate Suppressions

| Query | Candidate | Action | Outcome | Raw Value | Gated Value | Multiplier | Context Score | Reasons |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| pai_agv_slip_002 | pai_agv_slip_hard_001 | continue_route | success | 1.00 | 0.12 | 0.12 | 0.724 | topology_mismatch, change_context_mismatch, query_marks_action_unsafe |
| pai_agv_slip_002 | pai_agv_slip_003 | continue_route | success | 1.00 | 0.12 | 0.12 | 0.308 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch, query_marks_action_unsafe |
| pai_agv_slip_002 | pai_cold_002 | ignore_until_checkpoint | failure | -0.70 | -0.08 | 0.12 | 0.095 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_agv_slip_002 | pai_cold_001 | reroute_cold_dock | success | 1.00 | 0.12 | 0.12 | 0.092 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_agv_slip_002 | pai_arm_004 | reduce_speed | partial_success | 0.45 | 0.05 | 0.12 | 0.096 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_agv_slip_002 | pai_lidar_001 | safe_stop_clean_lens | success | 1.00 | 0.12 | 0.12 | 0.095 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_lidar_002 | pai_lidar_hard_001 | speed_up_clear_zone | success | 1.00 | 0.12 | 0.12 | 0.637 | topology_mismatch, change_context_mismatch, query_marks_action_unsafe |
| pai_lidar_002 | pai_lidar_004 | switch_sensor_mode | success | 1.00 | 0.45 | 0.45 | 0.793 | temporal_motif_mismatch |
| pai_lidar_002 | pai_lidar_003 | speed_up_clear_zone | success | 1.00 | 0.12 | 0.12 | 0.217 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch, query_marks_action_unsafe |
| pai_lidar_002 | pai_arm_002 | increase_torque_limit | unsafe_failure | -1.00 | -0.12 | 0.12 | 0.090 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_lidar_002 | pai_agv_slip_002 | continue_route | unsafe_failure | -1.00 | -0.12 | 0.12 | 0.088 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_lidar_002 | pai_arm_001 | pause_recalibrate | success | 1.00 | 0.12 | 0.12 | 0.090 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_lidar_002 | pai_arm_004 | reduce_speed | partial_success | 0.45 | 0.05 | 0.12 | 0.084 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_arm_002 | pai_arm_hard_001 | increase_torque_limit | success | 1.00 | 0.12 | 0.12 | 0.458 | topology_mismatch, change_context_mismatch, query_marks_action_unsafe |
| pai_arm_002 | pai_arm_003 | increase_torque_limit | success | 1.00 | 0.12 | 0.12 | 0.223 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch, query_marks_action_unsafe |
| pai_arm_002 | pai_lidar_002 | speed_up_clear_zone | unsafe_failure | -1.00 | -0.12 | 0.12 | 0.090 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_arm_002 | pai_agv_slip_002 | continue_route | unsafe_failure | -1.00 | -0.12 | 0.12 | 0.081 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_arm_002 | pai_lidar_001 | safe_stop_clean_lens | success | 1.00 | 0.12 | 0.12 | 0.084 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_arm_002 | pai_agv_slip_001 | reroute_zone | success | 1.00 | 0.12 | 0.12 | 0.078 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_cold_002 | pai_cold_hard_001 | ignore_until_checkpoint | success | 1.00 | 0.12 | 0.12 | 0.622 | topology_mismatch, change_context_mismatch, query_marks_action_unsafe |
| pai_cold_002 | pai_cold_004 | inspect_door_seal | success | 1.00 | 0.45 | 0.45 | 0.634 | topology_mismatch, temporal_motif_mismatch |
| pai_cold_002 | pai_cold_003 | ignore_until_checkpoint | success | 1.00 | 0.12 | 0.12 | 0.301 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch, query_marks_action_unsafe |
| pai_cold_002 | pai_agv_slip_004 | stop_and_inspect | success | 1.00 | 0.12 | 0.12 | 0.104 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_cold_002 | pai_agv_slip_001 | reroute_zone | success | 1.00 | 0.12 | 0.12 | 0.101 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_cold_002 | pai_agv_slip_hard_001 | continue_route | success | 1.00 | 0.12 | 0.12 | 0.152 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_cold_002 | pai_agv_slip_002 | continue_route | unsafe_failure | -1.00 | -0.12 | 0.12 | 0.095 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_drone_002 | pai_drone_hard_001 | continue_mission | success | 1.00 | 0.12 | 0.12 | 0.715 | topology_mismatch, change_context_mismatch, query_marks_action_unsafe |
| pai_drone_002 | pai_drone_003 | continue_mission | success | 1.00 | 0.12 | 0.12 | 0.275 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch, query_marks_action_unsafe |
| pai_drone_002 | pai_lidar_003 | speed_up_clear_zone | success | 1.00 | 0.12 | 0.12 | 0.093 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_drone_002 | pai_lidar_hard_001 | speed_up_clear_zone | success | 1.00 | 0.12 | 0.12 | 0.095 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_drone_002 | pai_agv_slip_hard_001 | continue_route | success | 1.00 | 0.12 | 0.12 | 0.089 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |
| pai_drone_002 | pai_arm_hard_001 | increase_torque_limit | success | 1.00 | 0.12 | 0.12 | 0.087 | topology_mismatch, change_context_mismatch, temporal_motif_mismatch |

## Interpretation

The strongest signal is recovery Top-1 hit rate, not raw Top-3 retrieval.
The context-gated variant can still retrieve misleading robot incidents
for audit, but it down-weights outcomes from mismatched topology, temporal
motifs, and change context before action aggregation.

In this fixture, the gated variant is the only method that reaches perfect
risky-repeat avoidance and perfect recovery-action selection across all
five Physical AI incident families.

## Next Best Step

Replay the fixture through native ZeptoDB SQL tables with ROS-style telemetry
windows, ASOF JOINs, action/outcome joins, and ROW_NUMBER ranking so the
Python-only comparison becomes auditable through the same SQL path used by
the AIOps Action-Outcome experiments.
