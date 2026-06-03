-- Extract a deterministic robot learning replay window.

SELECT timestamp,
       orientation_w,
       angular_velocity_z,
       linear_acceleration_x,
       linear_acceleration_y,
       linear_acceleration_z
FROM ros_imu
WHERE robot_id = 1001
  AND session_id = 2001
  AND timestamp BETWEEN 1710000000000000000 AND 1710000030000000000
ORDER BY timestamp ASC;
