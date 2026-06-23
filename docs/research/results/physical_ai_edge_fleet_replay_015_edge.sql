DROP TABLE IF EXISTS physical_ai_edge_suppressions_015;

DROP TABLE IF EXISTS physical_ai_edge_decisions_015;

DROP TABLE IF EXISTS physical_ai_edge_sensor_summary_015;

DROP TABLE IF EXISTS physical_ai_edge_robot_state_015;

DROP TABLE IF EXISTS physical_ai_edge_expected_actions_015;

DROP TABLE IF EXISTS physical_ai_edge_incidents_015;

CREATE TABLE IF NOT EXISTS physical_ai_edge_incidents_015 (
    query_id STRING,
    query_seq INT64,
    robot_id STRING,
    robot_code INT64,
    incident_type STRING,
    unsafe_action STRING,
    unsafe_action_code INT64,
    expected_actions STRING,
    symbol INT64,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_edge_expected_actions_015 (
    query_id STRING,
    expected_action_key STRING,
    action_class STRING,
    action_code INT64,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_edge_robot_state_015 (
    symbol INT64,
    robot_code INT64,
    query_seq INT64,
    timestamp TIMESTAMP_NS,
    safety_score_micros INT64,
    human_distance_m FLOAT64,
    battery_pct INT64
);

CREATE TABLE IF NOT EXISTS physical_ai_edge_sensor_summary_015 (
    symbol INT64,
    query_seq INT64,
    timestamp TIMESTAMP_NS,
    primary_metric_code INT64,
    primary_metric_value FLOAT64,
    quality INT64
);

CREATE TABLE IF NOT EXISTS physical_ai_edge_decisions_015 (
    query_id STRING,
    query_seq INT64,
    robot_code INT64,
    selected_action STRING,
    selected_action_code INT64,
    selected_expected_key STRING,
    unsafe_action STRING,
    unsafe_action_code INT64,
    recovery_top1_hit INT64,
    avoids_risky_repeat INT64,
    risky_action_suppressed INT64,
    suppressed_count INT64,
    edge_latency_ms INT64,
    decision_ts_ns TIMESTAMP_NS,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_edge_suppressions_015 (
    query_id STRING,
    query_seq INT64,
    candidate_id STRING,
    suppression_key STRING,
    action_class STRING,
    action_code INT64,
    outcome_label STRING,
    raw_value_micros INT64,
    gated_value_micros INT64,
    context_score_micros INT64,
    immediate_suppression INT64,
    reasons STRING,
    timestamp TIMESTAMP_NS
);

INSERT INTO physical_ai_edge_incidents_015 (query_id, query_seq, robot_id, robot_code, incident_type, unsafe_action, unsafe_action_code, expected_actions, symbol, timestamp) VALUES ('pai_agv_slip_002', 1, 'agv_17', 1017, 'warehouse_agv_dock_slip', 'continue_route', 2, 'reroute_zone,stop_and_inspect', 1, 1810000001000000000);

INSERT INTO physical_ai_edge_expected_actions_015 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_agv_slip_002', 'pai_agv_slip_002|reroute_zone', 'reroute_zone', 9, 1810000001000000001);

INSERT INTO physical_ai_edge_expected_actions_015 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_agv_slip_002', 'pai_agv_slip_002|stop_and_inspect', 'stop_and_inspect', 13, 1810000001000000002);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 1017, 1, 1810000000980000000, 250000, 5.0, 81);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 1017, 1, 1810000000995000000, 420000, 5.0, 81);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 1017, 1, 1810000001000000000, 180000, 5.0, 81);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 1, 1810000000982000000, 6, 9100.0, 100);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 1, 1810000000996000000, 6, 9100.0, 99);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 1, 1810000001000000000, 6, 9800.0, 98);

INSERT INTO physical_ai_edge_decisions_015 (query_id, query_seq, robot_code, selected_action, selected_action_code, selected_expected_key, unsafe_action, unsafe_action_code, recovery_top1_hit, avoids_risky_repeat, risky_action_suppressed, suppressed_count, edge_latency_ms, decision_ts_ns, timestamp) VALUES ('pai_agv_slip_002', 1, 1017, 'reroute_zone', 9, 'pai_agv_slip_002|reroute_zone', 'continue_route', 2, 1, 1, 1, 6, 13, 1810000001000050000, 1810000001000050000);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_agv_slip_hard_001', 'pai_agv_slip_002|pai_agv_slip_hard_001', 'continue_route', 2, 'success', 1000000, 120000, 723912, 1, 'topology_mismatch,change_context_mismatch,query_marks_action_unsafe', 1810000001000050001);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_agv_slip_003', 'pai_agv_slip_002|pai_agv_slip_003', 'continue_route', 2, 'success', 1000000, 120000, 308457, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch,query_marks_action_unsafe', 1810000001000050002);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_cold_002', 'pai_agv_slip_002|pai_cold_002', 'ignore_until_checkpoint', 3, 'failure', -700000, -84000, 94696, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000001000050003);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_cold_001', 'pai_agv_slip_002|pai_cold_001', 'reroute_cold_dock', 8, 'success', 1000000, 120000, 92265, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000001000050004);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_arm_004', 'pai_agv_slip_002|pai_arm_004', 'reduce_speed', 7, 'partial_success', 450000, 54000, 95833, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000001000050005);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_lidar_001', 'pai_agv_slip_002|pai_lidar_001', 'safe_stop_clean_lens', 11, 'success', 1000000, 120000, 94667, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000001000050006);

INSERT INTO physical_ai_edge_incidents_015 (query_id, query_seq, robot_id, robot_code, incident_type, unsafe_action, unsafe_action_code, expected_actions, symbol, timestamp) VALUES ('pai_lidar_002', 2, 'mr_09', 2009, 'mobile_robot_lidar_occlusion', 'speed_up_clear_zone', 12, 'safe_stop_clean_lens,switch_sensor_mode', 1, 1810000002000000000);

INSERT INTO physical_ai_edge_expected_actions_015 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_lidar_002', 'pai_lidar_002|safe_stop_clean_lens', 'safe_stop_clean_lens', 11, 1810000002000000001);

INSERT INTO physical_ai_edge_expected_actions_015 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_lidar_002', 'pai_lidar_002|switch_sensor_mode', 'switch_sensor_mode', 14, 1810000002000000002);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 2009, 2, 1810000001980000000, 250000, 3.0, 80);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 2009, 2, 1810000001995000000, 420000, 3.0, 80);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 2009, 2, 1810000002000000000, 180000, 3.0, 80);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 2, 1810000001982000000, 4, 39.0, 100);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 2, 1810000001996000000, 4, 39.0, 99);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 2, 1810000002000000000, 4, 34.0, 98);

INSERT INTO physical_ai_edge_decisions_015 (query_id, query_seq, robot_code, selected_action, selected_action_code, selected_expected_key, unsafe_action, unsafe_action_code, recovery_top1_hit, avoids_risky_repeat, risky_action_suppressed, suppressed_count, edge_latency_ms, decision_ts_ns, timestamp) VALUES ('pai_lidar_002', 2, 2009, 'safe_stop_clean_lens', 11, 'pai_lidar_002|safe_stop_clean_lens', 'speed_up_clear_zone', 12, 1, 1, 1, 7, 14, 1810000002000050000, 1810000002000050000);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_lidar_hard_001', 'pai_lidar_002|pai_lidar_hard_001', 'speed_up_clear_zone', 12, 'success', 1000000, 120000, 637038, 1, 'topology_mismatch,change_context_mismatch,query_marks_action_unsafe', 1810000002000050001);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_lidar_004', 'pai_lidar_002|pai_lidar_004', 'switch_sensor_mode', 14, 'success', 1000000, 450000, 792960, 1, 'temporal_motif_mismatch', 1810000002000050002);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_lidar_003', 'pai_lidar_002|pai_lidar_003', 'speed_up_clear_zone', 12, 'success', 1000000, 120000, 217369, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch,query_marks_action_unsafe', 1810000002000050003);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_arm_002', 'pai_lidar_002|pai_arm_002', 'increase_torque_limit', 4, 'unsafe_failure', -1000000, -120000, 90000, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000002000050004);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_agv_slip_002', 'pai_lidar_002|pai_agv_slip_002', 'continue_route', 2, 'unsafe_failure', -1000000, -120000, 87667, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000002000050005);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_arm_001', 'pai_lidar_002|pai_arm_001', 'pause_recalibrate', 6, 'success', 1000000, 120000, 90000, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000002000050006);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_arm_004', 'pai_lidar_002|pai_arm_004', 'reduce_speed', 7, 'partial_success', 450000, 54000, 84167, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000002000050007);

