# ROS 2 Physical AI Examples

These examples show the intended query shape for ROS 2 typed profiles,
simulation replay, and robot/factory edge operations.

| File | Purpose |
|---|---|
| `isaac_sim_typed_profiles.sql` | Create typed tables for Isaac Sim / digital twin replay |
| `robot_rl_replay.sql` | Extract replay windows for policy learning |
| `lidar_asof_join.sql` | Join Odometry and LaserScan timelines |
| `fleet_anomaly_detection.sql` | Detect fleet-level IMU and battery anomalies |
| `logistics_entity_timeline.sql` | Query pallet, AGV, and RFID entity timelines |

Run these after creating and ingesting the matching tables through the ROS 2
bridge or rosbag2 import path.

For an end-to-end Agent Memory example that seeds realistic Physical AI rows,
retrieves operational memory, and records context trace/replay telemetry, see
`examples/agent_memory/physical_ai_agent_demo.py`.
