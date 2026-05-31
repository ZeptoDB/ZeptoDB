# 146: ROS 2 rosbag2 Import And Replay

Date: 2026-05-31
Status: Complete

## Context

The ROS 2 / Physical AI roadmap calls for deterministic robot-log replay after
the live scalar subscriber MVP. The first production-safe slice keeps the scope
aligned with the existing scalar `std_msgs` connector and avoids publishing
anything back into the ROS graph.

## Changes

- `include/zeptodb/feeds/ros2_consumer.h` adds `Ros2BagConfig`,
  `Ros2BagStats`, `Ros2Consumer::import_bag()`, and
  `Ros2Consumer::replay_bag()`.
- `src/feeds/ros2_consumer.cpp` reads rosbag2 messages through
  `rosbag2_cpp::Reader` when compiled with `ZEPTO_ROS2_BAG_AVAILABLE`,
  preserves rosbag send timestamps as source time, keeps receive timestamps,
  decodes the supported scalar `std_msgs` set, and ingests through the same
  table-aware ZeptoPipeline route as live subscriptions.
- `CMakeLists.txt` detects `rosbag2_cpp` and `rosbag2_storage` under
  `-DZEPTO_USE_ROS2=ON`, links their imported targets, and exposes
  `ZEPTO_ROS2_BAG_AVAILABLE` to tests.
- `tests/unit/test_ros2.cpp` adds pure bag config validation plus ROS-enabled
  generated sqlite3 rosbag import tests for configured-topic ingest and
  explicit unknown-topic skipping.
- `tools/run-ros2-smoke.sh` now installs/verifies the rosbag2 packages needed
  for bag import/replay coverage.
- Documentation now marks roadmap R3 complete and documents the C++ bag API
  and ROS 2 smoke package requirements.

## Verification

- `cmake --build build --target zepto_tests -j8`
- `./build/tests/zepto_tests --gtest_filter='Ros2ConsumerTest.*'`
  - 28/28 passed in the default non-ROS build.
- `tools/run-ros2-smoke.sh`
  - Reused `/home/ec2-user/ros2_jazzy`.
  - Verified `rclcpp`, `std_msgs`, `rosbag2_cpp`, `rosbag2_storage`,
    `rosbag2_storage_default_plugins`, `ros2topic`, and `demo_nodes_cpp`.
  - ROS CLI pub/sub smoke passed for `std_msgs/msg/Float64`.
  - CMake detected `rosbag2_cpp/storage`.
  - `Ros2ConsumerTest.*` passed 31/31, including generated rosbag2 import tests
    and the live rclcpp publish-to-ZeptoDB ingest test.

Known environment warnings remain unchanged for the RoboStack smoke path:
`tracetools` exports raw `lttng-ust` linker names and CMake reports
conda/system runtime library RPATH conflicts. The focused ROS connector build
and tests still pass.

## Follow-ups

- Implement standard message profiles for IMU, JointState, Odometry, TF, and
  LaserScan metadata.
- Add richer session metadata tables for bag hash, ROS distro, message
  definitions, replay config, and simulation provenance.
- Evaluate a future bulk-load path that can batch large historical bag imports
  through Arrow or Parquet instead of row-by-row scalar ingest.