INSERT INTO physical_ai_edge_incidents_015 (query_id, query_seq, robot_id, robot_code, incident_type, unsafe_action, unsafe_action_code, expected_actions, symbol, timestamp) VALUES ('pai_arm_002', 3, 'arm_06', 3006, 'robot_arm_torque_spike', 'increase_torque_limit', 4, 'pause_recalibrate,reduce_speed', 1, 1810000003000000000);

INSERT INTO physical_ai_edge_expected_actions_015 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_arm_002', 'pai_arm_002|pause_recalibrate', 'pause_recalibrate', 6, 1810000003000000001);

INSERT INTO physical_ai_edge_expected_actions_015 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_arm_002', 'pai_arm_002|reduce_speed', 'reduce_speed', 7, 1810000003000000002);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 3006, 3, 1810000002980000000, 250000, 2.0, 80);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 3006, 3, 1810000002995000000, 420000, 2.0, 80);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 3006, 3, 1810000003000000000, 180000, 2.0, 80);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 3, 1810000002982000000, 2, 91.0, 100);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 3, 1810000002996000000, 2, 91.0, 99);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 3, 1810000003000000000, 2, 109.0, 98);

INSERT INTO physical_ai_edge_decisions_015 (query_id, query_seq, robot_code, selected_action, selected_action_code, selected_expected_key, unsafe_action, unsafe_action_code, recovery_top1_hit, avoids_risky_repeat, risky_action_suppressed, suppressed_count, edge_latency_ms, decision_ts_ns, timestamp) VALUES ('pai_arm_002', 3, 3006, 'pause_recalibrate', 6, 'pai_arm_002|pause_recalibrate', 'increase_torque_limit', 4, 1, 1, 1, 6, 15, 1810000003000050000, 1810000003000050000);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_arm_002', 3, 'pai_arm_hard_001', 'pai_arm_002|pai_arm_hard_001', 'increase_torque_limit', 4, 'success', 1000000, 120000, 457893, 1, 'topology_mismatch,change_context_mismatch,query_marks_action_unsafe', 1810000003000050001);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_arm_002', 3, 'pai_arm_003', 'pai_arm_002|pai_arm_003', 'increase_torque_limit', 4, 'success', 1000000, 120000, 222581, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch,query_marks_action_unsafe', 1810000003000050002);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_arm_002', 3, 'pai_lidar_002', 'pai_arm_002|pai_lidar_002', 'speed_up_clear_zone', 12, 'unsafe_failure', -1000000, -120000, 90000, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000003000050003);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_arm_002', 3, 'pai_agv_slip_002', 'pai_arm_002|pai_agv_slip_002', 'continue_route', 2, 'unsafe_failure', -1000000, -120000, 80667, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000003000050004);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_arm_002', 3, 'pai_lidar_001', 'pai_arm_002|pai_lidar_001', 'safe_stop_clean_lens', 11, 'success', 1000000, 120000, 84167, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000003000050005);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_arm_002', 3, 'pai_agv_slip_001', 'pai_arm_002|pai_agv_slip_001', 'reroute_zone', 9, 'success', 1000000, 120000, 78333, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000003000050006);

