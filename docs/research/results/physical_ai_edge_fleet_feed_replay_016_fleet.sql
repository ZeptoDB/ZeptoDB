DROP TABLE IF EXISTS physical_ai_fleet_feed_telemetry_016;

DROP TABLE IF EXISTS physical_ai_fleet_feed_ack_016;

DROP TABLE IF EXISTS physical_ai_fleet_feed_inbox_016;

DROP TABLE IF EXISTS physical_ai_fleet_suppressions_016;

DROP TABLE IF EXISTS physical_ai_fleet_retrieval_016;

DROP TABLE IF EXISTS physical_ai_fleet_edge_decisions_016;

DROP TABLE IF EXISTS physical_ai_fleet_action_outcomes_016;

DROP TABLE IF EXISTS physical_ai_fleet_expected_actions_016;

CREATE TABLE IF NOT EXISTS physical_ai_fleet_expected_actions_016 (
    query_id STRING,
    expected_action_key STRING,
    action_class STRING,
    action_code INT64,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_fleet_action_outcomes_016 (
    episode_id STRING,
    incident_type STRING,
    action_class STRING,
    action_code INT64,
    incident_action_key STRING,
    outcome_label STRING,
    outcome_value_micros INT64,
    safety_restored INT64,
    primary_metric_code INT64,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_fleet_edge_decisions_016 (
    query_id STRING,
    query_seq INT64,
    robot_code INT64,
    selected_action STRING,
    selected_action_code INT64,
    selected_expected_key STRING,
    unsafe_action_code INT64,
    recovery_top1_hit INT64,
    avoids_risky_repeat INT64,
    risky_action_suppressed INT64,
    suppressed_count INT64,
    edge_latency_ms INT64,
    decision_ts_ns TIMESTAMP_NS,
    consolidated_ts_ns TIMESTAMP_NS,
    consolidation_lag_ms INT64,
    source_edge_node_id INT64,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_fleet_retrieval_016 (
    query_id STRING,
    query_seq INT64,
    retrieval_rank INT64,
    candidate_id STRING,
    suppression_key STRING,
    candidate_action STRING,
    quality_label STRING,
    quality_code INT64,
    score_micros INT64,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_fleet_suppressions_016 (
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
    source_edge_node_id INT64,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_fleet_feed_inbox_016 (
    attempt_id STRING,
    feed_event_id STRING,
    stream_seq INT64,
    event_kind STRING,
    query_id STRING,
    delivery_status STRING,
    duplicate_delivery INT64,
    late_delivery INT64,
    attempt_no INT64,
    source_edge_node_id INT64,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_fleet_feed_ack_016 (
    feed_event_id STRING,
    stream_seq INT64,
    event_kind STRING,
    query_id STRING,
    ack_status STRING,
    source_edge_node_id INT64,
    ack_ts_ns TIMESTAMP_NS,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_fleet_feed_telemetry_016 (
    phase STRING,
    pass_index INT64,
    batch_event_count INT64,
    attempted_count INT64,
    delivered_count INT64,
    failed_count INT64,
    dropped_count INT64,
    duplicate_attempt_count INT64,
    late_count INT64,
    acked_before INT64,
    acked_after INT64,
    max_inflight INT64,
    batch_limit INT64,
    restart_reloaded_ack INT64,
    timestamp TIMESTAMP_NS
);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_agv_slip_001', 'warehouse_agv_dock_slip', 'reroute_zone', 9, 'warehouse_agv_dock_slip|reroute_zone', 'success', 1000000, 1, 6, 1710000030000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_agv_slip_002', 'warehouse_agv_dock_slip', 'continue_route', 2, 'warehouse_agv_dock_slip|continue_route', 'unsafe_failure', -1000000, 0, 6, 1710000130000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_agv_slip_003', 'warehouse_agv_dock_slip', 'continue_route', 2, 'warehouse_agv_dock_slip|continue_route', 'success', 1000000, 1, 6, 1710000230000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_agv_slip_004', 'warehouse_agv_dock_slip', 'stop_and_inspect', 13, 'warehouse_agv_dock_slip|stop_and_inspect', 'success', 1000000, 1, 6, 1710000330000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_lidar_001', 'mobile_robot_lidar_occlusion', 'safe_stop_clean_lens', 11, 'mobile_robot_lidar_occlusion|safe_stop_clean_lens', 'success', 1000000, 1, 4, 1710001030000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_lidar_002', 'mobile_robot_lidar_occlusion', 'speed_up_clear_zone', 12, 'mobile_robot_lidar_occlusion|speed_up_clear_zone', 'unsafe_failure', -1000000, 0, 4, 1710001130000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_lidar_003', 'mobile_robot_lidar_occlusion', 'speed_up_clear_zone', 12, 'mobile_robot_lidar_occlusion|speed_up_clear_zone', 'success', 1000000, 1, 4, 1710001230000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_lidar_004', 'mobile_robot_lidar_occlusion', 'switch_sensor_mode', 14, 'mobile_robot_lidar_occlusion|switch_sensor_mode', 'success', 1000000, 1, 3, 1710001330000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_arm_001', 'robot_arm_torque_spike', 'pause_recalibrate', 6, 'robot_arm_torque_spike|pause_recalibrate', 'success', 1000000, 1, 2, 1710002030000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_arm_002', 'robot_arm_torque_spike', 'increase_torque_limit', 4, 'robot_arm_torque_spike|increase_torque_limit', 'unsafe_failure', -1000000, 0, 2, 1710002130000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_arm_003', 'robot_arm_torque_spike', 'increase_torque_limit', 4, 'robot_arm_torque_spike|increase_torque_limit', 'success', 1000000, 1, 2, 1710002230000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_arm_004', 'robot_arm_torque_spike', 'reduce_speed', 7, 'robot_arm_torque_spike|reduce_speed', 'partial_success', 450000, 1, 2, 1710002330000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_cold_001', 'cold_chain_temperature_excursion', 'reroute_cold_dock', 8, 'cold_chain_temperature_excursion|reroute_cold_dock', 'success', 1000000, 1, 5, 1710003030000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_cold_002', 'cold_chain_temperature_excursion', 'ignore_until_checkpoint', 3, 'cold_chain_temperature_excursion|ignore_until_checkpoint', 'failure', -700000, 0, 5, 1710003130000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_cold_003', 'cold_chain_temperature_excursion', 'ignore_until_checkpoint', 3, 'cold_chain_temperature_excursion|ignore_until_checkpoint', 'success', 1000000, 1, 5, 1710003230000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_cold_004', 'cold_chain_temperature_excursion', 'inspect_door_seal', 5, 'cold_chain_temperature_excursion|inspect_door_seal', 'success', 1000000, 1, 5, 1710003330000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_drone_001', 'drone_gps_drift_near_boundary', 'return_to_base', 10, 'drone_gps_drift_near_boundary|return_to_base', 'success', 1000000, 1, 1, 1710004030000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_drone_002', 'drone_gps_drift_near_boundary', 'continue_mission', 1, 'drone_gps_drift_near_boundary|continue_mission', 'unsafe_failure', -1000000, 0, 1, 1710004130000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_drone_003', 'drone_gps_drift_near_boundary', 'continue_mission', 1, 'drone_gps_drift_near_boundary|continue_mission', 'success', 1000000, 1, 1, 1710004230000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_drone_004', 'drone_gps_drift_near_boundary', 'switch_vision_nav', 15, 'drone_gps_drift_near_boundary|switch_vision_nav', 'success', 1000000, 1, 1, 1710004330000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_agv_slip_hard_001', 'warehouse_agv_dock_slip', 'continue_route', 2, 'warehouse_agv_dock_slip|continue_route', 'success', 1000000, 1, 6, 1710000430000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_lidar_hard_001', 'mobile_robot_lidar_occlusion', 'speed_up_clear_zone', 12, 'mobile_robot_lidar_occlusion|speed_up_clear_zone', 'success', 1000000, 1, 4, 1710001430000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_arm_hard_001', 'robot_arm_torque_spike', 'increase_torque_limit', 4, 'robot_arm_torque_spike|increase_torque_limit', 'success', 1000000, 1, 2, 1710002430000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_cold_hard_001', 'cold_chain_temperature_excursion', 'ignore_until_checkpoint', 3, 'cold_chain_temperature_excursion|ignore_until_checkpoint', 'success', 1000000, 1, 5, 1710003430000000000);

INSERT INTO physical_ai_fleet_action_outcomes_016 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES ('pai_drone_hard_001', 'drone_gps_drift_near_boundary', 'continue_mission', 1, 'drone_gps_drift_near_boundary|continue_mission', 'success', 1000000, 1, 1, 1710004430000000000);

INSERT INTO physical_ai_fleet_expected_actions_016 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_agv_slip_002', 'pai_agv_slip_002|reroute_zone', 'reroute_zone', 9, 1810000001000000001);

INSERT INTO physical_ai_fleet_expected_actions_016 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_agv_slip_002', 'pai_agv_slip_002|stop_and_inspect', 'stop_and_inspect', 13, 1810000001000000002);

INSERT INTO physical_ai_fleet_expected_actions_016 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_lidar_002', 'pai_lidar_002|safe_stop_clean_lens', 'safe_stop_clean_lens', 11, 1810000002000000001);

INSERT INTO physical_ai_fleet_expected_actions_016 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_lidar_002', 'pai_lidar_002|switch_sensor_mode', 'switch_sensor_mode', 14, 1810000002000000002);

INSERT INTO physical_ai_fleet_expected_actions_016 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_arm_002', 'pai_arm_002|pause_recalibrate', 'pause_recalibrate', 6, 1810000003000000001);

INSERT INTO physical_ai_fleet_expected_actions_016 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_arm_002', 'pai_arm_002|reduce_speed', 'reduce_speed', 7, 1810000003000000002);

INSERT INTO physical_ai_fleet_expected_actions_016 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_cold_002', 'pai_cold_002|reroute_cold_dock', 'reroute_cold_dock', 8, 1810000004000000001);

INSERT INTO physical_ai_fleet_expected_actions_016 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_cold_002', 'pai_cold_002|inspect_door_seal', 'inspect_door_seal', 5, 1810000004000000002);

INSERT INTO physical_ai_fleet_expected_actions_016 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_drone_002', 'pai_drone_002|return_to_base', 'return_to_base', 10, 1810000005000000001);

INSERT INTO physical_ai_fleet_expected_actions_016 (query_id, expected_action_key, action_class, action_code, timestamp) VALUES ('pai_drone_002', 'pai_drone_002|switch_vision_nav', 'switch_vision_nav', 15, 1810000005000000002);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|decision|attempt|1|1810000001260050001', 'pai_agv_slip_002|decision', 1, 'decision', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001260050001);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|retrieval|1|attempt|1|1810000001310050003', 'pai_agv_slip_002|retrieval|1', 2, 'retrieval', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001310050003);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|retrieval|2|attempt|1|1810000001310050005', 'pai_agv_slip_002|retrieval|2', 3, 'retrieval', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001310050005);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|retrieval|3|attempt|1|1810000001310050007', 'pai_agv_slip_002|retrieval|3', 4, 'retrieval', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001310050007);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|suppression|1|attempt|1|1810000001360050006', 'pai_agv_slip_002|suppression|1', 5, 'suppression', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001360050006);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|suppression|2|attempt|1|1810000001360050008', 'pai_agv_slip_002|suppression|2', 6, 'suppression', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001360050008);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|suppression|3|attempt|1|1810000001360050010', 'pai_agv_slip_002|suppression|3', 7, 'suppression', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001360050010);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|suppression|4|attempt|1|1810000001360050012', 'pai_agv_slip_002|suppression|4', 8, 'suppression', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001360050012);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|suppression|5|attempt|1|1810000001360050014', 'pai_agv_slip_002|suppression|5', 9, 'suppression', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001360050014);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|suppression|6|attempt|1|1810000001360050016', 'pai_agv_slip_002|suppression|6', 10, 'suppression', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001360050016);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_lidar_002|decision|attempt|1|1810000002260050011', 'pai_lidar_002|decision', 11, 'decision', 'pai_lidar_002', 'delivered', 0, 0, 1, 1, 1810000002260050011);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_lidar_002|retrieval|1|attempt|1|1810000002310050013', 'pai_lidar_002|retrieval|1', 12, 'retrieval', 'pai_lidar_002', 'delivered', 0, 0, 1, 1, 1810000002310050013);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|decision|attempt|1|1810000001270050001', 'pai_agv_slip_002|decision', 1, 'decision', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001270050001);

