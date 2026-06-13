-- Fleet-level anomaly slice for IMU and battery-like telemetry.

SELECT robot_id,
       count(*) AS samples,
       avg(linear_acceleration_z) AS avg_accel_z,
       max(angular_velocity_z) AS max_yaw_rate
FROM ros_imu
WHERE timestamp BETWEEN 1710000000000000000 AND 1710000060000000000
GROUP BY robot_id
HAVING max_yaw_rate > 3;