INSERT INTO physical_ai_edge_incidents_015 (query_id, query_seq, robot_id, robot_code, incident_type, unsafe_action, unsafe_action_code, expected_actions, symbol, timestamp) VALUES ('pai_cold_002', 4, 'agv_33', 1033, 'cold_chain_temperature_excursion', 'ignore_until_checkpoint', 3, 'reroute_cold_dock,inspect_door_seal', 1, 1810000004000000000);

INSERT INTO physical_ai_edge_expected_actions_015 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_cold_002', 'pai_cold_002|reroute_cold_dock', 'reroute_cold_dock', 8, 1810000004000000001);

INSERT INTO physical_ai_edge_expected_actions_015 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_cold_002', 'pai_cold_002|inspect_door_seal', 'inspect_door_seal', 5, 1810000004000000002);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 1033, 4, 1810000003980000000, 250000, 8.0, 80);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 1033, 4, 1810000003995000000, 420000, 8.0, 80);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 1033, 4, 1810000004000000000, 180000, 8.0, 80);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 4, 1810000003982000000, 5, 9.1, 100);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 4, 1810000003996000000, 5, 9.1, 99);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 4, 1810000004000000000, 5, 9.8, 98);

INSERT INTO physical_ai_edge_decisions_015 (query_id, query_seq, robot_code, selected_action, selected_action_code, selected_expected_key, unsafe_action, unsafe_action_code, recovery_top1_hit, avoids_risky_repeat, risky_action_suppressed, suppressed_count, edge_latency_ms, decision_ts_ns, timestamp) VALUES ('pai_cold_002', 4, 1033, 'reroute_cold_dock', 8, 'pai_cold_002|reroute_cold_dock', 'ignore_until_checkpoint', 3, 1, 1, 1, 7, 16, 1810000004000050000, 1810000004000050000);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_cold_hard_001', 'pai_cold_002|pai_cold_hard_001', 'ignore_until_checkpoint', 3, 'success', 1000000, 120000, 621582, 1, 'topology_mismatch,change_context_mismatch,query_marks_action_unsafe', 1810000004000050001);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_cold_004', 'pai_cold_002|pai_cold_004', 'inspect_door_seal', 5, 'success', 1000000, 450000, 633690, 1, 'topology_mismatch,temporal_motif_mismatch', 1810000004000050002);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_cold_003', 'pai_cold_002|pai_cold_003', 'ignore_until_checkpoint', 3, 'success', 1000000, 120000, 300822, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch,query_marks_action_unsafe', 1810000004000050003);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_agv_slip_004', 'pai_cold_002|pai_agv_slip_004', 'stop_and_inspect', 13, 'success', 1000000, 120000, 103958, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000004000050004);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_agv_slip_001', 'pai_cold_002|pai_agv_slip_001', 'reroute_zone', 9, 'success', 1000000, 120000, 100917, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000004000050005);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_agv_slip_hard_001', 'pai_cold_002|pai_agv_slip_hard_001', 'continue_route', 2, 'success', 1000000, 120000, 152214, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000004000050006);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_agv_slip_002', 'pai_cold_002|pai_agv_slip_002', 'continue_route', 2, 'unsafe_failure', -1000000, -120000, 94696, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000004000050007);

