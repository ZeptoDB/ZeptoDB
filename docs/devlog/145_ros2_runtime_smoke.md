# 145 — ROS 2 Runtime Smoke Packaging

Date: 2026-05-31
Status: Complete

## Context

After the live scalar subscriber shipped in devlog 144, the next P9 item was to
make the ROS 2 install and smoke path repeatable. The local proof used
RoboStack Jazzy on Amazon Linux 2023, but the commands lived only in the
devlog.

## Changes

- Added `tools/run-ros2-smoke.sh`, a one-command smoke script that:
  - creates or reuses a RoboStack Jazzy micromamba environment
  - includes `lttng-ust` for RoboStack `tracetools` link dependencies
  - verifies core ROS 2 packages
  - runs a ROS CLI `std_msgs/msg/Float64` pub/sub smoke
  - configures a focused ZeptoDB ROS build with DuckDB disabled
  - builds `zepto_ros2` and `zepto_tests`
  - runs `Ros2ConsumerTest.*`
- Added `docs/operations/ROS2_SETUP.md` with install, manual build, smoke, and
  troubleshooting guidance.
- Added the ROS 2 setup guide to the MkDocs Operations nav.
- Updated BACKLOG so the ROS 2 roadmap's next open implementation item is
  rosbag2 import/replay.

## Verification

```bash
tools/run-ros2-smoke.sh
```

Results:

- ROS 2 Jazzy environment reused at `/home/ec2-user/ros2_jazzy`.
- ROS package check passed for `rclcpp`, `std_msgs`, `ros2topic`, and
  `demo_nodes_cpp`.
- ROS CLI pub/sub smoke passed with `data: 42.5`.
- Focused ROS-enabled ZeptoDB configure/build passed.
- `Ros2ConsumerTest.*`: 27 / 27 passed.

Cross-architecture verification was not run. This change adds a host smoke
script and documentation; it does not change SIMD or storage layout behavior.

## Follow-ups

- BACKLOG P9 1c — rosbag2 import/replay.
- BACKLOG P9 1d — standard message profiles for IMU, JointState, Odometry, TF,
  and LaserScan metadata.
