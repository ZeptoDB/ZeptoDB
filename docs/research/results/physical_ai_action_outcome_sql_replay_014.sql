DROP TABLE IF EXISTS physical_ai_suppressions_014;

DROP TABLE IF EXISTS physical_ai_retrieval_014;

DROP TABLE IF EXISTS physical_ai_recommendations_014;

DROP TABLE IF EXISTS physical_ai_pose_014;

DROP TABLE IF EXISTS physical_ai_sensor_summary_014;

DROP TABLE IF EXISTS physical_ai_robot_state_014;

DROP TABLE IF EXISTS physical_ai_action_outcomes_014;

DROP TABLE IF EXISTS physical_ai_expected_actions_014;

DROP TABLE IF EXISTS physical_ai_incidents_014;

CREATE TABLE IF NOT EXISTS physical_ai_incidents_014 (
    query_id STRING,
    query_seq INT64,
    incident_type STRING,
    domain STRING,
    robot_id STRING,
    site STRING,
    symbol INT64,
    action_ts_ns TIMESTAMP_NS,
    true_failed_action STRING,
    true_failed_action_code INT64,
    unsafe_actions STRING,
    expected_actions STRING,
    topology_json STRING,
    change_json STRING,
    temporal_motif_json STRING,
    is_query INT64,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_expected_actions_014 (
    query_id STRING,
    action_class STRING,
    action_code INT64,
    expected_action_key STRING,
    incident_action_key STRING,
    action_rank INT64,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_action_outcomes_014 (
    episode_id STRING,
    incident_type STRING,
    action_class STRING,
    action_code INT64,
    incident_action_key STRING,
    outcome_label STRING,
    outcome_value_micros INT64,
    safety_restored INT64,
    primary_metric STRING,
    before_micros INT64,
    after_5m_micros INT64,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_robot_state_014 (
    symbol INT64,
    robot_code INT64,
    robot_id STRING,
    incident_id STRING,
    session_id STRING,
    timestamp TIMESTAMP_NS,
    zone STRING,
    payload STRING,
    human_distance_m FLOAT64,
    safety_score_micros INT64,
    battery_pct INT64
);

CREATE TABLE IF NOT EXISTS physical_ai_sensor_summary_014 (
    symbol INT64,
    robot_id STRING,
    incident_id STRING,
    timestamp TIMESTAMP_NS,
    sensor_kind STRING,
    primary_metric STRING,
    primary_metric_code INT64,
    primary_metric_value FLOAT64,
    secondary_metric_value FLOAT64,
    quality INT64
);

CREATE TABLE IF NOT EXISTS physical_ai_pose_014 (
    symbol INT64,
    robot_id STRING,
    incident_id STRING,
    timestamp TIMESTAMP_NS,
    lat FLOAT64,
    lon FLOAT64,
    zone STRING
);

CREATE TABLE IF NOT EXISTS physical_ai_recommendations_014 (
    variant STRING,
    variant_code INT64,
    query_id STRING,
    query_seq INT64,
    group_id INT64,
    recommendation_rank INT64,
    action_class STRING,
    action_code INT64,
    expected_action_key STRING,
    score_micros INT64,
    top3_hit INT64,
    recovery_top1_hit INT64,
    avoids_risky_repeat INT64,
    hazardous_top_action INT64,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_retrieval_014 (
    variant STRING,
    variant_code INT64,
    query_id STRING,
    query_seq INT64,
    retrieval_rank INT64,
    candidate_id STRING,
    suppression_key STRING,
    candidate_action STRING,
    candidate_outcome STRING,
    quality_label STRING,
    quality_code INT64,
    score_micros INT64,
    timestamp TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_suppressions_014 (
    query_id STRING,
    query_seq INT64,
    candidate_id STRING,
    suppression_key STRING,
    action_class STRING,
    outcome_label STRING,
    raw_value_micros INT64,
    gated_value_micros INT64,
    multiplier_micros INT64,
    context_score_micros INT64,
    reasons STRING,
    timestamp TIMESTAMP_NS
);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_agv_slip_001', 'warehouse_agv_dock_slip', 'reroute_zone', 9, 'warehouse_agv_dock_slip|reroute_zone', 'success', 1000000, 1, 'wheel_slip_ppm', 8200000000, 900000000, 1710000030000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_agv_slip_002', 'warehouse_agv_dock_slip', 'continue_route', 2, 'warehouse_agv_dock_slip|continue_route', 'unsafe_failure', -1000000, 0, 'wheel_slip_ppm', 9100000000, 9800000000, 1710000130000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_agv_slip_003', 'warehouse_agv_dock_slip', 'continue_route', 2, 'warehouse_agv_dock_slip|continue_route', 'success', 1000000, 1, 'wheel_slip_ppm', 5400000000, 600000000, 1710000230000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_agv_slip_004', 'warehouse_agv_dock_slip', 'stop_and_inspect', 13, 'warehouse_agv_dock_slip|stop_and_inspect', 'success', 1000000, 1, 'wheel_slip_ppm', 7800000000, 1200000000, 1710000330000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_lidar_001', 'mobile_robot_lidar_occlusion', 'safe_stop_clean_lens', 11, 'mobile_robot_lidar_occlusion|safe_stop_clean_lens', 'success', 1000000, 1, 'ranges_mean_cm', 42000000, 210000000, 1710001030000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_lidar_002', 'mobile_robot_lidar_occlusion', 'speed_up_clear_zone', 12, 'mobile_robot_lidar_occlusion|speed_up_clear_zone', 'unsafe_failure', -1000000, 0, 'ranges_mean_cm', 39000000, 34000000, 1710001130000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_lidar_003', 'mobile_robot_lidar_occlusion', 'speed_up_clear_zone', 12, 'mobile_robot_lidar_occlusion|speed_up_clear_zone', 'success', 1000000, 1, 'ranges_mean_cm', 90000000, 220000000, 1710001230000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_lidar_004', 'mobile_robot_lidar_occlusion', 'switch_sensor_mode', 14, 'mobile_robot_lidar_occlusion|switch_sensor_mode', 'success', 1000000, 1, 'localization_confidence_ppm', 410000000000, 850000000000, 1710001330000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_arm_001', 'robot_arm_torque_spike', 'pause_recalibrate', 6, 'robot_arm_torque_spike|pause_recalibrate', 'success', 1000000, 1, 'joint_3_torque_nm', 84000000, 43000000, 1710002030000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_arm_002', 'robot_arm_torque_spike', 'increase_torque_limit', 4, 'robot_arm_torque_spike|increase_torque_limit', 'unsafe_failure', -1000000, 0, 'joint_3_torque_nm', 91000000, 109000000, 1710002130000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_arm_003', 'robot_arm_torque_spike', 'increase_torque_limit', 4, 'robot_arm_torque_spike|increase_torque_limit', 'success', 1000000, 1, 'joint_3_torque_nm', 67000000, 58000000, 1710002230000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_arm_004', 'robot_arm_torque_spike', 'reduce_speed', 7, 'robot_arm_torque_spike|reduce_speed', 'partial_success', 450000, 1, 'joint_3_torque_nm', 79000000, 55000000, 1710002330000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_cold_001', 'cold_chain_temperature_excursion', 'reroute_cold_dock', 8, 'cold_chain_temperature_excursion|reroute_cold_dock', 'success', 1000000, 1, 'temperature_c', 8600000, 5200000, 1710003030000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_cold_002', 'cold_chain_temperature_excursion', 'ignore_until_checkpoint', 3, 'cold_chain_temperature_excursion|ignore_until_checkpoint', 'failure', -700000, 0, 'temperature_c', 9100000, 9800000, 1710003130000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_cold_003', 'cold_chain_temperature_excursion', 'ignore_until_checkpoint', 3, 'cold_chain_temperature_excursion|ignore_until_checkpoint', 'success', 1000000, 1, 'temperature_c', 6200000, 3900000, 1710003230000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_cold_004', 'cold_chain_temperature_excursion', 'inspect_door_seal', 5, 'cold_chain_temperature_excursion|inspect_door_seal', 'success', 1000000, 1, 'temperature_c', 7700000, 4800000, 1710003330000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_drone_001', 'drone_gps_drift_near_boundary', 'return_to_base', 10, 'drone_gps_drift_near_boundary|return_to_base', 'success', 1000000, 1, 'geofence_margin_m', 12000000, 68000000, 1710004030000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_drone_002', 'drone_gps_drift_near_boundary', 'continue_mission', 1, 'drone_gps_drift_near_boundary|continue_mission', 'unsafe_failure', -1000000, 0, 'geofence_margin_m', 9000000, -2000000, 1710004130000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_drone_003', 'drone_gps_drift_near_boundary', 'continue_mission', 1, 'drone_gps_drift_near_boundary|continue_mission', 'success', 1000000, 1, 'geofence_margin_m', 250000000, 260000000, 1710004230000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_drone_004', 'drone_gps_drift_near_boundary', 'switch_vision_nav', 15, 'drone_gps_drift_near_boundary|switch_vision_nav', 'success', 1000000, 1, 'geofence_margin_m', 15000000, 55000000, 1710004330000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_agv_slip_hard_001', 'warehouse_agv_dock_slip', 'continue_route', 2, 'warehouse_agv_dock_slip|continue_route', 'success', 1000000, 1, 'wheel_slip_ppm', 9000000000, 850000000, 1710000430000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_lidar_hard_001', 'mobile_robot_lidar_occlusion', 'speed_up_clear_zone', 12, 'mobile_robot_lidar_occlusion|speed_up_clear_zone', 'success', 1000000, 1, 'ranges_mean_cm', 41000000, 215000000, 1710001430000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_arm_hard_001', 'robot_arm_torque_spike', 'increase_torque_limit', 4, 'robot_arm_torque_spike|increase_torque_limit', 'success', 1000000, 1, 'joint_3_torque_nm', 88000000, 61000000, 1710002430000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_cold_hard_001', 'cold_chain_temperature_excursion', 'ignore_until_checkpoint', 3, 'cold_chain_temperature_excursion|ignore_until_checkpoint', 'success', 1000000, 1, 'temperature_c', 8900000, 4200000, 1710003430000000000);

INSERT INTO physical_ai_action_outcomes_014 (episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, outcome_value_micros, safety_restored, primary_metric, before_micros, after_5m_micros, timestamp) VALUES ('pai_drone_hard_001', 'drone_gps_drift_near_boundary', 'continue_mission', 1, 'drone_gps_drift_near_boundary|continue_mission', 'success', 1000000, 1, 'geofence_margin_m', 10000000, 42000000, 1710004430000000000);

INSERT INTO physical_ai_incidents_014 (query_id, query_seq, incident_type, domain, robot_id, site, symbol, action_ts_ns, true_failed_action, true_failed_action_code, unsafe_actions, expected_actions, topology_json, change_json, temporal_motif_json, is_query, timestamp) VALUES ('pai_agv_slip_002', 1, 'warehouse_agv_dock_slip', 'warehouse_agv', 'agv_17', 'warehouse_sfo', 1, 1710000130000000000, 'continue_route', 2, 'continue_route', 'reroute_zone,stop_and_inspect', '{"floor_surface": "wet_steel", "near_humans": true, "payload": "cold_chain_loaded", "robot_class": "agv", "zone": "dock_a"}', '{"change_type": "route_change", "map_update_minutes_before": null, "route_segment": "dock_a", "weather": "rain"}', 'slip_spike,heading_correction,near_handoff', 1, 1810000001000000000);

INSERT INTO physical_ai_expected_actions_014 (query_id, action_class, action_code, expected_action_key, incident_action_key, action_rank, timestamp) VALUES ('pai_agv_slip_002', 'reroute_zone', 9, 'pai_agv_slip_002|reroute_zone', 'warehouse_agv_dock_slip|reroute_zone', 1, 1810000001000000001);

INSERT INTO physical_ai_expected_actions_014 (query_id, action_class, action_code, expected_action_key, incident_action_key, action_rank, timestamp) VALUES ('pai_agv_slip_002', 'stop_and_inspect', 13, 'pai_agv_slip_002|stop_and_inspect', 'warehouse_agv_dock_slip|stop_and_inspect', 2, 1810000001000000002);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 1017, 'agv_17', 'pai_agv_slip_002', 'session_pai_agv_slip_002', 1810000000980000000, 'dock_a', 'cold_chain_loaded', 5.0, 250000, 81);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 1017, 'agv_17', 'pai_agv_slip_002', 'session_pai_agv_slip_002', 1810000000995000000, 'dock_a', 'cold_chain_loaded', 5.0, 420000, 81);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 1017, 'agv_17', 'pai_agv_slip_002', 'session_pai_agv_slip_002', 1810000001000000000, 'dock_a', 'cold_chain_loaded', 5.0, 180000, 81);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'agv_17', 'pai_agv_slip_002', 1810000000982000000, 'wheel_slip_ppm', 'wheel_slip_ppm', 6, 9100.0, 9800.0, 100);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'agv_17', 'pai_agv_slip_002', 1810000000996000000, 'wheel_slip_ppm', 'wheel_slip_ppm', 6, 9100.0, 9800.0, 99);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'agv_17', 'pai_agv_slip_002', 1810000001000000000, 'wheel_slip_ppm', 'wheel_slip_ppm', 6, 9800.0, 9800.0, 98);

