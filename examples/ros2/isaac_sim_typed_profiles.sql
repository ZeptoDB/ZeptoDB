-- Isaac Sim / digital twin typed profile tables.
-- Keep these schemas aligned with Ros2Consumer::typed_profile_schema().

CREATE TABLE ros_imu (
  timestamp TIMESTAMP_NS,
  recv_ts TIMESTAMP_NS,
  robot_id SYMBOL,
  session_id SYMBOL,
  topic SYMBOL,
  frame_id SYMBOL,
  quality INT32,
  orientation_x FLOAT64,
  orientation_y FLOAT64,
  orientation_z FLOAT64,
  orientation_w FLOAT64,
  angular_velocity_x FLOAT64,
  angular_velocity_y FLOAT64,
  angular_velocity_z FLOAT64,
  linear_acceleration_x FLOAT64,
  linear_acceleration_y FLOAT64,
  linear_acceleration_z FLOAT64
);

CREATE TABLE ros_laserscan (
  timestamp TIMESTAMP_NS,
  recv_ts TIMESTAMP_NS,
  robot_id SYMBOL,
  session_id SYMBOL,
  topic SYMBOL,
  frame_id SYMBOL,
  quality INT32,
  angle_min FLOAT64,
  angle_max FLOAT64,
  angle_increment FLOAT64,
  time_increment FLOAT64,
  scan_time FLOAT64,
  range_min FLOAT64,
  range_max FLOAT64,
  ranges_count INT64,
  ranges_min FLOAT64,
  ranges_max FLOAT64,
  ranges_mean FLOAT64,
  intensities_count INT64,
  intensities_min FLOAT64,
  intensities_max FLOAT64,
  intensities_mean FLOAT64
);
