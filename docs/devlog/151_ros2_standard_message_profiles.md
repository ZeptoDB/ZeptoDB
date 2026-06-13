# 151: ROS 2 Standard Message Profiles

Date: 2026-06-02
Status: Complete

## Context

P9 needs first-class ROS 2 standard message support for Physical AI workloads.
The existing ROS 2 bridge already handled live and rosbag2 scalar
`std_msgs` topics, but robotics demos need IMU, JointState, Odometry, TF, and
LaserScan data without requiring users to hand-publish every signal as a
scalar topic.

## Changes

- `include/zeptodb/feeds/ros2_consumer.h` adds
  `Ros2IngestMode::StandardProfile`, `Ros2StandardProfile`, ROS-type-free
  sample structs for IMU, JointState, Odometry, TF, and LaserScan, plus pure
  profile-to-scalar mapper helpers.
- `src/feeds/ros2_consumer.cpp` validates standard profile message types and
  field paths, maps supported standard messages into `Ros2ScalarSample` rows,
  wires live `rclcpp` subscriptions when `sensor_msgs`, `nav_msgs`, and
  `tf2_msgs` are available, and decodes the same profiles from rosbag2.
- JointState and TF profiles expand array-like data with `symbol_id + index`;
  callers must reserve non-overlapping symbol ranges per configured field.
- LaserScan emits configured metadata plus count/min/max/mean summaries over
  finite range and intensity values. Raw range expansion stays out of the R4
  hot path.
- `CMakeLists.txt` detects `sensor_msgs`, `nav_msgs`, `tf2_msgs`, and
  `geometry_msgs` under `-DZEPTO_USE_ROS2=ON` and enables
  `ZEPTO_ROS2_STANDARD_AVAILABLE` only when all standard packages are present.
- `tools/run-ros2-smoke.sh` and `docs/operations/ROS2_SETUP.md` now install,
  verify, and document the standard ROS 2 message package prerequisites.
- `tests/unit/test_ros2.cpp` adds profile validation, pure mapper coverage for
  supported IMU, JointState, Odometry, TF, and LaserScan field paths, and a
  ROS-enabled live IMU publish-to-ZeptoDB table ingest test.
- `docs/design/ros2_physical_ai_roadmap.md`,
  `docs/api/CPP_REFERENCE.md`, `docs/BACKLOG.md`, and
  `docs/COMPLETED.md` now reflect R4 completion and the remaining typed-ingest
  follow-up.

## Verification

- Default local build:
  `cmake --build build --target zepto_ros2 zepto_tests -j$(nproc)`
- Default focused tests:
  `./build/tests/zepto_tests --gtest_filter='Ros2ConsumerTest.*'`
  passed 36/36.
- ROS-enabled smoke:
  `tools/run-ros2-smoke.sh`
  reused `/home/ec2-user/ros2_jazzy`, verified `sensor_msgs`, `nav_msgs`,
  `tf2_msgs`, `geometry_msgs`, `rclcpp`, `std_msgs`, and rosbag2 packages,
  configured `zepto_ros2` with scalar + standard profiles + rosbag2 enabled,
  built `zepto_ros2` and `zepto_tests`, and passed `Ros2ConsumerTest.*` 40/40
  including a live IMU standard-profile publish into ZeptoDB.

## Follow-ups

- P9 1e: schema-aware typed ingest for wide ROS 2 tables instead of forcing
  every standard profile through scalar `TickMessage` rows.
- P9 1f: Isaac Sim and `/clock`-aware digital twin ingest recipe.
- P9 1g: reference examples for robot replay, LiDAR ASOF JOIN, and fleet
  anomaly detection.