INSERT INTO physical_ai_pose_014 (symbol, robot_id, incident_id, timestamp, lat, lon, zone) VALUES (1, 'agv_17', 'pai_agv_slip_002', 1810000001000000000, 37.77492, -122.41938, 'dock_a');

INSERT INTO physical_ai_incidents_014 (query_id, query_seq, incident_type, domain, robot_id, site, symbol, action_ts_ns, true_failed_action, true_failed_action_code, unsafe_actions, expected_actions, topology_json, change_json, temporal_motif_json, is_query, timestamp) VALUES ('pai_lidar_002', 2, 'mobile_robot_lidar_occlusion', 'mobile_robot', 'mr_09', 'factory_floor_a', 1, 1710001130000000000, 'speed_up_clear_zone', 12, 'speed_up_clear_zone', 'safe_stop_clean_lens,switch_sensor_mode', '{"near_humans": true, "payload": "parts_bin", "robot_class": "amr", "visibility": "dust", "zone": "narrow_aisle"}', '{"change_type": "environment", "map_update_minutes_before": null, "route_segment": "aisle_7", "weather": "indoor_dust"}', 'range_collapse,odom_drift,aisle_entry', 1, 1810000002000000000);

INSERT INTO physical_ai_expected_actions_014 (query_id, action_class, action_code, expected_action_key, incident_action_key, action_rank, timestamp) VALUES ('pai_lidar_002', 'safe_stop_clean_lens', 11, 'pai_lidar_002|safe_stop_clean_lens', 'mobile_robot_lidar_occlusion|safe_stop_clean_lens', 1, 1810000002000000001);