INSERT INTO physical_ai_fleet_edge_decisions_016 (query_id, query_seq, robot_code, selected_action, selected_action_code, selected_expected_key, unsafe_action_code, recovery_top1_hit, avoids_risky_repeat, risky_action_suppressed, suppressed_count, edge_latency_ms, decision_ts_ns, consolidated_ts_ns, consolidation_lag_ms, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002', 1, 0, 'reroute_zone', 9, 'pai_agv_slip_002|reroute_zone', 2, 1, 1, 1, 6, 13, 1810000001000050000, 1810000001270050001, 270, 1, 1810000001270050001);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_agv_slip_002|decision', 1, 'decision', 'pai_agv_slip_002', 'acked', 1, 1810000001270050001, 1810000001270050001);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|retrieval|2|attempt|1|1810000001320050005', 'pai_agv_slip_002|retrieval|2', 3, 'retrieval', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001320050005);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_agv_slip_002', 1, 2, 'pai_agv_slip_hard_001', 'pai_agv_slip_002|pai_agv_slip_hard_001', 'continue_route', 'misleading', 3, 773229, 1810000001320050005);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_agv_slip_002|retrieval|2', 3, 'retrieval', 'pai_agv_slip_002', 'acked', 1, 1810000001320050005, 1810000001320050005);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|retrieval|2|attempt|2|1810000001320050006', 'pai_agv_slip_002|retrieval|2', 3, 'retrieval', 'pai_agv_slip_002', 'duplicate', 1, 0, 2, 1, 1810000001320050006);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|retrieval|3|attempt|1|1810000001320050007', 'pai_agv_slip_002|retrieval|3', 4, 'retrieval', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001320050007);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_agv_slip_002', 1, 3, 'pai_agv_slip_004', 'pai_agv_slip_002|pai_agv_slip_004', 'stop_and_inspect', 'useful', 1, 664990, 1810000001320050007);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_agv_slip_002|retrieval|3', 4, 'retrieval', 'pai_agv_slip_002', 'acked', 1, 1810000001320050007, 1810000001320050007);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|suppression|1|attempt|1|1810000001370050006', 'pai_agv_slip_002|suppression|1', 5, 'suppression', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001370050006);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_agv_slip_hard_001', 'pai_agv_slip_002|pai_agv_slip_hard_001', 'continue_route', 2, 'success', 1000000, 120000, 723912, 1, 1810000001370050006);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_agv_slip_002|suppression|1', 5, 'suppression', 'pai_agv_slip_002', 'acked', 1, 1810000001370050006, 1810000001370050006);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|suppression|2|attempt|1|1810000001370050008', 'pai_agv_slip_002|suppression|2', 6, 'suppression', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001370050008);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_agv_slip_003', 'pai_agv_slip_002|pai_agv_slip_003', 'continue_route', 2, 'success', 1000000, 120000, 308457, 1, 1810000001370050008);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_agv_slip_002|suppression|2', 6, 'suppression', 'pai_agv_slip_002', 'acked', 1, 1810000001370050008, 1810000001370050008);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|suppression|3|attempt|1|1810000001370050010', 'pai_agv_slip_002|suppression|3', 7, 'suppression', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001370050010);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_cold_002', 'pai_agv_slip_002|pai_cold_002', 'ignore_until_checkpoint', 3, 'failure', -700000, -84000, 94696, 1, 1810000001370050010);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_agv_slip_002|suppression|3', 7, 'suppression', 'pai_agv_slip_002', 'acked', 1, 1810000001370050010, 1810000001370050010);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|suppression|4|attempt|1|1810000001370050012', 'pai_agv_slip_002|suppression|4', 8, 'suppression', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001370050012);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_cold_001', 'pai_agv_slip_002|pai_cold_001', 'reroute_cold_dock', 8, 'success', 1000000, 120000, 92265, 1, 1810000001370050012);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_agv_slip_002|suppression|4', 8, 'suppression', 'pai_agv_slip_002', 'acked', 1, 1810000001370050012, 1810000001370050012);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|suppression|5|attempt|1|1810000001370050014', 'pai_agv_slip_002|suppression|5', 9, 'suppression', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001370050014);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_arm_004', 'pai_agv_slip_002|pai_arm_004', 'reduce_speed', 7, 'partial_success', 450000, 54000, 95833, 1, 1810000001370050014);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_agv_slip_002|suppression|5', 9, 'suppression', 'pai_agv_slip_002', 'acked', 1, 1810000001370050014, 1810000001370050014);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|suppression|6|attempt|1|1810000001370050016', 'pai_agv_slip_002|suppression|6', 10, 'suppression', 'pai_agv_slip_002', 'delivered', 0, 0, 1, 1, 1810000001370050016);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_lidar_001', 'pai_agv_slip_002|pai_lidar_001', 'safe_stop_clean_lens', 11, 'success', 1000000, 120000, 94667, 1, 1810000001370050016);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_agv_slip_002|suppression|6', 10, 'suppression', 'pai_agv_slip_002', 'acked', 1, 1810000001370050016, 1810000001370050016);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_lidar_002|decision|attempt|1|1810000002270050011', 'pai_lidar_002|decision', 11, 'decision', 'pai_lidar_002', 'delivered', 0, 0, 1, 1, 1810000002270050011);

