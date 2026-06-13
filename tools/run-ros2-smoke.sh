#!/usr/bin/env bash
# ============================================================================
# tools/run-ros2-smoke.sh
# ----------------------------------------------------------------------------
# Installs or reuses a RoboStack ROS 2 Jazzy environment, verifies ROS 2
# pub/sub, then builds and runs the ZeptoDB ROS 2 connector smoke tests.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MICROMAMBA="${MICROMAMBA:-$HOME/.local/bin/micromamba}"
ROS2_PREFIX="${ROS2_PREFIX:-$HOME/ros2_jazzy}"
MAMBA_ROOT_PREFIX="${MAMBA_ROOT_PREFIX:-$HOME/.mamba}"
BUILD_DIR="${BUILD_DIR:-/tmp/zeptodb_ros2_smoke_build}"
ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-79}"
JOBS="${JOBS:-$(nproc)}"

export MAMBA_ROOT_PREFIX

ROS2_CONDA_PACKAGES=(
    lttng-ust
    ros-jazzy-ros-base
    ros-jazzy-rclcpp
    ros-jazzy-std-msgs
    ros-jazzy-sensor-msgs
    ros-jazzy-nav-msgs
    ros-jazzy-tf2-msgs
    ros-jazzy-geometry-msgs
    ros-jazzy-rosbag2-cpp
    ros-jazzy-rosbag2-storage-default-plugins
    ros-jazzy-ros2topic
    ros-jazzy-demo-nodes-cpp
)

ROS2_REQUIRED_PACKAGES=(
    rclcpp
    std_msgs
    sensor_msgs
    nav_msgs
    tf2_msgs
    geometry_msgs
    rosbag2_cpp
    rosbag2_storage
    rosbag2_storage_default_plugins
    ros2topic
    demo_nodes_cpp
)

info() { printf '[INFO] %s\n' "$*"; }
die()  { printf '[ERR] %s\n' "$*" >&2; exit 1; }

run_ros2() {
    "$MICROMAMBA" run -p "$ROS2_PREFIX" "$@"
}

ensure_micromamba() {
    if [[ ! -x "$MICROMAMBA" ]]; then
        die "micromamba not found at $MICROMAMBA. Set MICROMAMBA=/path/to/micromamba."
    fi
}

ensure_ros2_env() {
    if [[ -x "$ROS2_PREFIX/bin/ros2" ]]; then
        info "Reusing ROS 2 environment: $ROS2_PREFIX"
        return
    fi

    info "Creating ROS 2 Jazzy environment: $ROS2_PREFIX"
    "$MICROMAMBA" create -y -p "$ROS2_PREFIX" \
        -c conda-forge -c robostack-jazzy \
        "${ROS2_CONDA_PACKAGES[@]}"
}

ensure_ros2_packages() {
    local required
    printf -v required '%q ' "${ROS2_REQUIRED_PACKAGES[@]}"

    local missing
    missing="$(run_ros2 bash -lc "
        for pkg in $required; do
            ros2 pkg prefix \"\$pkg\" >/dev/null 2>&1 || echo \"\$pkg\"
        done
    ")"

    if [[ -z "$missing" ]]; then
        return
    fi

    info "Installing missing ROS 2 packages: ${missing//$'\n'/ }"
    "$MICROMAMBA" install -y -p "$ROS2_PREFIX" \
        -c conda-forge -c robostack-jazzy \
        "${ROS2_CONDA_PACKAGES[@]}"
}

verify_ros_packages() {
    info "Verifying ROS 2 packages"
    run_ros2 bash -lc "
        ros2 --help >/dev/null
        ros2 pkg list | rg '^(rclcpp|std_msgs|sensor_msgs|nav_msgs|tf2_msgs|geometry_msgs|rosbag2_cpp|rosbag2_storage|rosbag2_storage_default_plugins|ros2topic|demo_nodes_cpp)$'
    "
}

verify_ros_pubsub() {
    info "Running ROS 2 CLI pub/sub smoke"
    local tmpdir
    tmpdir="$(mktemp -d)"

    if ! run_ros2 bash -lc "
        set -euo pipefail
        export ROS_DOMAIN_ID=$ROS_DOMAIN_ID
        timeout 12 ros2 topic echo --once /zepto_smoke std_msgs/msg/Float64 > '$tmpdir/echo.log' &
        echo_pid=\$!
        sleep 2
        timeout 8 ros2 topic pub --once /zepto_smoke std_msgs/msg/Float64 '{data: 42.5}' > '$tmpdir/pub.log'
        wait \"\$echo_pid\"
        grep -q 'data: 42.5' '$tmpdir/echo.log'
    "; then
        rm -rf "$tmpdir"
        return 1
    fi

    rm -rf "$tmpdir"
}

build_and_test_zeptodb() {
    info "Configuring ZeptoDB ROS 2 smoke build: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
    run_ros2 /usr/bin/cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja \
        -DZEPTO_USE_ROS2=ON \
        -DZEPTO_BUILD_PYTHON=OFF \
        -DZEPTO_ENABLE_DUCKDB=OFF

    info "Building zepto_ros2 and zepto_tests"
    run_ros2 ninja -C "$BUILD_DIR" -j"$JOBS" zepto_ros2 zepto_tests

    info "Running Ros2Consumer tests"
    run_ros2 "$BUILD_DIR/tests/zepto_tests" --gtest_filter='Ros2ConsumerTest.*'
}

main() {
    ensure_micromamba
    ensure_ros2_env
    ensure_ros2_packages
    verify_ros_packages
    verify_ros_pubsub
    build_and_test_zeptodb
    info "ROS 2 smoke passed"
}

main "$@"