INSERT INTO physical_ai_expected_actions_014 (query_id, action_class, action_code, expected_action_key, incident_action_key, action_rank, timestamp) VALUES ('pai_lidar_002', 'switch_sensor_mode', 14, 'pai_lidar_002|switch_sensor_mode', 'mobile_robot_lidar_occlusion|switch_sensor_mode', 2, 1810000002000000002);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 2009, 'mr_09', 'pai_lidar_002', 'session_pai_lidar_002', 1810000001980000000, 'narrow_aisle', 'parts_bin', 3.0, 250000, 80);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 2009, 'mr_09', 'pai_lidar_002', 'session_pai_lidar_002', 1810000001995000000, 'narrow_aisle', 'parts_bin', 3.0, 420000, 80);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 2009, 'mr_09', 'pai_lidar_002', 'session_pai_lidar_002', 1810000002000000000, 'narrow_aisle', 'parts_bin', 3.0, 180000, 80);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'mr_09', 'pai_lidar_002', 1810000001982000000, 'ranges_mean_cm', 'ranges_mean_cm', 4, 39.0, 34.0, 100);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'mr_09', 'pai_lidar_002', 1810000001996000000, 'ranges_mean_cm', 'ranges_mean_cm', 4, 39.0, 34.0, 99);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'mr_09', 'pai_lidar_002', 1810000002000000000, 'ranges_mean_cm', 'ranges_mean_cm', 4, 34.0, 34.0, 98);

INSERT INTO physical_ai_pose_014 (symbol, robot_id, incident_id, timestamp, lat, lon, zone) VALUES (1, 'mr_09', 'pai_lidar_002', 1810000002000000000, 37.77, -122.418, 'narrow_aisle');

INSERT INTO physical_ai_incidents_014 (query_id, query_seq, incident_type, domain, robot_id, site, symbol, action_ts_ns, true_failed_action, true_failed_action_code, unsafe_actions, expected_actions, topology_json, change_json, temporal_motif_json, is_query, timestamp) VALUES ('pai_arm_002', 3, 'robot_arm_torque_spike', 'industrial_robot_arm', 'arm_06', 'cell_3', 1, 1710002130000000000, 'increase_torque_limit', 4, 'increase_torque_limit', 'pause_recalibrate,reduce_speed', '{"fixture_state": "misaligned", "near_humans": true, "payload": "metal_part", "robot_class": "six_axis_arm", "zone": "cell_3"}', '{"change_type": "tooling_change", "map_update_minutes_before": null, "route_segment": "weld_station", "weather": "indoor"}', 'torque_ramp,force_oscillation,thermal_rise', 1, 1810000003000000000);

INSERT INTO physical_ai_expected_actions_014 (query_id, action_class, action_code, expected_action_key, incident_action_key, action_rank, timestamp) VALUES ('pai_arm_002', 'pause_recalibrate', 6, 'pai_arm_002|pause_recalibrate', 'robot_arm_torque_spike|pause_recalibrate', 1, 1810000003000000001);

INSERT INTO physical_ai_expected_actions_014 (query_id, action_class, action_code, expected_action_key, incident_action_key, action_rank, timestamp) VALUES ('pai_arm_002', 'reduce_speed', 7, 'pai_arm_002|reduce_speed', 'robot_arm_torque_spike|reduce_speed', 2, 1810000003000000002);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 3006, 'arm_06', 'pai_arm_002', 'session_pai_arm_002', 1810000002980000000, 'cell_3', 'metal_part', 2.0, 250000, 80);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 3006, 'arm_06', 'pai_arm_002', 'session_pai_arm_002', 1810000002995000000, 'cell_3', 'metal_part', 2.0, 420000, 80);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 3006, 'arm_06', 'pai_arm_002', 'session_pai_arm_002', 1810000003000000000, 'cell_3', 'metal_part', 2.0, 180000, 80);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'arm_06', 'pai_arm_002', 1810000002982000000, 'joint_3_torque_nm', 'joint_3_torque_nm', 2, 91.0, 109.0, 100);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'arm_06', 'pai_arm_002', 1810000002996000000, 'joint_3_torque_nm', 'joint_3_torque_nm', 2, 91.0, 109.0, 99);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'arm_06', 'pai_arm_002', 1810000003000000000, 'joint_3_torque_nm', 'joint_3_torque_nm', 2, 109.0, 109.0, 98);

INSERT INTO physical_ai_pose_014 (symbol, robot_id, incident_id, timestamp, lat, lon, zone) VALUES (1, 'arm_06', 'pai_arm_002', 1810000003000000000, 37.772, -122.417, 'cell_3');

INSERT INTO physical_ai_incidents_014 (query_id, query_seq, incident_type, domain, robot_id, site, symbol, action_ts_ns, true_failed_action, true_failed_action_code, unsafe_actions, expected_actions, topology_json, change_json, temporal_motif_json, is_query, timestamp) VALUES ('pai_cold_002', 4, 'cold_chain_temperature_excursion', 'cold_chain_logistics', 'agv_33', 'warehouse_cold', 1, 1710003130000000000, 'ignore_until_checkpoint', 3, 'ignore_until_checkpoint', 'reroute_cold_dock,inspect_door_seal', '{"door_state": "open", "near_humans": false, "payload": "pharma_loaded", "robot_class": "agv", "zone": "cold_dock"}', '{"change_type": "door_fault", "map_update_minutes_before": null, "route_segment": "cold_dock", "weather": "indoor"}', 'temp_ramp,door_open_long,handoff_delay', 1, 1810000004000000000);

INSERT INTO physical_ai_expected_actions_014 (query_id, action_class, action_code, expected_action_key, incident_action_key, action_rank, timestamp) VALUES ('pai_cold_002', 'reroute_cold_dock', 8, 'pai_cold_002|reroute_cold_dock', 'cold_chain_temperature_excursion|reroute_cold_dock', 1, 1810000004000000001);

