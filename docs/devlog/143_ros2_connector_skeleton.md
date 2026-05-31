# 143 — ROS 2 Connector Skeleton

Date: 2026-05-30
Status: Complete

## Context

This is the first implementation slice of the ROS 2 / Physical AI roadmap
from `docs/design/ros2_physical_ai_roadmap.md`.

The goal for P9 1a was deliberately narrow: establish the connector surface,
optional build plumbing, pure mapping helpers, stats, and tests without
requiring a live ROS 2 graph. The first `std_msgs` scalar subscriber code path
landed later in devlog 144; live ROS 2 smoke verification remains P9 1b.

## Changes

- Added `include/zeptodb/feeds/ros2_consumer.h`:
  - `Ros2Config`, `Ros2SubscriptionConfig`, `Ros2FieldMapping`
  - `Ros2ScalarSample` and `Ros2Stats`
  - `Ros2Consumer` lifecycle, routing, validation, mapping, and metrics API
- Added `src/feeds/ros2_consumer.cpp`:
  - config validation for empty subscriptions/topics, duplicate topics,
    invalid symbol IDs, invalid queue capacity, and non-finite scales
  - `Ros2Time` → nanoseconds conversion with invalid timestamp rejection
  - scalar sample → `TickMessage` mapping
  - table-aware local and cluster routing using the same pattern as
    Kafka/MQTT/OPC-UA
  - Prometheus/OpenMetrics formatter for ROS 2 connector counters and source lag
  - `start()` fail-closed behavior until P9 1b wires real `rclcpp`
    subscriptions
- Added optional CMake plumbing:
  - `ZEPTO_USE_ROS2`
  - `ZEPTO_HAS_ROS2`
  - `zepto_ros2` static library
- Added `tests/unit/test_ros2.cpp` with no-live-ROS coverage:
  - valid and invalid config cases
  - timestamp boundary cases
  - scalar mapping
  - no-pipeline and local-pipeline routing
  - unknown-topic and unknown-table failure paths
  - table-scoped ingest
  - Prometheus formatter
  - idempotent stop

## Verification

```bash
cd build
cmake ..
ninja -j$(nproc) zepto_tests
./tests/zepto_tests --gtest_filter='Ros2ConsumerTest.*'
./tests/zepto_tests --gtest_filter='Ros2ConsumerTest.*:KafkaConsumerTest.*:MqttConsumerTest.*:OpcUa*'
```

Results:

- `cmake ..` passed; `zepto_ros2` configured in no-rclcpp mode.
- `ninja -j$(nproc) zepto_tests` passed.
- `Ros2ConsumerTest.*`: 21 / 21 passed.
- Adjacent connector sweep: 125 / 125 passed, 1 disabled test reported by
  the existing suite.

Cross-architecture verification was not run for this slice. The code is
arch-neutral and does not add SIMD or layout-sensitive data structures.

## Follow-ups

- BACKLOG P9 1b — live ROS 2 scalar smoke verification and hardening.
- BACKLOG P9 1c — rosbag2 import/replay.
- BACKLOG P9 1d — standard message profiles.
