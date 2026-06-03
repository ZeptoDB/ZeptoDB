# ROS 2 Setup And Smoke Test

This guide describes the supported local smoke path for the ZeptoDB ROS 2
connector. It is intended for development hosts, CI workers, and robot-lab edge
nodes where the full ROS 2 apt distribution is not already installed.

The tested path on Amazon Linux 2023 is RoboStack Jazzy through micromamba. On
Ubuntu hosts, a sourced system ROS 2 Jazzy install can also work as long as
`rclcpp`, `std_msgs`, `sensor_msgs`, `nav_msgs`, `tf2_msgs`,
`geometry_msgs`, `rosbag2_cpp`, `rosbag2_storage`, `ros2topic`, and the
`tracetools` runtime dependencies are available to CMake and the dynamic
linker.

## Quick Smoke

```bash
tools/run-ros2-smoke.sh
```

The script performs four checks:

1. Creates or reuses `ROS2_PREFIX` (default: `$HOME/ros2_jazzy`) and installs
   any missing required RoboStack packages.
2. Verifies `rclcpp`, `std_msgs`, `sensor_msgs`, `nav_msgs`, `tf2_msgs`,
   `geometry_msgs`, `rosbag2_cpp`, `rosbag2_storage`,
   `rosbag2_storage_default_plugins`, `ros2topic`, and `demo_nodes_cpp`.
3. Publishes `std_msgs/msg/Float64 {data: 42.5}` on `/zepto_smoke` and confirms
   `ros2 topic echo --once` receives it.
4. Configures a focused ZeptoDB build with `-DZEPTO_USE_ROS2=ON`,
   `-DZEPTO_BUILD_PYTHON=OFF`, and `-DZEPTO_ENABLE_DUCKDB=OFF`, then runs
   `Ros2ConsumerTest.*`, including standard profile mapper tests, generated
   sqlite3 rosbag2 import tests, schema-aware typed ingest tests, and live
   `rclcpp` publishes into ZeptoDB scalar, standard-profile, and typed-profile
   tables.

Useful overrides:

```bash
MICROMAMBA=$HOME/.local/bin/micromamba \
ROS2_PREFIX=$HOME/ros2_jazzy \
BUILD_DIR=/tmp/zeptodb_ros2_smoke_build \
ROS_DOMAIN_ID=79 \
JOBS=8 \
tools/run-ros2-smoke.sh
```

## Manual Install

```bash
MAMBA_ROOT_PREFIX="$HOME/.mamba" ~/.local/bin/micromamba create -y \
  -p "$HOME/ros2_jazzy" \
  -c conda-forge -c robostack-jazzy \
  lttng-ust \
  ros-jazzy-ros-base \
  ros-jazzy-rclcpp \
  ros-jazzy-std-msgs \
  ros-jazzy-sensor-msgs \
  ros-jazzy-nav-msgs \
  ros-jazzy-tf2-msgs \
  ros-jazzy-geometry-msgs \
  ros-jazzy-rosbag2-cpp \
  ros-jazzy-rosbag2-storage-default-plugins \
  ros-jazzy-ros2topic \
  ros-jazzy-demo-nodes-cpp
```

`lttng-ust` is included explicitly because RoboStack `tracetools` may export
raw `-llttng-ust` linker flags. ZeptoDB's `zepto_ros2` CMake target propagates
the ROS prefix `lib` directory so downstream executables can resolve those
libraries during link.

## Manual Build

```bash
MAMBA_ROOT_PREFIX="$HOME/.mamba" ~/.local/bin/micromamba run \
  -p "$HOME/ros2_jazzy" \
  /usr/bin/cmake -S . -B /tmp/zeptodb_ros2_smoke_build -G Ninja \
  -DZEPTO_USE_ROS2=ON \
  -DZEPTO_BUILD_PYTHON=OFF \
  -DZEPTO_ENABLE_DUCKDB=OFF

MAMBA_ROOT_PREFIX="$HOME/.mamba" ~/.local/bin/micromamba run \
  -p "$HOME/ros2_jazzy" \
  ninja -C /tmp/zeptodb_ros2_smoke_build -j"$(nproc)" zepto_ros2 zepto_tests

MAMBA_ROOT_PREFIX="$HOME/.mamba" ~/.local/bin/micromamba run \
  -p "$HOME/ros2_jazzy" \
  /tmp/zeptodb_ros2_smoke_build/tests/zepto_tests \
  --gtest_filter='Ros2ConsumerTest.*'
```

The smoke build disables embedded DuckDB because the ROS connector path does
not depend on it, and avoiding that dependency keeps CI and edge validation
small.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `micromamba not found` | Install micromamba or set `MICROMAMBA=/path/to/micromamba`. |
| `rclcpp/std_msgs: not found` | Run CMake inside the ROS environment or source a system ROS 2 setup before configuring. |
| `ROS 2 standard messages: not fully found` | Install `ros-jazzy-sensor-msgs`, `ros-jazzy-nav-msgs`, `ros-jazzy-tf2-msgs`, and `ros-jazzy-geometry-msgs`, then reconfigure. |
| `rosbag2_cpp/storage: not found` | Install `ros-jazzy-rosbag2-cpp` and `ros-jazzy-rosbag2-storage-default-plugins`, then reconfigure. |
| `cannot find -llttng-ust` | Install `lttng-ust` into the ROS environment and reconfigure. |
| ROS pub/sub timeout | Use an isolated `ROS_DOMAIN_ID`, check local firewall rules, and ensure no stale daemon is using incompatible middleware settings. |
| CMake RPATH warning for conda/system libraries | Expected for focused RoboStack smoke builds. Use one consistent toolchain/runtime path for release packaging. |

## Production Notes

- The current bridge is read-only. It subscribes to scalar `std_msgs` topics
  and standard Physical AI profiles (`sensor_msgs/Imu`, `JointState`,
  `LaserScan`, `nav_msgs/Odometry`, `tf2_msgs/TFMessage`) without publishing
  back into the ROS graph. Use `Ros2IngestMode::TypedProfile` when standard
  messages should land in wide typed tables instead of scalarized
  `TickMessage` rows.
- Use explicit topic allowlists in production configs. Avoid wildcard discovery
  outside lab tooling.
- Keep robot control loops in their own ROS 2 executor. ZeptoDB should sit
  beside the graph as telemetry storage, replay, and analytics infrastructure.
- Run the smoke script whenever changing ROS 2 packages, middleware, compiler,
  or container base images.
- For robot-local, lab-edge, and k3s deployment recipes, see
  [`ROS2_EDGE_DEPLOYMENT.md`](ROS2_EDGE_DEPLOYMENT.md).
- For query examples, see [`examples/ros2/`](../../examples/ros2/).