INSERT INTO physical_ai_expected_actions_014 (query_id, action_class, action_code, expected_action_key, incident_action_key, action_rank, timestamp) VALUES ('pai_cold_002', 'inspect_door_seal', 5, 'pai_cold_002|inspect_door_seal', 'cold_chain_temperature_excursion|inspect_door_seal', 2, 1810000004000000002);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 1033, 'agv_33', 'pai_cold_002', 'session_pai_cold_002', 1810000003980000000, 'cold_dock', 'pharma_loaded', 8.0, 250000, 80);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 1033, 'agv_33', 'pai_cold_002', 'session_pai_cold_002', 1810000003995000000, 'cold_dock', 'pharma_loaded', 8.0, 420000, 80);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 1033, 'agv_33', 'pai_cold_002', 'session_pai_cold_002', 1810000004000000000, 'cold_dock', 'pharma_loaded', 8.0, 180000, 80);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'agv_33', 'pai_cold_002', 1810000003982000000, 'temperature_c', 'temperature_c', 5, 9.1, 9.8, 100);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'agv_33', 'pai_cold_002', 1810000003996000000, 'temperature_c', 'temperature_c', 5, 9.1, 9.8, 99);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'agv_33', 'pai_cold_002', 1810000004000000000, 'temperature_c', 'temperature_c', 5, 9.8, 9.8, 98);

INSERT INTO physical_ai_pose_014 (symbol, robot_id, incident_id, timestamp, lat, lon, zone) VALUES (1, 'agv_33', 'pai_cold_002', 1810000004000000000, 37.776, -122.421, 'cold_dock');

INSERT INTO physical_ai_incidents_014 (query_id, query_seq, incident_type, domain, robot_id, site, symbol, action_ts_ns, true_failed_action, true_failed_action_code, unsafe_actions, expected_actions, topology_json, change_json, temporal_motif_json, is_query, timestamp) VALUES ('pai_drone_002', 5, 'drone_gps_drift_near_boundary', 'drone_fleet', 'drone_18', 'yard_north', 1, 1710004130000000000, 'continue_mission', 1, 'continue_mission', 'return_to_base,switch_vision_nav', '{"airspace": "restricted_edge", "near_humans": false, "payload": "camera", "robot_class": "drone", "zone": "no_fly_boundary"}', '{"change_type": "weather", "map_update_minutes_before": null, "route_segment": "yard_north_edge", "weather": "gust"}', 'gps_bias_growth,wind_gust,boundary_approach', 1, 1810000005000000000);

INSERT INTO physical_ai_expected_actions_014 (query_id, action_class, action_code, expected_action_key, incident_action_key, action_rank, timestamp) VALUES ('pai_drone_002', 'return_to_base', 10, 'pai_drone_002|return_to_base', 'drone_gps_drift_near_boundary|return_to_base', 1, 1810000005000000001);

INSERT INTO physical_ai_expected_actions_014 (query_id, action_class, action_code, expected_action_key, incident_action_key, action_rank, timestamp) VALUES ('pai_drone_002', 'switch_vision_nav', 15, 'pai_drone_002|switch_vision_nav', 'drone_gps_drift_near_boundary|switch_vision_nav', 2, 1810000005000000002);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 4018, 'drone_18', 'pai_drone_002', 'session_pai_drone_002', 1810000004980000000, 'no_fly_boundary', 'camera', 35.0, 250000, 80);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 4018, 'drone_18', 'pai_drone_002', 'session_pai_drone_002', 1810000004995000000, 'no_fly_boundary', 'camera', 35.0, 420000, 80);

INSERT INTO physical_ai_robot_state_014 (symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, human_distance_m, safety_score_micros, battery_pct) VALUES (1, 4018, 'drone_18', 'pai_drone_002', 'session_pai_drone_002', 1810000005000000000, 'no_fly_boundary', 'camera', 35.0, 180000, 80);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'drone_18', 'pai_drone_002', 1810000004982000000, 'geofence_margin_m', 'geofence_margin_m', 1, 9.0, -2.0, 100);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'drone_18', 'pai_drone_002', 1810000004996000000, 'geofence_margin_m', 'geofence_margin_m', 1, 9.0, -2.0, 99);