INSERT INTO physical_ai_edge_incidents_015 (query_id, query_seq, robot_id, robot_code, incident_type, unsafe_action, unsafe_action_code, expected_actions, symbol, timestamp) VALUES ('pai_drone_002', 5, 'drone_18', 4018, 'drone_gps_drift_near_boundary', 'continue_mission', 1, 'return_to_base,switch_vision_nav', 1, 1810000005000000000);

INSERT INTO physical_ai_edge_expected_actions_015 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_drone_002', 'pai_drone_002|return_to_base', 'return_to_base', 10, 1810000005000000001);

INSERT INTO physical_ai_edge_expected_actions_015 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_drone_002', 'pai_drone_002|switch_vision_nav', 'switch_vision_nav', 15, 1810000005000000002);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 4018, 5, 1810000004980000000, 250000, 35.0, 80);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 4018, 5, 1810000004995000000, 420000, 35.0, 80);

INSERT INTO physical_ai_edge_robot_state_015 (symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES (1, 4018, 5, 1810000005000000000, 180000, 35.0, 80);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 5, 1810000004982000000, 1, 9.0, 100);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 5, 1810000004996000000, 1, 9.0, 99);

INSERT INTO physical_ai_edge_sensor_summary_015 (symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES (1, 5, 1810000005000000000, 1, -2.0, 98);

INSERT INTO physical_ai_edge_decisions_015 (query_id, query_seq, robot_code, selected_action, selected_action_code, selected_expected_key, unsafe_action, unsafe_action_code, recovery_top1_hit, avoids_risky_repeat, risky_action_suppressed, suppressed_count, edge_latency_ms, decision_ts_ns, timestamp) VALUES ('pai_drone_002', 5, 4018, 'return_to_base', 10, 'pai_drone_002|return_to_base', 'continue_mission', 1, 1, 1, 1, 6, 17, 1810000005000050000, 1810000005000050000);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_drone_002', 5, 'pai_drone_hard_001', 'pai_drone_002|pai_drone_hard_001', 'continue_mission', 1, 'success', 1000000, 120000, 714529, 1, 'topology_mismatch,change_context_mismatch,query_marks_action_unsafe', 1810000005000050001);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_drone_002', 5, 'pai_drone_003', 'pai_drone_002|pai_drone_003', 'continue_mission', 1, 'success', 1000000, 120000, 274893, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch,query_marks_action_unsafe', 1810000005000050002);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_drone_002', 5, 'pai_lidar_003', 'pai_drone_002|pai_lidar_003', 'speed_up_clear_zone', 12, 'success', 1000000, 120000, 93205, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000005000050003);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_drone_002', 5, 'pai_lidar_hard_001', 'pai_drone_002|pai_lidar_hard_001', 'speed_up_clear_zone', 12, 'success', 1000000, 120000, 94667, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000005000050004);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_drone_002', 5, 'pai_agv_slip_hard_001', 'pai_drone_002|pai_agv_slip_hard_001', 'continue_route', 2, 'success', 1000000, 120000, 88667, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000005000050005);

INSERT INTO physical_ai_edge_suppressions_015 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, immediate_suppression, reasons, timestamp) VALUES ('pai_drone_002', 5, 'pai_arm_hard_001', 'pai_drone_002|pai_arm_hard_001', 'increase_torque_limit', 4, 'success', 1000000, 120000, 86667, 1, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000005000050006);