INSERT INTO physical_ai_fleet_edge_decisions_016 (query_id, query_seq, robot_code, selected_action, selected_action_code, selected_expected_key, unsafe_action_code, recovery_top1_hit, avoids_risky_repeat, risky_action_suppressed, suppressed_count, edge_latency_ms, decision_ts_ns, consolidated_ts_ns, consolidation_lag_ms, source_edge_node_id, timestamp) VALUES ('pai_lidar_002', 2, 0, 'safe_stop_clean_lens', 11, 'pai_lidar_002|safe_stop_clean_lens', 12, 1, 1, 1, 7, 14, 1810000002000050000, 1810000002270050011, 270, 1, 1810000002270050011);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_lidar_002|decision', 11, 'decision', 'pai_lidar_002', 'acked', 1, 1810000002270050011, 1810000002270050011);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_lidar_002|retrieval|1|attempt|1|1810000002320050013', 'pai_lidar_002|retrieval|1', 12, 'retrieval', 'pai_lidar_002', 'delivered', 0, 0, 1, 1, 1810000002320050013);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_lidar_002', 2, 1, 'pai_lidar_001', 'pai_lidar_002|pai_lidar_001', 'safe_stop_clean_lens', 'useful', 1, 864195, 1810000002320050013);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_lidar_002|retrieval|1', 12, 'retrieval', 'pai_lidar_002', 'acked', 1, 1810000002320050013, 1810000002320050013);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_agv_slip_002|retrieval|1|attempt|1|1810000002230050002', 'pai_agv_slip_002|retrieval|1', 2, 'retrieval', 'pai_agv_slip_002', 'delivered', 0, 1, 1, 1, 1810000002230050002);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_agv_slip_002', 1, 1, 'pai_agv_slip_001', 'pai_agv_slip_002|pai_agv_slip_001', 'reroute_zone', 'useful', 1, 914131, 1810000002230050002);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_agv_slip_002|retrieval|1', 2, 'retrieval', 'pai_agv_slip_002', 'acked', 1, 1810000002230050002, 1810000002230050002);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_lidar_002|retrieval|2|attempt|1|1810000002330050004', 'pai_lidar_002|retrieval|2', 13, 'retrieval', 'pai_lidar_002', 'delivered', 0, 0, 1, 1, 1810000002330050004);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_lidar_002', 2, 2, 'pai_lidar_hard_001', 'pai_lidar_002|pai_lidar_hard_001', 'speed_up_clear_zone', 'misleading', 3, 701931, 1810000002330050004);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_lidar_002|retrieval|2', 13, 'retrieval', 'pai_lidar_002', 'acked', 1, 1810000002330050004, 1810000002330050004);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_lidar_002|retrieval|3|attempt|1|1810000002330050006', 'pai_lidar_002|retrieval|3', 14, 'retrieval', 'pai_lidar_002', 'delivered', 0, 0, 1, 1, 1810000002330050006);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_lidar_002', 2, 3, 'pai_lidar_004', 'pai_lidar_002|pai_lidar_004', 'switch_sensor_mode', 'useful', 1, 552693, 1810000002330050006);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_lidar_002|retrieval|3', 14, 'retrieval', 'pai_lidar_002', 'acked', 1, 1810000002330050006, 1810000002330050006);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_lidar_002|suppression|1|attempt|1|1810000003280050005', 'pai_lidar_002|suppression|1', 15, 'suppression', 'pai_lidar_002', 'delivered', 0, 1, 1, 1, 1810000003280050005);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_lidar_002', 2, 'pai_lidar_hard_001', 'pai_lidar_002|pai_lidar_hard_001', 'speed_up_clear_zone', 12, 'success', 1000000, 120000, 637038, 1, 1810000003280050005);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_lidar_002|suppression|1', 15, 'suppression', 'pai_lidar_002', 'acked', 1, 1810000003280050005, 1810000003280050005);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_lidar_002|suppression|2|attempt|1|1810000002380050007', 'pai_lidar_002|suppression|2', 16, 'suppression', 'pai_lidar_002', 'delivered', 0, 0, 1, 1, 1810000002380050007);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_lidar_002', 2, 'pai_lidar_004', 'pai_lidar_002|pai_lidar_004', 'switch_sensor_mode', 14, 'success', 1000000, 450000, 792960, 1, 1810000002380050007);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_lidar_002|suppression|2', 16, 'suppression', 'pai_lidar_002', 'acked', 1, 1810000002380050007, 1810000002380050007);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_lidar_002|suppression|3|attempt|1|1810000002380050009', 'pai_lidar_002|suppression|3', 17, 'suppression', 'pai_lidar_002', 'delivered', 0, 0, 1, 1, 1810000002380050009);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_lidar_002', 2, 'pai_lidar_003', 'pai_lidar_002|pai_lidar_003', 'speed_up_clear_zone', 12, 'success', 1000000, 120000, 217369, 1, 1810000002380050009);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_lidar_002|suppression|3', 17, 'suppression', 'pai_lidar_002', 'acked', 1, 1810000002380050009, 1810000002380050009);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_lidar_002|suppression|4|attempt|1|1810000002380050011', 'pai_lidar_002|suppression|4', 18, 'suppression', 'pai_lidar_002', 'delivered', 0, 0, 1, 1, 1810000002380050011);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_lidar_002', 2, 'pai_arm_002', 'pai_lidar_002|pai_arm_002', 'increase_torque_limit', 4, 'unsafe_failure', -1000000, -120000, 90000, 1, 1810000002380050011);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_lidar_002|suppression|4', 18, 'suppression', 'pai_lidar_002', 'acked', 1, 1810000002380050011, 1810000002380050011);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_lidar_002|suppression|5|attempt|1|1810000002380050013', 'pai_lidar_002|suppression|5', 19, 'suppression', 'pai_lidar_002', 'delivered', 0, 0, 1, 1, 1810000002380050013);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_lidar_002', 2, 'pai_agv_slip_002', 'pai_lidar_002|pai_agv_slip_002', 'continue_route', 2, 'unsafe_failure', -1000000, -120000, 87667, 1, 1810000002380050013);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_lidar_002|suppression|5', 19, 'suppression', 'pai_lidar_002', 'acked', 1, 1810000002380050013, 1810000002380050013);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_lidar_002|suppression|6|attempt|1|1810000002380050015', 'pai_lidar_002|suppression|6', 20, 'suppression', 'pai_lidar_002', 'delivered', 0, 0, 1, 1, 1810000002380050015);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_lidar_002', 2, 'pai_arm_001', 'pai_lidar_002|pai_arm_001', 'pause_recalibrate', 6, 'success', 1000000, 120000, 90000, 1, 1810000002380050015);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_lidar_002|suppression|6', 20, 'suppression', 'pai_lidar_002', 'acked', 1, 1810000002380050015, 1810000002380050015);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_lidar_002|suppression|7|attempt|1|1810000002380050017', 'pai_lidar_002|suppression|7', 21, 'suppression', 'pai_lidar_002', 'delivered', 0, 0, 1, 1, 1810000002380050017);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_lidar_002', 2, 'pai_arm_004', 'pai_lidar_002|pai_arm_004', 'reduce_speed', 7, 'partial_success', 450000, 54000, 84167, 1, 1810000002380050017);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_lidar_002|suppression|7', 21, 'suppression', 'pai_lidar_002', 'acked', 1, 1810000002380050017, 1810000002380050017);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_arm_002|decision|attempt|1|1810000003280050011', 'pai_arm_002|decision', 22, 'decision', 'pai_arm_002', 'delivered', 0, 0, 1, 1, 1810000003280050011);