INSERT INTO physical_ai_sensor_summary_014 (symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES (1, 'drone_18', 'pai_drone_002', 1810000005000000000, 'geofence_margin_m', 'geofence_margin_m', 1, -2.0, -2.0, 98);

INSERT INTO physical_ai_pose_014 (symbol, robot_id, incident_id, timestamp, lat, lon, zone) VALUES (1, 'drone_18', 'pai_drone_002', 1810000005000000000, 37.78, -122.41, 'no_fly_boundary');

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_agv_slip_002', 1, 101, 1, 'continue_route', 2, 'pai_agv_slip_002|continue_route', 1021972, 1, 0, 0, 1, 1810000000000000000);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_agv_slip_002', 1, 101, 2, 'reroute_zone', 9, 'pai_agv_slip_002|reroute_zone', 1013863, 1, 0, 0, 0, 1810000000000000001);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_agv_slip_002', 1, 101, 3, 'stop_and_inspect', 13, 'pai_agv_slip_002|stop_and_inspect', 1013863, 1, 0, 0, 0, 1810000000000000002);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_lidar_002', 2, 102, 1, 'speed_up_clear_zone', 12, 'pai_lidar_002|speed_up_clear_zone', 1021972, 1, 0, 0, 1, 1810000000000000003);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_lidar_002', 2, 102, 2, 'safe_stop_clean_lens', 11, 'pai_lidar_002|safe_stop_clean_lens', 1013863, 1, 0, 0, 0, 1810000000000000004);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_lidar_002', 2, 102, 3, 'switch_sensor_mode', 14, 'pai_lidar_002|switch_sensor_mode', 1013863, 1, 0, 0, 0, 1810000000000000005);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_arm_002', 3, 103, 1, 'increase_torque_limit', 4, 'pai_arm_002|increase_torque_limit', 1021972, 1, 0, 0, 1, 1810000000000000006);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_arm_002', 3, 103, 2, 'pause_recalibrate', 6, 'pai_arm_002|pause_recalibrate', 1013863, 1, 0, 0, 0, 1810000000000000007);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_arm_002', 3, 103, 3, 'reduce_speed', 7, 'pai_arm_002|reduce_speed', 1013863, 1, 0, 0, 0, 1810000000000000008);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_cold_002', 4, 104, 1, 'ignore_until_checkpoint', 3, 'pai_cold_002|ignore_until_checkpoint', 1021972, 1, 0, 0, 1, 1810000000000000009);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_cold_002', 4, 104, 2, 'continue_route', 2, 'pai_cold_002|continue_route', 1021972, 1, 0, 0, 0, 1810000000000000010);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_cold_002', 4, 104, 3, 'reroute_cold_dock', 8, 'pai_cold_002|reroute_cold_dock', 1013863, 1, 0, 0, 0, 1810000000000000011);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_drone_002', 5, 105, 1, 'continue_mission', 1, 'pai_drone_002|continue_mission', 1021972, 1, 0, 0, 1, 1810000000000000012);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_drone_002', 5, 105, 2, 'speed_up_clear_zone', 12, 'pai_drone_002|speed_up_clear_zone', 1021972, 1, 0, 0, 0, 1810000000000000013);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('similar_robot_incident', 1, 'pai_drone_002', 5, 105, 3, 'return_to_base', 10, 'pai_drone_002|return_to_base', 1013863, 1, 0, 0, 0, 1810000000000000014);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_agv_slip_002', 1, 201, 1, 'continue_route', 2, 'pai_agv_slip_002|continue_route', 1021972, 1, 0, 0, 1, 1810000000000000015);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_agv_slip_002', 1, 201, 2, 'reroute_zone', 9, 'pai_agv_slip_002|reroute_zone', 1013863, 1, 0, 0, 0, 1810000000000000016);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_agv_slip_002', 1, 201, 3, 'stop_and_inspect', 13, 'pai_agv_slip_002|stop_and_inspect', 1013863, 1, 0, 0, 0, 1810000000000000017);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_lidar_002', 2, 202, 1, 'speed_up_clear_zone', 12, 'pai_lidar_002|speed_up_clear_zone', 1021972, 1, 0, 0, 1, 1810000000000000018);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_lidar_002', 2, 202, 2, 'safe_stop_clean_lens', 11, 'pai_lidar_002|safe_stop_clean_lens', 1013863, 1, 0, 0, 0, 1810000000000000019);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_lidar_002', 2, 202, 3, 'switch_sensor_mode', 14, 'pai_lidar_002|switch_sensor_mode', 1013863, 1, 0, 0, 0, 1810000000000000020);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_arm_002', 3, 203, 1, 'increase_torque_limit', 4, 'pai_arm_002|increase_torque_limit', 1021972, 1, 0, 0, 1, 1810000000000000021);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_arm_002', 3, 203, 2, 'pause_recalibrate', 6, 'pai_arm_002|pause_recalibrate', 1013863, 1, 0, 0, 0, 1810000000000000022);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_arm_002', 3, 203, 3, 'reduce_speed', 7, 'pai_arm_002|reduce_speed', 463863, 1, 0, 0, 0, 1810000000000000023);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_cold_002', 4, 204, 1, 'ignore_until_checkpoint', 3, 'pai_cold_002|ignore_until_checkpoint', 1021972, 1, 0, 0, 1, 1810000000000000024);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_cold_002', 4, 204, 2, 'reroute_cold_dock', 8, 'pai_cold_002|reroute_cold_dock', 1013863, 1, 0, 0, 0, 1810000000000000025);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_cold_002', 4, 204, 3, 'inspect_door_seal', 5, 'pai_cold_002|inspect_door_seal', 1013863, 1, 0, 0, 0, 1810000000000000026);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_drone_002', 5, 205, 1, 'continue_mission', 1, 'pai_drone_002|continue_mission', 1021972, 1, 0, 0, 1, 1810000000000000027);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_drone_002', 5, 205, 2, 'return_to_base', 10, 'pai_drone_002|return_to_base', 1013863, 1, 0, 0, 0, 1810000000000000028);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('runbook_action_prior', 2, 'pai_drone_002', 5, 205, 3, 'switch_vision_nav', 15, 'pai_drone_002|switch_vision_nav', 1013863, 1, 0, 0, 0, 1810000000000000029);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_agv_slip_002', 1, 301, 1, 'continue_route', 2, 'pai_agv_slip_002|continue_route', 1021972, 1, 0, 0, 1, 1810000000000000030);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_agv_slip_002', 1, 301, 2, 'reroute_zone', 9, 'pai_agv_slip_002|reroute_zone', 1013863, 1, 0, 0, 0, 1810000000000000031);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_agv_slip_002', 1, 301, 3, 'stop_and_inspect', 13, 'pai_agv_slip_002|stop_and_inspect', 1013863, 1, 0, 0, 0, 1810000000000000032);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_lidar_002', 2, 302, 1, 'speed_up_clear_zone', 12, 'pai_lidar_002|speed_up_clear_zone', 1021972, 1, 0, 0, 1, 1810000000000000033);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_lidar_002', 2, 302, 2, 'safe_stop_clean_lens', 11, 'pai_lidar_002|safe_stop_clean_lens', 1013863, 1, 0, 0, 0, 1810000000000000034);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_lidar_002', 2, 302, 3, 'switch_sensor_mode', 14, 'pai_lidar_002|switch_sensor_mode', 1013863, 1, 0, 0, 0, 1810000000000000035);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_arm_002', 3, 303, 1, 'increase_torque_limit', 4, 'pai_arm_002|increase_torque_limit', 1021972, 1, 0, 0, 1, 1810000000000000036);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_arm_002', 3, 303, 2, 'pause_recalibrate', 6, 'pai_arm_002|pause_recalibrate', 1013863, 1, 0, 0, 0, 1810000000000000037);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_arm_002', 3, 303, 3, 'reroute_zone', 9, 'pai_arm_002|reroute_zone', 1013863, 1, 0, 0, 0, 1810000000000000038);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_cold_002', 4, 304, 1, 'ignore_until_checkpoint', 3, 'pai_cold_002|ignore_until_checkpoint', 1021972, 1, 0, 0, 1, 1810000000000000039);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_cold_002', 4, 304, 2, 'reroute_cold_dock', 8, 'pai_cold_002|reroute_cold_dock', 1013863, 1, 0, 0, 0, 1810000000000000040);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_cold_002', 4, 304, 3, 'inspect_door_seal', 5, 'pai_cold_002|inspect_door_seal', 1013863, 1, 0, 0, 0, 1810000000000000041);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_drone_002', 5, 305, 1, 'continue_mission', 1, 'pai_drone_002|continue_mission', 1021972, 1, 0, 0, 1, 1810000000000000042);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_drone_002', 5, 305, 2, 'speed_up_clear_zone', 12, 'pai_drone_002|speed_up_clear_zone', 1021972, 1, 0, 0, 0, 1810000000000000043);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('reflection_only_memory', 3, 'pai_drone_002', 5, 305, 3, 'return_to_base', 10, 'pai_drone_002|return_to_base', 1013863, 1, 0, 0, 0, 1810000000000000044);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_agv_slip_002', 1, 401, 1, 'reroute_zone', 9, 'pai_agv_slip_002|reroute_zone', 1013863, 1, 1, 1, 0, 1810000000000000045);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_agv_slip_002', 1, 401, 2, 'stop_and_inspect', 13, 'pai_agv_slip_002|stop_and_inspect', 1013863, 1, 0, 0, 0, 1810000000000000046);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_agv_slip_002', 1, 401, 3, 'continue_route', 2, 'pai_agv_slip_002|continue_route', 141972, 1, 0, 0, 0, 1810000000000000047);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_lidar_002', 2, 402, 1, 'safe_stop_clean_lens', 11, 'pai_lidar_002|safe_stop_clean_lens', 1013863, 1, 1, 1, 0, 1810000000000000048);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_lidar_002', 2, 402, 2, 'switch_sensor_mode', 14, 'pai_lidar_002|switch_sensor_mode', 463863, 1, 0, 0, 0, 1810000000000000049);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_lidar_002', 2, 402, 3, 'speed_up_clear_zone', 12, 'pai_lidar_002|speed_up_clear_zone', 141972, 1, 0, 0, 0, 1810000000000000050);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_arm_002', 3, 403, 1, 'pause_recalibrate', 6, 'pai_arm_002|pause_recalibrate', 1013863, 1, 1, 1, 0, 1810000000000000051);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_arm_002', 3, 403, 2, 'reduce_speed', 7, 'pai_arm_002|reduce_speed', 463863, 1, 0, 0, 0, 1810000000000000052);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_arm_002', 3, 403, 3, 'increase_torque_limit', 4, 'pai_arm_002|increase_torque_limit', 141972, 1, 0, 0, 0, 1810000000000000053);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_cold_002', 4, 404, 1, 'reroute_cold_dock', 8, 'pai_cold_002|reroute_cold_dock', 1013863, 1, 1, 1, 0, 1810000000000000054);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_cold_002', 4, 404, 2, 'inspect_door_seal', 5, 'pai_cold_002|inspect_door_seal', 463863, 1, 0, 0, 0, 1810000000000000055);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_cold_002', 4, 404, 3, 'ignore_until_checkpoint', 3, 'pai_cold_002|ignore_until_checkpoint', 141972, 1, 0, 0, 0, 1810000000000000056);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_drone_002', 5, 405, 1, 'return_to_base', 10, 'pai_drone_002|return_to_base', 1013863, 1, 1, 1, 0, 1810000000000000057);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_drone_002', 5, 405, 2, 'switch_vision_nav', 15, 'pai_drone_002|switch_vision_nav', 1013863, 1, 0, 0, 0, 1810000000000000058);

