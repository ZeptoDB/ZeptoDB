# Physical AI Action-Outcome Experiment 014 SQL Replay Results

Generated at: 2026-06-23T13:32:07Z
Endpoint: `http://127.0.0.1:19341/`
Fixture: `docs/research/fixtures/physical_ai_action_outcome_episodes.json`
SQL replay file: `docs/research/results/physical_ai_action_outcome_sql_replay_014.sql`
Classification: Research-only

## Status

- Row-count status: pass
- Failed-repeat JOIN status: pass
- Context top-action JOIN status: pass
- Suppression audit JOIN status: pass
- Action/outcome JOIN status: pass
- Robot state ASOF JOIN status: pass
- Sensor ASOF JOIN status: pass
- ROW_NUMBER window status: pass
- LAG window status: pass
- Spatial `ST_Within` status: pass
- Overall SQL replay status: pass

## Table Counts

| Table | Rows |
| --- | ---: |
| `physical_ai_action_outcomes_014` | 25 |
| `physical_ai_expected_actions_014` | 10 |
| `physical_ai_incidents_014` | 5 |
| `physical_ai_pose_014` | 5 |
| `physical_ai_recommendations_014` | 60 |
| `physical_ai_retrieval_014` | 60 |
| `physical_ai_robot_state_014` | 15 |
| `physical_ai_sensor_summary_014` | 15 |
| `physical_ai_suppressions_014` | 32 |

## Failed-Repeat JOIN

Native SQL finds every non-gated baseline that selected the known unsafe
query action as its Top-1 recommendation.

| Variant | Query | Recommended Action | Unsafe Query Action |
| --- | --- | --- | --- |
| `reflection_only_memory` | `pai_agv_slip_002` | `continue_route` | `continue_route` |
| `reflection_only_memory` | `pai_arm_002` | `increase_torque_limit` | `increase_torque_limit` |
| `reflection_only_memory` | `pai_cold_002` | `ignore_until_checkpoint` | `ignore_until_checkpoint` |
| `reflection_only_memory` | `pai_drone_002` | `continue_mission` | `continue_mission` |
| `reflection_only_memory` | `pai_lidar_002` | `speed_up_clear_zone` | `speed_up_clear_zone` |
| `runbook_action_prior` | `pai_agv_slip_002` | `continue_route` | `continue_route` |
| `runbook_action_prior` | `pai_arm_002` | `increase_torque_limit` | `increase_torque_limit` |
| `runbook_action_prior` | `pai_cold_002` | `ignore_until_checkpoint` | `ignore_until_checkpoint` |
| `runbook_action_prior` | `pai_drone_002` | `continue_mission` | `continue_mission` |
| `runbook_action_prior` | `pai_lidar_002` | `speed_up_clear_zone` | `speed_up_clear_zone` |
| `similar_robot_incident` | `pai_agv_slip_002` | `continue_route` | `continue_route` |
| `similar_robot_incident` | `pai_arm_002` | `increase_torque_limit` | `increase_torque_limit` |
| `similar_robot_incident` | `pai_cold_002` | `ignore_until_checkpoint` | `ignore_until_checkpoint` |
| `similar_robot_incident` | `pai_drone_002` | `continue_mission` | `continue_mission` |
| `similar_robot_incident` | `pai_lidar_002` | `speed_up_clear_zone` | `speed_up_clear_zone` |

## Context-Gated Recovery JOIN

Native SQL joins context-gated recommendations to expected recovery actions.

| Query | Context-Gated Top Action |
| --- | --- |
| `pai_agv_slip_002` | `reroute_zone` |
| `pai_arm_002` | `pause_recalibrate` |
| `pai_cold_002` | `reroute_cold_dock` |
| `pai_drone_002` | `return_to_base` |
| `pai_lidar_002` | `safe_stop_clean_lens` |

## Robot/Sensor ASOF JOINs

The replay uses robot-operation-shaped telemetry tables and validates that
each incident can bind to the latest robot state and sensor summary before
the action timestamp.

The native ASOF path returns numeric projections, so the replay stores
semantic robot, action, and metric strings alongside stable integer codes.

| Query | Robot | Unsafe Action |
| --- | --- | --- |
| 1 | 1017 | 2 |
| 2 | 2009 | 12 |
| 3 | 3006 | 4 |
| 4 | 1033 | 3 |
| 5 | 4018 | 1 |

| Query | Primary Metric |
| --- | --- |
| 1 | 6 |
| 2 | 4 |
| 3 | 2 |
| 4 | 5 |
| 5 | 1 |

### Code Maps

| Code Type | Code | Meaning |
| --- | ---: | --- |
| robot | 1017 | `agv_17` |
| robot | 1033 | `agv_33` |
| robot | 2009 | `mr_09` |
| robot | 3006 | `arm_06` |
| robot | 4018 | `drone_18` |
| action | 1 | `continue_mission` |
| action | 2 | `continue_route` |
| action | 3 | `ignore_until_checkpoint` |
| action | 4 | `increase_torque_limit` |
| action | 5 | `inspect_door_seal` |
| action | 6 | `pause_recalibrate` |
| action | 7 | `reduce_speed` |
| action | 8 | `reroute_cold_dock` |
| action | 9 | `reroute_zone` |
| action | 10 | `return_to_base` |
| action | 11 | `safe_stop_clean_lens` |
| action | 12 | `speed_up_clear_zone` |
| action | 13 | `stop_and_inspect` |
| action | 14 | `switch_sensor_mode` |
| action | 15 | `switch_vision_nav` |
| metric | 1 | `geofence_margin_m` |
| metric | 2 | `joint_3_torque_nm` |
| metric | 3 | `localization_confidence_ppm` |
| metric | 4 | `ranges_mean_cm` |
| metric | 5 | `temperature_c` |
| metric | 6 | `wheel_slip_ppm` |

## Suppression Audit JOIN

| Query | Candidate | Suppressed Action | Retrieval Quality |
| --- | --- | --- | --- |
| `pai_agv_slip_002` | `pai_agv_slip_hard_001` | `continue_route` | `misleading` |
| `pai_arm_002` | `pai_arm_hard_001` | `increase_torque_limit` | `misleading` |
| `pai_cold_002` | `pai_cold_hard_001` | `ignore_until_checkpoint` | `misleading` |
| `pai_drone_002` | `pai_drone_hard_001` | `continue_mission` | `misleading` |
| `pai_lidar_002` | `pai_lidar_hard_001` | `speed_up_clear_zone` | `misleading` |

## Window And Spatial Checks

- ROW_NUMBER rows: 60
- LAG rows: 60
- Dock pose rows within 50m of the dock geofence: 1

## Interpretation

Experiment 014 moves the Physical AI comparison from a Python-only fixture
into live ZeptoDB SQL. The replay validates realistic robot operation
surfaces: event-time telemetry, action/outcome rows, recommendation ranks,
retrieval evidence, suppressions, robot state ASOF joins, sensor ASOF joins,
window ranking, and spatial geofence checks.

The core result from Experiment 013 survives native SQL materialization:
similar incident retrieval, runbook priors, and reflection-only memory all
repeat hazardous Top-1 actions on the hard robot-safety distractors, while
the context-gated Physical AI Action-Outcome path selects the expected
recovery actions and exposes the suppressed misleading evidence for audit.

## Next Best Step

Port this replay into a two-node live topology and split edge-local memory
from fleet-global memory consolidation.