INSERT INTO physical_ai_fleet_edge_decisions_016 (query_id, query_seq, robot_code, selected_action, selected_action_code, selected_expected_key, unsafe_action_code, recovery_top1_hit, avoids_risky_repeat, risky_action_suppressed, suppressed_count, edge_latency_ms, decision_ts_ns, consolidated_ts_ns, consolidation_lag_ms, source_edge_node_id, timestamp) VALUES ('pai_arm_002', 3, 0, 'pause_recalibrate', 6, 'pai_arm_002|pause_recalibrate', 4, 1, 1, 1, 6, 15, 1810000003000050000, 1810000003280050011, 280, 1, 1810000003280050011);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_arm_002|decision', 22, 'decision', 'pai_arm_002', 'acked', 1, 1810000003280050011, 1810000003280050011);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_arm_002|retrieval|1|attempt|1|1810000003330050013', 'pai_arm_002|retrieval|1', 23, 'retrieval', 'pai_arm_002', 'delivered', 0, 0, 1, 1, 1810000003330050013);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_arm_002', 3, 1, 'pai_arm_001', 'pai_arm_002|pai_arm_001', 'pause_recalibrate', 'useful', 1, 916855, 1810000003330050013);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_arm_002|retrieval|1', 23, 'retrieval', 'pai_arm_002', 'acked', 1, 1810000003330050013, 1810000003330050013);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_arm_002|retrieval|2|attempt|1|1810000003340050003', 'pai_arm_002|retrieval|2', 24, 'retrieval', 'pai_arm_002', 'delivered', 0, 0, 1, 1, 1810000003340050003);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_arm_002', 3, 2, 'pai_arm_004', 'pai_arm_002|pai_arm_004', 'reduce_speed', 'useful', 1, 750738, 1810000003340050003);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_arm_002|retrieval|2', 24, 'retrieval', 'pai_arm_002', 'acked', 1, 1810000003340050003, 1810000003340050003);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_arm_002|retrieval|3|attempt|1|1810000003340050005', 'pai_arm_002|retrieval|3', 25, 'retrieval', 'pai_arm_002', 'delivered', 0, 0, 1, 1, 1810000003340050005);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_arm_002', 3, 3, 'pai_arm_hard_001', 'pai_arm_002|pai_arm_hard_001', 'increase_torque_limit', 'misleading', 3, 641124, 1810000003340050005);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_arm_002|retrieval|3', 25, 'retrieval', 'pai_arm_002', 'acked', 1, 1810000003340050005, 1810000003340050005);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_arm_002|suppression|1|attempt|1|1810000003390050004', 'pai_arm_002|suppression|1', 26, 'suppression', 'pai_arm_002', 'delivered', 0, 0, 1, 1, 1810000003390050004);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_arm_002', 3, 'pai_arm_hard_001', 'pai_arm_002|pai_arm_hard_001', 'increase_torque_limit', 4, 'success', 1000000, 120000, 457893, 1, 1810000003390050004);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_arm_002|suppression|1', 26, 'suppression', 'pai_arm_002', 'acked', 1, 1810000003390050004, 1810000003390050004);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_arm_002|suppression|2|attempt|1|1810000003390050006', 'pai_arm_002|suppression|2', 27, 'suppression', 'pai_arm_002', 'delivered', 0, 0, 1, 1, 1810000003390050006);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_arm_002', 3, 'pai_arm_003', 'pai_arm_002|pai_arm_003', 'increase_torque_limit', 4, 'success', 1000000, 120000, 222581, 1, 1810000003390050006);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_arm_002|suppression|2', 27, 'suppression', 'pai_arm_002', 'acked', 1, 1810000003390050006, 1810000003390050006);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_arm_002|suppression|3|attempt|1|1810000003390050008', 'pai_arm_002|suppression|3', 28, 'suppression', 'pai_arm_002', 'delivered', 0, 0, 1, 1, 1810000003390050008);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_arm_002', 3, 'pai_lidar_002', 'pai_arm_002|pai_lidar_002', 'speed_up_clear_zone', 12, 'unsafe_failure', -1000000, -120000, 90000, 1, 1810000003390050008);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_arm_002|suppression|3', 28, 'suppression', 'pai_arm_002', 'acked', 1, 1810000003390050008, 1810000003390050008);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_arm_002|suppression|4|attempt|1|1810000003390050010', 'pai_arm_002|suppression|4', 29, 'suppression', 'pai_arm_002', 'delivered', 0, 0, 1, 1, 1810000003390050010);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_arm_002', 3, 'pai_agv_slip_002', 'pai_arm_002|pai_agv_slip_002', 'continue_route', 2, 'unsafe_failure', -1000000, -120000, 80667, 1, 1810000003390050010);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_arm_002|suppression|4', 29, 'suppression', 'pai_arm_002', 'acked', 1, 1810000003390050010, 1810000003390050010);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_arm_002|suppression|5|attempt|1|1810000003390050012', 'pai_arm_002|suppression|5', 30, 'suppression', 'pai_arm_002', 'delivered', 0, 0, 1, 1, 1810000003390050012);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_arm_002', 3, 'pai_lidar_001', 'pai_arm_002|pai_lidar_001', 'safe_stop_clean_lens', 11, 'success', 1000000, 120000, 84167, 1, 1810000003390050012);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_arm_002|suppression|5', 30, 'suppression', 'pai_arm_002', 'acked', 1, 1810000003390050012, 1810000003390050012);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_arm_002|suppression|6|attempt|1|1810000003390050014', 'pai_arm_002|suppression|6', 31, 'suppression', 'pai_arm_002', 'delivered', 0, 0, 1, 1, 1810000003390050014);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_arm_002', 3, 'pai_agv_slip_001', 'pai_arm_002|pai_agv_slip_001', 'reroute_zone', 9, 'success', 1000000, 120000, 78333, 1, 1810000003390050014);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_arm_002|suppression|6', 31, 'suppression', 'pai_arm_002', 'acked', 1, 1810000003390050014, 1810000003390050014);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_cold_002|decision|attempt|1|1810000004290050009', 'pai_cold_002|decision', 32, 'decision', 'pai_cold_002', 'delivered', 0, 0, 1, 1, 1810000004290050009);