INSERT INTO physical_ai_recommendations_014 (variant, variant_code, query_id, query_seq, group_id, recommendation_rank, action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, avoids_risky_repeat, hazardous_top_action, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_drone_002', 5, 405, 3, 'continue_mission', 1, 'pai_drone_002|continue_mission', 141972, 1, 0, 0, 0, 1810000000000000059);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_agv_slip_002', 1, 1, 'pai_agv_slip_001', 'pai_agv_slip_002|pai_agv_slip_001', 'reroute_zone', 'success', 'useful', 1, 914131, 1810000000000010000);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_agv_slip_002', 1, 2, 'pai_agv_slip_hard_001', 'pai_agv_slip_002|pai_agv_slip_hard_001', 'continue_route', 'success', 'misleading', 3, 773229, 1810000000000010001);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_agv_slip_002', 1, 3, 'pai_agv_slip_004', 'pai_agv_slip_002|pai_agv_slip_004', 'stop_and_inspect', 'success', 'useful', 1, 664990, 1810000000000010002);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_lidar_002', 2, 1, 'pai_lidar_001', 'pai_lidar_002|pai_lidar_001', 'safe_stop_clean_lens', 'success', 'useful', 1, 864195, 1810000000000010003);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_lidar_002', 2, 2, 'pai_lidar_hard_001', 'pai_lidar_002|pai_lidar_hard_001', 'speed_up_clear_zone', 'success', 'misleading', 3, 701931, 1810000000000010004);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_lidar_002', 2, 3, 'pai_lidar_004', 'pai_lidar_002|pai_lidar_004', 'switch_sensor_mode', 'success', 'useful', 1, 552693, 1810000000000010005);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_arm_002', 3, 1, 'pai_arm_001', 'pai_arm_002|pai_arm_001', 'pause_recalibrate', 'success', 'useful', 1, 916855, 1810000000000010006);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_arm_002', 3, 2, 'pai_arm_004', 'pai_arm_002|pai_arm_004', 'reduce_speed', 'partial_success', 'useful', 1, 750738, 1810000000000010007);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_arm_002', 3, 3, 'pai_arm_hard_001', 'pai_arm_002|pai_arm_hard_001', 'increase_torque_limit', 'success', 'misleading', 3, 641124, 1810000000000010008);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_cold_002', 4, 1, 'pai_cold_001', 'pai_cold_002|pai_cold_001', 'reroute_cold_dock', 'success', 'useful', 1, 935729, 1810000000000010009);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_cold_002', 4, 2, 'pai_cold_hard_001', 'pai_cold_002|pai_cold_hard_001', 'ignore_until_checkpoint', 'success', 'misleading', 3, 728209, 1810000000000010010);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_cold_002', 4, 3, 'pai_cold_004', 'pai_cold_002|pai_cold_004', 'inspect_door_seal', 'success', 'useful', 1, 485328, 1810000000000010011);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_drone_002', 5, 1, 'pai_drone_001', 'pai_drone_002|pai_drone_001', 'return_to_base', 'success', 'useful', 1, 923511, 1810000000000010012);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_drone_002', 5, 2, 'pai_drone_hard_001', 'pai_drone_002|pai_drone_hard_001', 'continue_mission', 'success', 'misleading', 3, 728503, 1810000000000010013);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('similar_robot_incident', 1, 'pai_drone_002', 5, 3, 'pai_drone_004', 'pai_drone_002|pai_drone_004', 'switch_vision_nav', 'success', 'useful', 1, 620268, 1810000000000010014);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_agv_slip_002', 1, 1, 'pai_agv_slip_001', 'pai_agv_slip_002|pai_agv_slip_001', 'reroute_zone', 'success', 'useful', 1, 1096750, 1810000000000010015);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_agv_slip_002', 1, 2, 'pai_agv_slip_hard_001', 'pai_agv_slip_002|pai_agv_slip_hard_001', 'continue_route', 'success', 'misleading', 3, 1093723, 1810000000000010016);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_agv_slip_002', 1, 3, 'pai_agv_slip_004', 'pai_agv_slip_002|pai_agv_slip_004', 'stop_and_inspect', 'success', 'useful', 1, 1090626, 1810000000000010017);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_lidar_002', 2, 1, 'pai_lidar_hard_001', 'pai_lidar_002|pai_lidar_hard_001', 'speed_up_clear_zone', 'success', 'misleading', 3, 1089453, 1810000000000010018);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_lidar_002', 2, 2, 'pai_lidar_001', 'pai_lidar_002|pai_lidar_001', 'safe_stop_clean_lens', 'success', 'useful', 1, 1089050, 1810000000000010019);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_lidar_002', 2, 3, 'pai_lidar_003', 'pai_lidar_002|pai_lidar_003', 'speed_up_clear_zone', 'success', 'misleading', 3, 1087475, 1810000000000010020);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_arm_002', 3, 1, 'pai_arm_001', 'pai_arm_002|pai_arm_001', 'pause_recalibrate', 'success', 'useful', 1, 1095665, 1810000000000010021);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_arm_002', 3, 2, 'pai_arm_hard_001', 'pai_arm_002|pai_arm_hard_001', 'increase_torque_limit', 'success', 'misleading', 3, 1093437, 1810000000000010022);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_arm_002', 3, 3, 'pai_arm_003', 'pai_arm_002|pai_arm_003', 'increase_torque_limit', 'success', 'misleading', 3, 1090817, 1810000000000010023);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_cold_002', 4, 1, 'pai_cold_001', 'pai_cold_002|pai_cold_001', 'reroute_cold_dock', 'success', 'useful', 1, 1097685, 1810000000000010024);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_cold_002', 4, 2, 'pai_cold_hard_001', 'pai_cold_002|pai_cold_hard_001', 'ignore_until_checkpoint', 'success', 'misleading', 3, 1091844, 1810000000000010025);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_cold_002', 4, 3, 'pai_cold_004', 'pai_cold_002|pai_cold_004', 'inspect_door_seal', 'success', 'useful', 1, 1088565, 1810000000000010026);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_drone_002', 5, 1, 'pai_drone_001', 'pai_drone_002|pai_drone_001', 'return_to_base', 'success', 'useful', 1, 1096682, 1810000000000010027);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_drone_002', 5, 2, 'pai_drone_004', 'pai_drone_002|pai_drone_004', 'switch_vision_nav', 'success', 'useful', 1, 1087184, 1810000000000010028);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('runbook_action_prior', 2, 'pai_drone_002', 5, 3, 'pai_drone_hard_001', 'pai_drone_002|pai_drone_hard_001', 'continue_mission', 'success', 'misleading', 3, 1087092, 1810000000000010029);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_agv_slip_002', 1, 1, 'pai_agv_slip_001', 'pai_agv_slip_002|pai_agv_slip_001', 'reroute_zone', 'success', 'useful', 1, 680221, 1810000000000010030);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_agv_slip_002', 1, 2, 'pai_agv_slip_hard_001', 'pai_agv_slip_002|pai_agv_slip_hard_001', 'continue_route', 'success', 'misleading', 3, 629477, 1810000000000010031);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_agv_slip_002', 1, 3, 'pai_agv_slip_004', 'pai_agv_slip_002|pai_agv_slip_004', 'stop_and_inspect', 'success', 'useful', 1, 467918, 1810000000000010032);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_lidar_002', 2, 1, 'pai_lidar_hard_001', 'pai_lidar_002|pai_lidar_hard_001', 'speed_up_clear_zone', 'success', 'misleading', 3, 560216, 1810000000000010033);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_lidar_002', 2, 2, 'pai_lidar_001', 'pai_lidar_002|pai_lidar_001', 'safe_stop_clean_lens', 'success', 'useful', 1, 559672, 1810000000000010034);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_lidar_002', 2, 3, 'pai_lidar_004', 'pai_lidar_002|pai_lidar_004', 'switch_sensor_mode', 'success', 'useful', 1, 290118, 1810000000000010035);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_arm_002', 3, 1, 'pai_arm_001', 'pai_arm_002|pai_arm_001', 'pause_recalibrate', 'success', 'useful', 1, 669746, 1810000000000010036);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_arm_002', 3, 2, 'pai_arm_hard_001', 'pai_arm_002|pai_arm_hard_001', 'increase_torque_limit', 'success', 'misleading', 3, 613725, 1810000000000010037);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_arm_002', 3, 3, 'pai_arm_004', 'pai_arm_002|pai_arm_004', 'reduce_speed', 'partial_success', 'useful', 1, 440655, 1810000000000010038);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_cold_002', 4, 1, 'pai_cold_001', 'pai_cold_002|pai_cold_001', 'reroute_cold_dock', 'success', 'useful', 1, 705224, 1810000000000010039);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_cold_002', 4, 2, 'pai_cold_hard_001', 'pai_cold_002|pai_cold_hard_001', 'ignore_until_checkpoint', 'success', 'misleading', 3, 600393, 1810000000000010040);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_cold_002', 4, 3, 'pai_cold_004', 'pai_cold_002|pai_cold_004', 'inspect_door_seal', 'success', 'useful', 1, 345898, 1810000000000010041);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_drone_002', 5, 1, 'pai_drone_001', 'pai_drone_002|pai_drone_001', 'return_to_base', 'success', 'useful', 1, 685705, 1810000000000010042);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_drone_002', 5, 2, 'pai_drone_hard_001', 'pai_drone_002|pai_drone_hard_001', 'continue_mission', 'success', 'misleading', 3, 524502, 1810000000000010043);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('reflection_only_memory', 3, 'pai_drone_002', 5, 3, 'pai_drone_004', 'pai_drone_002|pai_drone_004', 'switch_vision_nav', 'success', 'useful', 1, 370200, 1810000000000010044);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_agv_slip_002', 1, 1, 'pai_agv_slip_001', 'pai_agv_slip_002|pai_agv_slip_001', 'reroute_zone', 'success', 'useful', 1, 914131, 1810000000000010045);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_agv_slip_002', 1, 2, 'pai_agv_slip_hard_001', 'pai_agv_slip_002|pai_agv_slip_hard_001', 'continue_route', 'success', 'misleading', 3, 773229, 1810000000000010046);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_agv_slip_002', 1, 3, 'pai_agv_slip_004', 'pai_agv_slip_002|pai_agv_slip_004', 'stop_and_inspect', 'success', 'useful', 1, 664990, 1810000000000010047);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_lidar_002', 2, 1, 'pai_lidar_001', 'pai_lidar_002|pai_lidar_001', 'safe_stop_clean_lens', 'success', 'useful', 1, 864195, 1810000000000010048);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_lidar_002', 2, 2, 'pai_lidar_hard_001', 'pai_lidar_002|pai_lidar_hard_001', 'speed_up_clear_zone', 'success', 'misleading', 3, 701931, 1810000000000010049);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_lidar_002', 2, 3, 'pai_lidar_004', 'pai_lidar_002|pai_lidar_004', 'switch_sensor_mode', 'success', 'useful', 1, 552693, 1810000000000010050);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_arm_002', 3, 1, 'pai_arm_001', 'pai_arm_002|pai_arm_001', 'pause_recalibrate', 'success', 'useful', 1, 916855, 1810000000000010051);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_arm_002', 3, 2, 'pai_arm_004', 'pai_arm_002|pai_arm_004', 'reduce_speed', 'partial_success', 'useful', 1, 750738, 1810000000000010052);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_arm_002', 3, 3, 'pai_arm_hard_001', 'pai_arm_002|pai_arm_hard_001', 'increase_torque_limit', 'success', 'misleading', 3, 641124, 1810000000000010053);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_cold_002', 4, 1, 'pai_cold_001', 'pai_cold_002|pai_cold_001', 'reroute_cold_dock', 'success', 'useful', 1, 935729, 1810000000000010054);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_cold_002', 4, 2, 'pai_cold_hard_001', 'pai_cold_002|pai_cold_hard_001', 'ignore_until_checkpoint', 'success', 'misleading', 3, 728209, 1810000000000010055);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_cold_002', 4, 3, 'pai_cold_004', 'pai_cold_002|pai_cold_004', 'inspect_door_seal', 'success', 'useful', 1, 485328, 1810000000000010056);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_drone_002', 5, 1, 'pai_drone_001', 'pai_drone_002|pai_drone_001', 'return_to_base', 'success', 'useful', 1, 923511, 1810000000000010057);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_drone_002', 5, 2, 'pai_drone_hard_001', 'pai_drone_002|pai_drone_hard_001', 'continue_mission', 'success', 'misleading', 3, 728503, 1810000000000010058);

