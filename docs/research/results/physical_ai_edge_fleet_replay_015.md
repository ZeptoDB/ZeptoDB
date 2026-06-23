# Physical AI Action-Outcome Experiment 015 Edge/Fleet Replay Results

Generated at: 2026-06-23T13:41:38Z
Edge endpoint: `http://127.0.0.1:19441/`
Fleet endpoint: `http://127.0.0.1:19442/`
Fixture: `docs/research/fixtures/physical_ai_action_outcome_episodes.json`
Edge SQL replay file: `docs/research/results/physical_ai_edge_fleet_replay_015_edge.sql`
Fleet SQL replay file: `docs/research/results/physical_ai_edge_fleet_replay_015_fleet.sql`
Classification: Research-only

## Status

- Edge row-count status: pass
- Fleet row-count status: pass
- Edge immediate recovery status: pass
- Edge risky-action suppression status: pass
- Edge robot ASOF status: pass
- Edge sensor ASOF status: pass
- Fleet consolidated recovery status: pass
- Fleet suppression audit JOIN status: pass
- Fleet consolidation-lag status: pass
- Fleet ROW_NUMBER status: pass
- Fleet LAG status: pass
- Overall edge/fleet replay status: pass

## Node Activity

| Node Role | ticks_ingested delta | ticks_stored delta | partitions_created delta | Research rows |
| --- | ---: | ---: | ---: | ---: |
| edge-local | 82 | 82 | 6 | 82 |
| fleet-global | 87 | 87 | 6 | 87 |

## Table Counts

### Edge-Local Tables

| Table | Rows |
| --- | ---: |
| `physical_ai_edge_decisions_015` | 5 |
| `physical_ai_edge_expected_actions_015` | 10 |
| `physical_ai_edge_incidents_015` | 5 |
| `physical_ai_edge_robot_state_015` | 15 |
| `physical_ai_edge_sensor_summary_015` | 15 |
| `physical_ai_edge_suppressions_015` | 32 |

### Fleet-Global Tables

| Table | Rows |
| --- | ---: |
| `physical_ai_fleet_action_outcomes_015` | 25 |
| `physical_ai_fleet_edge_decisions_015` | 5 |
| `physical_ai_fleet_expected_actions_015` | 10 |
| `physical_ai_fleet_retrieval_015` | 15 |
| `physical_ai_fleet_suppressions_015` | 32 |

## Edge-Local Immediate Decision

The edge node validates that the selected action is an expected recovery
action and differs from the unsafe query action before any fleet
consolidation happens.

| Query | Edge Selected Action |
| --- | --- |
| `pai_agv_slip_002` | `reroute_zone` |
| `pai_arm_002` | `pause_recalibrate` |
| `pai_cold_002` | `reroute_cold_dock` |
| `pai_drone_002` | `return_to_base` |
| `pai_lidar_002` | `safe_stop_clean_lens` |

- Risky actions suppressed immediately: 5

## Edge ASOF Telemetry

| Query Seq | Robot Code | Unsafe Action Code |
| --- | --- | --- |
| 1 | 1017 | 2 |
| 2 | 2009 | 12 |
| 3 | 3006 | 4 |
| 4 | 1033 | 3 |
| 5 | 4018 | 1 |

| Query Seq | Primary Metric Code |
| --- | --- |
| 1 | 6 |
| 2 | 4 |
| 3 | 2 |
| 4 | 5 |
| 5 | 1 |

## Fleet-Global Consolidation

The fleet node receives delayed decision rows and validates that recovery
selection and risky-repeat avoidance survive consolidation.

| Query | Fleet Consolidated Action |
| --- | --- |
| `pai_agv_slip_002` | `reroute_zone` |
| `pai_arm_002` | `pause_recalibrate` |
| `pai_cold_002` | `reroute_cold_dock` |
| `pai_drone_002` | `return_to_base` |
| `pai_lidar_002` | `safe_stop_clean_lens` |

- Consolidated rows with lag >= 250 ms: 5

## Fleet Audit JOIN

| Query | Candidate | Suppressed Action | Retrieval Quality |
| --- | --- | --- | --- |
| `pai_agv_slip_002` | `pai_agv_slip_hard_001` | `continue_route` | `misleading` |
| `pai_arm_002` | `pai_arm_hard_001` | `increase_torque_limit` | `misleading` |
| `pai_cold_002` | `pai_cold_hard_001` | `ignore_until_checkpoint` | `misleading` |
| `pai_drone_002` | `pai_drone_hard_001` | `continue_mission` | `misleading` |
| `pai_lidar_002` | `pai_lidar_hard_001` | `speed_up_clear_zone` | `misleading` |

## Fleet Window Checks

- ROW_NUMBER rows: 15
- LAG rows: 5

## Interpretation

Experiment 015 validates the intended split-brain-safe memory shape for
Physical AI: edge-local memory makes the immediate safety decision, while
fleet-global memory receives slower consolidated evidence for audit and
future cross-robot learning.

This is still research-only. The delayed edge-to-fleet transfer is modeled
by the harness writing SQL rows to two live ZeptoDB endpoints; it is not a
new runtime replication or control-plane feature.

## Next Best Step

Replace harness-driven consolidation with a bounded, explicit edge-to-fleet
replication path or feed connector, then test dropped/duplicated
consolidation events and late-arriving fleet audits.