INSERT INTO physical_ai_fleet_edge_decisions_016 (query_id, query_seq, robot_code, selected_action, selected_action_code, selected_expected_key, unsafe_action_code, recovery_top1_hit, avoids_risky_repeat, risky_action_suppressed, suppressed_count, edge_latency_ms, decision_ts_ns, consolidated_ts_ns, consolidation_lag_ms, source_edge_node_id, timestamp) VALUES ('pai_cold_002', 4, 0, 'reroute_cold_dock', 8, 'pai_cold_002|reroute_cold_dock', 3, 1, 1, 1, 7, 16, 1810000004000050000, 1810000004290050009, 290, 1, 1810000004290050009);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_cold_002|decision', 32, 'decision', 'pai_cold_002', 'acked', 1, 1810000004290050009, 1810000004290050009);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_cold_002|retrieval|1|attempt|1|1810000004340050011', 'pai_cold_002|retrieval|1', 33, 'retrieval', 'pai_cold_002', 'delivered', 0, 0, 1, 1, 1810000004340050011);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_cold_002', 4, 1, 'pai_cold_001', 'pai_cold_002|pai_cold_001', 'reroute_cold_dock', 'useful', 1, 935729, 1810000004340050011);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_cold_002|retrieval|1', 33, 'retrieval', 'pai_cold_002', 'acked', 1, 1810000004340050011, 1810000004340050011);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_cold_002|retrieval|2|attempt|1|1810000004340050013', 'pai_cold_002|retrieval|2', 34, 'retrieval', 'pai_cold_002', 'delivered', 0, 0, 1, 1, 1810000004340050013);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_cold_002', 4, 2, 'pai_cold_hard_001', 'pai_cold_002|pai_cold_hard_001', 'ignore_until_checkpoint', 'misleading', 3, 728209, 1810000004340050013);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_cold_002|retrieval|2', 34, 'retrieval', 'pai_cold_002', 'acked', 1, 1810000004340050013, 1810000004340050013);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_cold_002|retrieval|3|attempt|1|1810000004340050015', 'pai_cold_002|retrieval|3', 35, 'retrieval', 'pai_cold_002', 'delivered', 0, 0, 1, 1, 1810000004340050015);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_cold_002', 4, 3, 'pai_cold_004', 'pai_cold_002|pai_cold_004', 'inspect_door_seal', 'useful', 1, 485328, 1810000004340050015);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_cold_002|retrieval|3', 35, 'retrieval', 'pai_cold_002', 'acked', 1, 1810000004340050015, 1810000004340050015);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_cold_002|suppression|1|attempt|1|1810000004400050002', 'pai_cold_002|suppression|1', 36, 'suppression', 'pai_cold_002', 'delivered', 0, 0, 1, 1, 1810000004400050002);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_cold_002', 4, 'pai_cold_hard_001', 'pai_cold_002|pai_cold_hard_001', 'ignore_until_checkpoint', 3, 'success', 1000000, 120000, 621582, 1, 1810000004400050002);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_cold_002|suppression|1', 36, 'suppression', 'pai_cold_002', 'acked', 1, 1810000004400050002, 1810000004400050002);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_cold_002|suppression|2|attempt|1|1810000004400050004', 'pai_cold_002|suppression|2', 37, 'suppression', 'pai_cold_002', 'delivered', 0, 0, 1, 1, 1810000004400050004);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_cold_002', 4, 'pai_cold_004', 'pai_cold_002|pai_cold_004', 'inspect_door_seal', 5, 'success', 1000000, 450000, 633690, 1, 1810000004400050004);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_cold_002|suppression|2', 37, 'suppression', 'pai_cold_002', 'acked', 1, 1810000004400050004, 1810000004400050004);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_cold_002|suppression|3|attempt|1|1810000004400050006', 'pai_cold_002|suppression|3', 38, 'suppression', 'pai_cold_002', 'delivered', 0, 0, 1, 1, 1810000004400050006);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_cold_002', 4, 'pai_cold_003', 'pai_cold_002|pai_cold_003', 'ignore_until_checkpoint', 3, 'success', 1000000, 120000, 300822, 1, 1810000004400050006);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_cold_002|suppression|3', 38, 'suppression', 'pai_cold_002', 'acked', 1, 1810000004400050006, 1810000004400050006);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_cold_002|suppression|4|attempt|1|1810000004400050008', 'pai_cold_002|suppression|4', 39, 'suppression', 'pai_cold_002', 'delivered', 0, 0, 1, 1, 1810000004400050008);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_cold_002', 4, 'pai_agv_slip_004', 'pai_cold_002|pai_agv_slip_004', 'stop_and_inspect', 13, 'success', 1000000, 120000, 103958, 1, 1810000004400050008);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_cold_002|suppression|4', 39, 'suppression', 'pai_cold_002', 'acked', 1, 1810000004400050008, 1810000004400050008);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_cold_002|suppression|5|attempt|1|1810000004400050010', 'pai_cold_002|suppression|5', 40, 'suppression', 'pai_cold_002', 'delivered', 0, 0, 1, 1, 1810000004400050010);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_cold_002', 4, 'pai_agv_slip_001', 'pai_cold_002|pai_agv_slip_001', 'reroute_zone', 9, 'success', 1000000, 120000, 100917, 1, 1810000004400050010);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_cold_002|suppression|5', 40, 'suppression', 'pai_cold_002', 'acked', 1, 1810000004400050010, 1810000004400050010);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_cold_002|suppression|6|attempt|1|1810000004400050012', 'pai_cold_002|suppression|6', 41, 'suppression', 'pai_cold_002', 'delivered', 0, 0, 1, 1, 1810000004400050012);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_cold_002', 4, 'pai_agv_slip_hard_001', 'pai_cold_002|pai_agv_slip_hard_001', 'continue_route', 2, 'success', 1000000, 120000, 152214, 1, 1810000004400050012);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_cold_002|suppression|6', 41, 'suppression', 'pai_cold_002', 'acked', 1, 1810000004400050012, 1810000004400050012);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_cold_002|suppression|7|attempt|1|1810000004400050014', 'pai_cold_002|suppression|7', 42, 'suppression', 'pai_cold_002', 'delivered', 0, 0, 1, 1, 1810000004400050014);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_cold_002', 4, 'pai_agv_slip_002', 'pai_cold_002|pai_agv_slip_002', 'continue_route', 2, 'unsafe_failure', -1000000, -120000, 94696, 1, 1810000004400050014);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_cold_002|suppression|7', 42, 'suppression', 'pai_cold_002', 'acked', 1, 1810000004400050014, 1810000004400050014);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_drone_002|decision|attempt|1|1810000005300050008', 'pai_drone_002|decision', 43, 'decision', 'pai_drone_002', 'delivered', 0, 0, 1, 1, 1810000005300050008);

