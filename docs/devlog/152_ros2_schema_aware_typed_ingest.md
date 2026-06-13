# 152: ROS 2 Schema-Aware Typed Ingest

Date: 2026-06-02
Status: Complete

## Context

P9 1e closes the gap between scalarized ROS 2 profiles and the Physical AI
storage shape users actually need. R4 made IMU, JointState, Odometry, TF, and
LaserScan easy to ingest as configured scalar `TickMessage` rows. R5 adds wide
typed rows so standard ROS 2 messages can land directly in queryable tables
with timestamp, receive-time, robot/session/topic/frame metadata, quality, and
profile-specific numeric columns.

## Changes

- `include/zeptodb/core/pipeline.h` adds `TypedColumnValue`,
  `TypedRowMessage`, and `ZeptoPipeline::ingest_typed_row()` for schema-aware
  table-scoped wide-row ingest.
- `src/core/pipeline.cpp` validates typed rows against `SchemaRegistry`,
  materializes every declared table column in the target partition,
  default-fills omitted columns, appends values with type-specific storage
  widths, and marks the table as having data.
- `include/zeptodb/storage/schema_registry.h` adds
  `SchemaRegistry::get(uint16_t table_id)` so connector code can resolve a
  schema by stable table id.
- `include/zeptodb/feeds/ros2_consumer.h` adds
  `Ros2IngestMode::TypedProfile`, `typed_partition_symbol_id`,
  `validate_typed_profile_subscription()`, and
  `typed_profile_schema(profile)`.
- `src/feeds/ros2_consumer.cpp` maps IMU, JointState, Odometry, TFMessage, and
  LaserScan standard messages into typed rows for live ROS and rosbag2 paths.
  `set_pipeline()` validates required typed profile columns before ingest.
  Typed rows route locally and fail closed on remote owner routing because the
  cluster RPC surface still accepts only `TickMessage`.
- `src/sql/executor.cpp` now reads simple SELECT/WHERE column values through
  type-aware helpers for `SYMBOL`, `STRING`, `INT32`, `BOOL`, `FLOAT32`, and
  `FLOAT64`, avoiding invalid 64-bit casts over narrow typed columns.
- `tests/unit/test_ros2.cpp` adds typed profile config validation, typed schema
  coverage, direct core typed-row ingest/query coverage, schema mismatch
  rejection, and a ROS-enabled live IMU typed-profile publish test.
- `docs/design/ros2_physical_ai_roadmap.md`,
  `docs/api/CPP_REFERENCE.md`, `docs/operations/ROS2_SETUP.md`,
  `docs/BACKLOG.md`, and `docs/COMPLETED.md` now describe R5 completion and
  the remaining typed-row cluster forwarding follow-up.

## Verification

- Default local build:
  `cmake --build build --target zepto_ros2 zepto_tests -j$(nproc)`
- Typed focused tests:
  `./build/tests/zepto_tests --gtest_filter='Ros2ConsumerTest.*Typed*:Ros2ConsumerTest.Typed*:Ros2ConsumerTest.Validate*Typed*:Ros2ConsumerTest.*Schema*'`
  passed 5/5.
- Default focused tests:
  `./build/tests/zepto_tests --gtest_filter='Ros2ConsumerTest.*'`
  passed 41/41.
- ROS-enabled smoke:
  `tools/run-ros2-smoke.sh`
  reused `/home/ec2-user/ros2_jazzy`, verified scalar ROS packages, standard
  message packages, and rosbag2 packages, configured a fresh
  `-DZEPTO_USE_ROS2=ON` build with scalar + standard + typed profiles +
  rosbag2 enabled, built `zepto_ros2` and `zepto_tests`, and passed
  `Ros2ConsumerTest.*` 46/46 including a live IMU typed-profile publish into a
  ZeptoDB wide table.

## Follow-ups

- P9 1i: typed-profile cluster RPC forwarding. Remote owner routing currently
  fails closed for typed rows because the RPC interface is still
  `TickMessage`-only.
- P9 1f: Isaac Sim and `/clock`-aware digital twin ingest recipe.
- P9 1g: reference examples for robot replay, LiDAR ASOF JOIN, and fleet
  anomaly detection.