INSERT INTO physical_ai_retrieval_014 (variant, variant_code, query_id, query_seq, retrieval_rank, candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, quality_code, score_micros, timestamp) VALUES ('context_gated_physical_ai_action_outcome', 4, 'pai_drone_002', 5, 3, 'pai_drone_004', 'pai_drone_002|pai_drone_004', 'switch_vision_nav', 'success', 'useful', 1, 620268, 1810000000000010059);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_agv_slip_hard_001', 'pai_agv_slip_002|pai_agv_slip_hard_001', 'continue_route', 'success', 1000000, 120000, 120000, 723912, 'topology_mismatch,change_context_mismatch,query_marks_action_unsafe', 1810000000000020000);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_agv_slip_003', 'pai_agv_slip_002|pai_agv_slip_003', 'continue_route', 'success', 1000000, 120000, 120000, 308457, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch,query_marks_action_unsafe', 1810000000000020001);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_cold_002', 'pai_agv_slip_002|pai_cold_002', 'ignore_until_checkpoint', 'failure', -700000, -84000, 120000, 94696, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020002);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_cold_001', 'pai_agv_slip_002|pai_cold_001', 'reroute_cold_dock', 'success', 1000000, 120000, 120000, 92265, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020003);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_arm_004', 'pai_agv_slip_002|pai_arm_004', 'reduce_speed', 'partial_success', 450000, 54000, 120000, 95833, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020004);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_agv_slip_002', 1, 'pai_lidar_001', 'pai_agv_slip_002|pai_lidar_001', 'safe_stop_clean_lens', 'success', 1000000, 120000, 120000, 94667, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020005);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_lidar_hard_001', 'pai_lidar_002|pai_lidar_hard_001', 'speed_up_clear_zone', 'success', 1000000, 120000, 120000, 637038, 'topology_mismatch,change_context_mismatch,query_marks_action_unsafe', 1810000000000020006);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_lidar_004', 'pai_lidar_002|pai_lidar_004', 'switch_sensor_mode', 'success', 1000000, 450000, 450000, 792960, 'temporal_motif_mismatch', 1810000000000020007);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_lidar_003', 'pai_lidar_002|pai_lidar_003', 'speed_up_clear_zone', 'success', 1000000, 120000, 120000, 217369, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch,query_marks_action_unsafe', 1810000000000020008);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_arm_002', 'pai_lidar_002|pai_arm_002', 'increase_torque_limit', 'unsafe_failure', -1000000, -120000, 120000, 90000, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020009);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_agv_slip_002', 'pai_lidar_002|pai_agv_slip_002', 'continue_route', 'unsafe_failure', -1000000, -120000, 120000, 87667, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020010);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_arm_001', 'pai_lidar_002|pai_arm_001', 'pause_recalibrate', 'success', 1000000, 120000, 120000, 90000, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020011);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_lidar_002', 2, 'pai_arm_004', 'pai_lidar_002|pai_arm_004', 'reduce_speed', 'partial_success', 450000, 54000, 120000, 84167, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020012);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_arm_002', 3, 'pai_arm_hard_001', 'pai_arm_002|pai_arm_hard_001', 'increase_torque_limit', 'success', 1000000, 120000, 120000, 457893, 'topology_mismatch,change_context_mismatch,query_marks_action_unsafe', 1810000000000020013);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_arm_002', 3, 'pai_arm_003', 'pai_arm_002|pai_arm_003', 'increase_torque_limit', 'success', 1000000, 120000, 120000, 222581, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch,query_marks_action_unsafe', 1810000000000020014);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_arm_002', 3, 'pai_lidar_002', 'pai_arm_002|pai_lidar_002', 'speed_up_clear_zone', 'unsafe_failure', -1000000, -120000, 120000, 90000, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020015);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_arm_002', 3, 'pai_agv_slip_002', 'pai_arm_002|pai_agv_slip_002', 'continue_route', 'unsafe_failure', -1000000, -120000, 120000, 80667, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020016);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_arm_002', 3, 'pai_lidar_001', 'pai_arm_002|pai_lidar_001', 'safe_stop_clean_lens', 'success', 1000000, 120000, 120000, 84167, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020017);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_arm_002', 3, 'pai_agv_slip_001', 'pai_arm_002|pai_agv_slip_001', 'reroute_zone', 'success', 1000000, 120000, 120000, 78333, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020018);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_cold_hard_001', 'pai_cold_002|pai_cold_hard_001', 'ignore_until_checkpoint', 'success', 1000000, 120000, 120000, 621582, 'topology_mismatch,change_context_mismatch,query_marks_action_unsafe', 1810000000000020019);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_cold_004', 'pai_cold_002|pai_cold_004', 'inspect_door_seal', 'success', 1000000, 450000, 450000, 633690, 'topology_mismatch,temporal_motif_mismatch', 1810000000000020020);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_cold_003', 'pai_cold_002|pai_cold_003', 'ignore_until_checkpoint', 'success', 1000000, 120000, 120000, 300822, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch,query_marks_action_unsafe', 1810000000000020021);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_agv_slip_004', 'pai_cold_002|pai_agv_slip_004', 'stop_and_inspect', 'success', 1000000, 120000, 120000, 103958, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020022);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_agv_slip_001', 'pai_cold_002|pai_agv_slip_001', 'reroute_zone', 'success', 1000000, 120000, 120000, 100917, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020023);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_agv_slip_hard_001', 'pai_cold_002|pai_agv_slip_hard_001', 'continue_route', 'success', 1000000, 120000, 120000, 152214, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020024);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_cold_002', 4, 'pai_agv_slip_002', 'pai_cold_002|pai_agv_slip_002', 'continue_route', 'unsafe_failure', -1000000, -120000, 120000, 94696, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020025);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_drone_002', 5, 'pai_drone_hard_001', 'pai_drone_002|pai_drone_hard_001', 'continue_mission', 'success', 1000000, 120000, 120000, 714529, 'topology_mismatch,change_context_mismatch,query_marks_action_unsafe', 1810000000000020026);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_drone_002', 5, 'pai_drone_003', 'pai_drone_002|pai_drone_003', 'continue_mission', 'success', 1000000, 120000, 120000, 274893, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch,query_marks_action_unsafe', 1810000000000020027);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_drone_002', 5, 'pai_lidar_003', 'pai_drone_002|pai_lidar_003', 'speed_up_clear_zone', 'success', 1000000, 120000, 120000, 93205, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020028);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_drone_002', 5, 'pai_lidar_hard_001', 'pai_drone_002|pai_lidar_hard_001', 'speed_up_clear_zone', 'success', 1000000, 120000, 120000, 94667, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020029);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_drone_002', 5, 'pai_agv_slip_hard_001', 'pai_drone_002|pai_agv_slip_hard_001', 'continue_route', 'success', 1000000, 120000, 120000, 88667, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020030);

INSERT INTO physical_ai_suppressions_014 (query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, raw_value_micros, gated_value_micros, multiplier_micros, context_score_micros, reasons, timestamp) VALUES ('pai_drone_002', 5, 'pai_arm_hard_001', 'pai_drone_002|pai_arm_hard_001', 'increase_torque_limit', 'success', 1000000, 120000, 120000, 86667, 'topology_mismatch,change_context_mismatch,temporal_motif_mismatch', 1810000000000020031);