INSERT INTO physical_ai_fleet_edge_decisions_016 (query_id, query_seq, robot_code, selected_action, selected_action_code, selected_expected_key, unsafe_action_code, recovery_top1_hit, avoids_risky_repeat, risky_action_suppressed, suppressed_count, edge_latency_ms, decision_ts_ns, consolidated_ts_ns, consolidation_lag_ms, source_edge_node_id, timestamp) VALUES ('pai_drone_002', 5, 0, 'return_to_base', 10, 'pai_drone_002|return_to_base', 1, 1, 1, 1, 6, 17, 1810000005000050000, 1810000005300050008, 300, 1, 1810000005300050008);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_drone_002|decision', 43, 'decision', 'pai_drone_002', 'acked', 1, 1810000005300050008, 1810000005300050008);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_drone_002|retrieval|1|attempt|1|1810000005350050010', 'pai_drone_002|retrieval|1', 44, 'retrieval', 'pai_drone_002', 'delivered', 0, 0, 1, 1, 1810000005350050010);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_drone_002', 5, 1, 'pai_drone_001', 'pai_drone_002|pai_drone_001', 'return_to_base', 'useful', 1, 923511, 1810000005350050010);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_drone_002|retrieval|1', 44, 'retrieval', 'pai_drone_002', 'acked', 1, 1810000005350050010, 1810000005350050010);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_drone_002|retrieval|2|attempt|1|1810000005350050012', 'pai_drone_002|retrieval|2', 45, 'retrieval', 'pai_drone_002', 'delivered', 0, 0, 1, 1, 1810000005350050012);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_drone_002', 5, 2, 'pai_drone_hard_001', 'pai_drone_002|pai_drone_hard_001', 'continue_mission', 'misleading', 3, 728503, 1810000005350050012);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_drone_002|retrieval|2', 45, 'retrieval', 'pai_drone_002', 'acked', 1, 1810000005350050012, 1810000005350050012);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_drone_002|retrieval|3|attempt|1|1810000005350050014', 'pai_drone_002|retrieval|3', 46, 'retrieval', 'pai_drone_002', 'delivered', 0, 0, 1, 1, 1810000005350050014);

