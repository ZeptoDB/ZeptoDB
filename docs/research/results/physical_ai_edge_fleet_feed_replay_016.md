# Physical AI Edge/Fleet Feed Replay Experiment 016 Results

Generated: 2026-06-23T13:53:05.599993+00:00

## Configuration

- Edge URL: `http://127.0.0.1:19441/`
- Fleet URL: `http://127.0.0.1:19442/`
- Injected outage URL: `http://127.0.0.1:1/`
- Batch limit: 12
- Max in-flight events: 12
- Classification: Research-only

## Status

- Edge-local count status: pass
- Edge-local immediate recovery status: pass
- Edge-local risky-action suppression status: pass
- Feed acknowledgement convergence status: pass
- Feed event-kind accounting status: pass
- Duplicate delivery handling status: pass
- Late delivery handling status: pass
- Outage retry status: pass
- Restart reload status: pass
- Bounded batch status: pass
- Fleet final count status: pass
- Fleet recovery JOIN status: pass
- Fleet suppression audit JOIN status: pass
- Fleet ACK ROW_NUMBER/LAG status: pass
- Overall bounded feed replay status: pass

## Node Activity

| Node Role | ticks_ingested delta | ticks_stored delta | partitions_created delta | Research rows |
| --- | ---: | ---: | ---: | ---: |
| edge-local | 134 | 134 | 7 | 134 |
| fleet-global | 198 | 198 | 9 | 198 |

## Feed Passes

| Phase | Batch | Attempted | Delivered | Failed | Dropped | Duplicates | Late | Acked Before | Acked After | Restart Reload |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `outage_probe` | 12 | 12 | 0 | 12 | 0 | 0 | 0 | 0 | 0 | 0 |
| `bounded_recovery_with_drop_duplicate` | 12 | 11 | 11 | 0 | 1 | 1 | 0 | 0 | 11 | 0 |
| `restart_retry_late_delivery` | 12 | 12 | 12 | 0 | 0 | 0 | 2 | 11 | 23 | 1 |
| `bounded_final_drain_1` | 12 | 12 | 12 | 0 | 0 | 0 | 0 | 23 | 35 | 0 |
| `bounded_final_drain_2` | 12 | 12 | 12 | 0 | 0 | 0 | 0 | 35 | 47 | 0 |
| `bounded_final_drain_3` | 5 | 5 | 5 | 0 | 0 | 0 | 0 | 47 | 52 | 0 |

## Feed Convergence

- Edge outbox events: 52
- Fleet acknowledged events: 52
- Duplicate inbox attempts: 1
- Late inbox attempts: 2
- Outage telemetry rows: 1
- Restart reload telemetry rows: 1

| Event Kind | ACK Rows |
| --- | ---: |
| `decision` | 5 |
| `retrieval` | 15 |
| `suppression` | 32 |

## Edge Tables

| Table | Rows |
| --- | ---: |
| `physical_ai_edge_decisions_016` | 5 |
| `physical_ai_edge_expected_actions_016` | 10 |
| `physical_ai_edge_feed_outbox_016` | 52 |
| `physical_ai_edge_incidents_016` | 5 |
| `physical_ai_edge_robot_state_016` | 15 |
| `physical_ai_edge_sensor_summary_016` | 15 |
| `physical_ai_edge_suppressions_016` | 32 |

## Fleet Tables

| Table | Rows |
| --- | ---: |
| `physical_ai_fleet_action_outcomes_016` | 25 |
| `physical_ai_fleet_edge_decisions_016` | 5 |
| `physical_ai_fleet_expected_actions_016` | 10 |
| `physical_ai_fleet_feed_ack_016` | 52 |
| `physical_ai_fleet_feed_inbox_016` | 53 |
| `physical_ai_fleet_feed_telemetry_016` | 6 |
| `physical_ai_fleet_retrieval_016` | 15 |
| `physical_ai_fleet_suppressions_016` | 32 |

## Fleet Recovery Decisions

| Query | Fleet Selected Action |
| --- | --- |
| `pai_agv_slip_002` | `reroute_zone` |
| `pai_arm_002` | `pause_recalibrate` |
| `pai_cold_002` | `reroute_cold_dock` |
| `pai_drone_002` | `return_to_base` |
| `pai_lidar_002` | `safe_stop_clean_lens` |

## Fleet Suppression Audit JOIN

| Query | Candidate | Suppressed Action | Quality |
| --- | --- | --- | --- |
| `pai_agv_slip_002` | `pai_agv_slip_hard_001` | `continue_route` | `misleading` |
| `pai_arm_002` | `pai_arm_hard_001` | `increase_torque_limit` | `misleading` |
| `pai_cold_002` | `pai_cold_hard_001` | `ignore_until_checkpoint` | `misleading` |
| `pai_drone_002` | `pai_drone_hard_001` | `continue_mission` | `misleading` |
| `pai_lidar_002` | `pai_lidar_hard_001` | `speed_up_clear_zone` | `misleading` |

## Interpretation

Experiment 016 validates a bounded, explicit edge-to-fleet feed shape. The edge-local node still makes the immediate safety decision and suppresses risky robot actions before fleet consolidation. Fleet-global memory receives edge-generated evidence only through the outbox, bounded feed passes, persistent ACK rows, and retry logic.

This remains research-only. The feed worker is a deterministic research tool, not a ZeptoDB runtime replication service. It proves the semantics needed before product promotion: bounded batches, duplicate tolerance, late arrival tolerance, outage retry, and restart ACK reload.

## Next Best Step

Promote the feed semantics into an experimental runtime connector with operator-visible telemetry, persisted cursor state, and explicit behavior for the non-transactional final-table-plus-ACK boundary.
