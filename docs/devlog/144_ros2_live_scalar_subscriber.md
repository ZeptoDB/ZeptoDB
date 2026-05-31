# 144 — ROS 2 Live Scalar Subscriber

Date: 2026-05-31
Status: Complete

## Context

P9 1b follows the ROS 2 connector skeleton from devlog 143. The goal is to
turn the no-live-ROS bridge surface into a first live subscriber path while
keeping the default build usable on systems without ROS 2 installed.

The first live path is intentionally narrow: one `std_msgs` scalar message per
subscription, mapped through the existing scalar sample and table-aware ingest
contract. Array/profile messages such as `sensor_msgs/msg/JointState` stay in
the standard-message profile phase.

## Changes

- Extended `Ros2Consumer::start()` behind `ZEPTO_ROS2_AVAILABLE`:
  - creates a private `rclcpp::Context`, node, and single-threaded executor
  - subscribes to `std_msgs/msg/Float64`, `Float32`, `Int64`, `Int32`,
    `UInt64`, and `UInt32`
  - maps each message's `data` field through `Ros2ScalarSample` and the
    existing local/cluster ingest path
  - shuts the executor, context, and spin thread down in `stop()`
- Updated `ZEPTO_USE_ROS2` CMake probing to require both `rclcpp` and
  `std_msgs` before defining `ZEPTO_ROS2_AVAILABLE`.
- Added the ROS prefix `lib` directory as a public link directory for
  `zepto_ros2`, so RoboStack/conda `tracetools` dependencies such as
  `lttng-ust` resolve when downstream executables link the connector.
- Tightened config validation for the live scalar MVP:
  - only the supported `std_msgs` scalar message types are accepted
  - live scalar subscriptions require exactly one field mapping
  - the field path must be `data`
- Added pure helper coverage for supported message types, unsupported profile
  types, non-`data` fields, numeric scaling, NaN/invalid scale, and overflow.
- Added a live ROS 2 integration test that publishes `std_msgs/msg/Float64`
  into a real `rclcpp` graph and verifies ZeptoDB table-scoped ingest.
- Updated API/design docs to describe the current `std_msgs` scalar surface.

## Verification

```bash
MAMBA_ROOT_PREFIX="$HOME/.mamba" ~/.local/bin/micromamba create -y \
  -p "$HOME/ros2_jazzy" -c conda-forge -c robostack-jazzy \
  ros-jazzy-ros-base ros-jazzy-rclcpp ros-jazzy-std-msgs \
  ros-jazzy-ros2topic ros-jazzy-demo-nodes-cpp

MAMBA_ROOT_PREFIX="$HOME/.mamba" ~/.local/bin/micromamba run \
  -p "$HOME/ros2_jazzy" ros2 pkg list

MAMBA_ROOT_PREFIX="$HOME/.mamba" ~/.local/bin/micromamba run \
  -p "$HOME/ros2_jazzy" /usr/bin/cmake -S . -B build_ros2_fast -G Ninja \
  -DZEPTO_USE_ROS2=ON -DZEPTO_BUILD_PYTHON=OFF -DZEPTO_ENABLE_DUCKDB=OFF

MAMBA_ROOT_PREFIX="$HOME/.mamba" ~/.local/bin/micromamba run \
  -p "$HOME/ros2_jazzy" ninja -C build_ros2_fast -j8 zepto_ros2

MAMBA_ROOT_PREFIX="$HOME/.mamba" ~/.local/bin/micromamba run \
  -p "$HOME/ros2_jazzy" ninja -C build_ros2_fast -j8 zepto_tests

MAMBA_ROOT_PREFIX="$HOME/.mamba" ~/.local/bin/micromamba run \
  -p "$HOME/ros2_jazzy" ./build_ros2_fast/tests/zepto_tests \
  --gtest_filter='Ros2ConsumerTest.*'

cmake --build build --target zepto_tests -j8
./build/tests/zepto_tests --gtest_filter='Ros2ConsumerTest.*'
```

Results:

- ROS 2 Jazzy installed through RoboStack/conda at `/home/ec2-user/ros2_jazzy`
  on Amazon Linux 2023. `rclcpp`, `std_msgs`, `ros2topic`, and
  `demo_nodes_cpp` are visible to `ros2 pkg list`.
- ROS 2 CLI pub/sub smoke passed on `/zepto_smoke`:
  `std_msgs/msg/Float64` published `data: 42.5` and `ros2 topic echo --once`
  received it.
- ROS-enabled configure passed with `rclcpp/std_msgs: found`,
  `zepto_ros2: ROS 2 std_msgs scalar subscriber enabled`, `DuckDB: OFF`,
  and `Tests: ON`.
- `ninja -C build_ros2_fast -j8 zepto_ros2` passed.
- `ninja -C build_ros2_fast -j8 zepto_tests` passed.
- ROS-enabled `Ros2ConsumerTest.*`: 27 / 27 passed, including
  `LiveFloat64SubscriberIngestsIntoTable`.
- Default non-ROS build still passes `Ros2ConsumerTest.*`: 26 / 26.

The ROS verification build intentionally used `ZEPTO_ENABLE_DUCKDB=OFF` to keep
the connector smoke focused and avoid compiling the embedded DuckDB dependency.
The default project build remains covered separately.

Cross-architecture verification was not run. The new code is connector-layer
control flow and scalar conversion only; it does not add SIMD or
layout-sensitive storage structures.

## Follow-ups

- BACKLOG P9 1b — package/runtime hardening for ROS 2 installs outside the
  local RoboStack smoke path.
- BACKLOG P9 1c — rosbag2 import/replay.
- BACKLOG P9 1d — standard message profiles for IMU, JointState, Odometry, TF,
  and LaserScan metadata.