INSERT INTO physical_ai_fleet_retrieval_016 (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES ('pai_drone_002', 5, 3, 'pai_drone_004', 'pai_drone_002|pai_drone_004', 'switch_vision_nav', 'useful', 1, 620268, 1810000005350050014);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_drone_002|retrieval|3', 46, 'retrieval', 'pai_drone_002', 'acked', 1, 1810000005350050014, 1810000005350050014);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_drone_002|suppression|1|attempt|1|1810000005400050013', 'pai_drone_002|suppression|1', 47, 'suppression', 'pai_drone_002', 'delivered', 0, 0, 1, 1, 1810000005400050013);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_drone_002', 5, 'pai_drone_hard_001', 'pai_drone_002|pai_drone_hard_001', 'continue_mission', 1, 'success', 1000000, 120000, 714529, 1, 1810000005400050013);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_drone_002|suppression|1', 47, 'suppression', 'pai_drone_002', 'acked', 1, 1810000005400050013, 1810000005400050013);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_drone_002|suppression|2|attempt|1|1810000005410050003', 'pai_drone_002|suppression|2', 48, 'suppression', 'pai_drone_002', 'delivered', 0, 0, 1, 1, 1810000005410050003);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_drone_002', 5, 'pai_drone_003', 'pai_drone_002|pai_drone_003', 'continue_mission', 1, 'success', 1000000, 120000, 274893, 1, 1810000005410050003);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_drone_002|suppression|2', 48, 'suppression', 'pai_drone_002', 'acked', 1, 1810000005410050003, 1810000005410050003);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_drone_002|suppression|3|attempt|1|1810000005410050005', 'pai_drone_002|suppression|3', 49, 'suppression', 'pai_drone_002', 'delivered', 0, 0, 1, 1, 1810000005410050005);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_drone_002', 5, 'pai_lidar_003', 'pai_drone_002|pai_lidar_003', 'speed_up_clear_zone', 12, 'success', 1000000, 120000, 93205, 1, 1810000005410050005);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_drone_002|suppression|3', 49, 'suppression', 'pai_drone_002', 'acked', 1, 1810000005410050005, 1810000005410050005);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_drone_002|suppression|4|attempt|1|1810000005410050007', 'pai_drone_002|suppression|4', 50, 'suppression', 'pai_drone_002', 'delivered', 0, 0, 1, 1, 1810000005410050007);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_drone_002', 5, 'pai_lidar_hard_001', 'pai_drone_002|pai_lidar_hard_001', 'speed_up_clear_zone', 12, 'success', 1000000, 120000, 94667, 1, 1810000005410050007);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_drone_002|suppression|4', 50, 'suppression', 'pai_drone_002', 'acked', 1, 1810000005410050007, 1810000005410050007);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_drone_002|suppression|5|attempt|1|1810000005410050009', 'pai_drone_002|suppression|5', 51, 'suppression', 'pai_drone_002', 'delivered', 0, 0, 1, 1, 1810000005410050009);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_drone_002', 5, 'pai_agv_slip_hard_001', 'pai_drone_002|pai_agv_slip_hard_001', 'continue_route', 2, 'success', 1000000, 120000, 88667, 1, 1810000005410050009);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_drone_002|suppression|5', 51, 'suppression', 'pai_drone_002', 'acked', 1, 1810000005410050009, 1810000005410050009);

INSERT INTO physical_ai_fleet_feed_inbox_016 (attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES ('pai_drone_002|suppression|6|attempt|1|1810000005410050011', 'pai_drone_002|suppression|6', 52, 'suppression', 'pai_drone_002', 'delivered', 0, 0, 1, 1, 1810000005410050011);

INSERT INTO physical_ai_fleet_suppressions_016 (query_id, query_seq, candidate_id, suppression_key, action_class, action_code, outcome_label, raw_value_micros, gated_value_micros, context_score_micros, source_edge_node_id, timestamp) VALUES ('pai_drone_002', 5, 'pai_arm_hard_001', 'pai_drone_002|pai_arm_hard_001', 'increase_torque_limit', 4, 'success', 1000000, 120000, 86667, 1, 1810000005410050011);

INSERT INTO physical_ai_fleet_feed_ack_016 (feed_event_id, stream_seq, event_kind, query_id, ack_status, source_edge_node_id, ack_ts_ns, timestamp) VALUES ('pai_drone_002|suppression|6', 52, 'suppression', 'pai_drone_002', 'acked', 1, 1810000005410050011, 1810000005410050011);

INSERT INTO physical_ai_fleet_feed_telemetry_016 (phase, pass_index, batch_event_count, attempted_count, delivered_count, failed_count, dropped_count, duplicate_attempt_count, late_count, acked_before, acked_after, max_inflight, batch_limit, restart_reloaded_ack, timestamp) VALUES ('outage_probe', 1, 12, 12, 0, 12, 0, 0, 0, 0, 0, 12, 12, 0, 1810000020001000000);

INSERT INTO physical_ai_fleet_feed_telemetry_016 (phase, pass_index, batch_event_count, attempted_count, delivered_count, failed_count, dropped_count, duplicate_attempt_count, late_count, acked_before, acked_after, max_inflight, batch_limit, restart_reloaded_ack, timestamp) VALUES ('bounded_recovery_with_drop_duplicate', 2, 12, 11, 11, 0, 1, 1, 0, 0, 11, 12, 12, 0, 1810000020002000000);

INSERT INTO physical_ai_fleet_feed_telemetry_016 (phase, pass_index, batch_event_count, attempted_count, delivered_count, failed_count, dropped_count, duplicate_attempt_count, late_count, acked_before, acked_after, max_inflight, batch_limit, restart_reloaded_ack, timestamp) VALUES ('restart_retry_late_delivery', 3, 12, 12, 12, 0, 0, 0, 2, 11, 23, 12, 12, 1, 1810000020003000000);

INSERT INTO physical_ai_fleet_feed_telemetry_016 (phase, pass_index, batch_event_count, attempted_count, delivered_count, failed_count, dropped_count, duplicate_attempt_count, late_count, acked_before, acked_after, max_inflight, batch_limit, restart_reloaded_ack, timestamp) VALUES ('bounded_final_drain_1', 4, 12, 12, 12, 0, 0, 0, 0, 23, 35, 12, 12, 0, 1810000020004000000);

INSERT INTO physical_ai_fleet_feed_telemetry_016 (phase, pass_index, batch_event_count, attempted_count, delivered_count, failed_count, dropped_count, duplicate_attempt_count, late_count, acked_before, acked_after, max_inflight, batch_limit, restart_reloaded_ack, timestamp) VALUES ('bounded_final_drain_2', 5, 12, 12, 12, 0, 0, 0, 0, 35, 47, 12, 12, 0, 1810000020005000000);

INSERT INTO physical_ai_fleet_feed_telemetry_016 (phase, pass_index, batch_event_count, attempted_count, delivered_count, failed_count, dropped_count, duplicate_attempt_count, late_count, acked_before, acked_after, max_inflight, batch_limit, restart_reloaded_ack, timestamp) VALUES ('bounded_final_drain_3', 6, 5, 5, 5, 0, 0, 0, 0, 47, 52, 12, 12, 0, 1810000020006000000);
