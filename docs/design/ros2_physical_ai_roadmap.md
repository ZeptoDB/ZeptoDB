# ROS 2 and Physical AI Roadmap

**Status:** Roadmap with implementation in progress
**Last updated:** 2026-05-31
**Related docs:** `physical_ai_market.md`, `layer2_ingestion_network.md`,
`layer1_storage_memory.md`, `logging_observability.md`,
`opcua_connector.md`, `license_system.md`

---

## 1. Goal

Make ZeptoDB a first-class data layer for ROS 2 and Physical AI systems:
robotics, autonomous vehicles, digital twins, smart factories, drones, and
multi-robot fleets.

ZeptoDB does **not** replace DDS, ROS 2 executors, or real-time robot control.
It sits beside the ROS 2 graph as a high-throughput time-series engine for:

- live telemetry ingest from topics
- deterministic replay from rosbag2 and simulation clocks
- sensor-fusion queries with ASOF JOIN and Window JOIN
- online feature calculation for policy inference and robot learning
- zero-copy Python analytics on recent and historical data
- operational observability for robot and factory fleets

The product target is simple: **ROS 2 streams in, ZeptoDB makes the time axis
queryable immediately.**

---

## 2. Design Principles

1. **Read-only by default.** The initial bridge subscribes to telemetry topics
   and never publishes actuator commands. Command-topic publishing, if added
   later, must be a separate audited capability.
2. **Time semantics first.** Every row preserves source time, receive time, and
   session context. Sensor fusion is only trustworthy if clock provenance is
   explicit.
3. **Schema before blobs.** Hot-path tables should contain typed numeric and
   symbolic columns. Large camera, LiDAR, and point-cloud payloads should use
   metadata tables plus optional external payload references until a dedicated
   binary-object path exists.
4. **Bounded backpressure.** The bridge must expose queue depth, drops, decode
   errors, and ingest lag. Best-effort ROS topics may drop by policy; reliable
   streams need explicit blocking or spill behavior.
5. **Connector-layer isolation.** ROS 2 support follows the same pattern as
   Kafka, MQTT, and OPC-UA: protocol code lives in the feeds/connector layer;
   the engine sees table-aware ingest records.
6. **Simulation equals production data.** Isaac Sim, Gazebo, and robot logs
   should land in the same schema shape as physical robots, with a session flag
   distinguishing simulated, replayed, and live data.

---

## 3. Architecture

```
ROS 2 DDS graph
  topics: /joint_states, /imu, /odom, /tf, /scan, /points, /clock
        |
        v
Ros2Consumer / zepto_ros2_bridge
  rclcpp subscriptions
  rosbag2 replay reader
  message profile mapper
  bounded queue + stats
        |
        v
ZeptoPipeline ingest
  table_id + symbol_id routing
  WAL durability
  in-memory column store
  HDB / Parquet / S3 cold tier
        |
        v
SQL + Python + Arrow
  ASOF JOIN
  Window JOIN
  feature extraction
  replay and debugging
```

### 3.1 Proposed code placement

| Component | Path | Notes |
|---|---|---|
| ROS 2 connector API | `include/zeptodb/feeds/ros2_consumer.h` | Optional dependency, parallel to `KafkaConsumer`, `MqttConsumer`, `OpcUaConsumer` |
| ROS 2 connector implementation | `src/feeds/ros2_consumer.cpp` | `rclcpp` subscription, message mapping, stats |
| Bridge executable | `src/tools/zepto_ros2_bridge.cpp` or `src/feeds/` binary | Runs outside the robot control process by default |
| Tests | `tests/unit/test_ros2.cpp` | Mapping and config tests must pass without a live ROS graph |
| Examples | `examples/ros2/` | Launch files, topic configs, rosbag2 replay recipes |

### 3.2 Build flag

ROS 2 support should be optional, matching existing connector practice:

```bash
cmake -DZEPTO_USE_ROS2=ON ..
```

Default builds stay dependency-light. When the flag is off, public config and
pure mapping helpers may still compile, but `start()` should fail closed with a
clear reason.

---

## 4. Time Model

Every ROS 2 ingested row should carry enough timing metadata to distinguish
sensor time, transport latency, simulation time, and replay time.

| Field | Meaning | Source |
|---|---|---|
| `source_ts_ns` | Time assigned by the producer | `std_msgs/Header.stamp` when available |
| `recv_ts_ns` | Time observed by the bridge | ZeptoDB / bridge monotonic or system clock |
| `sim_ts_ns` | Simulation clock time | `/clock`, when `use_sim_time` is active |
| `replay_ts_ns` | Wall-clock time during replay | rosbag2 replay/import path |
| `session_id` | Live run, replay, simulation, mission, or experiment | Bridge config |
| `clock_domain` | `system`, `steady`, `sim`, `ptp`, or `unknown` | Bridge config and ROS time mode |

Policy:

- If a message has `Header.stamp`, it becomes `source_ts_ns`.
- If no source stamp exists, `recv_ts_ns` is used and the row quality marks
  `missing_source_time`.
- Replay must preserve original source timestamps and may add replay wall time.
- Simulation must store `/clock` time separately from wall time.
- Multi-robot deployments should prefer PTP-synchronized system clocks where
  available, but ZeptoDB must keep enough metadata to diagnose clock skew.

---

## 5. Message Profiles

The roadmap should start with high-value standard message profiles before
supporting arbitrary custom messages.

| ROS 2 message | Initial table strategy | Query value |
|---|---|---|
| `sensor_msgs/msg/Imu` | Flatten orientation, angular velocity, linear acceleration | stabilization, anomaly detection, sensor fusion |
| `sensor_msgs/msg/JointState` | One row per joint per sample | robot learning, torque/current drift, policy features |
| `nav_msgs/msg/Odometry` | Flatten pose and twist; keep frame symbols | localization, fleet tracking |
| `tf2_msgs/msg/TFMessage` | One row per transform | frame debugging, replay, transform drift |
| `sensor_msgs/msg/LaserScan` | Metadata + summary columns first; optional range expansion | LiDAR ASOF JOIN, obstacle timeline |
| `sensor_msgs/msg/PointCloud2` | Metadata + payload reference first | point-cloud session catalog, later binary payload path |
| `diagnostic_msgs/msg/DiagnosticArray` | Status rows keyed by hardware/component | robot health and fleet ops |

Large binary payloads are intentionally not the MVP hot path. A practical
first cut stores metadata, summaries, and a payload reference ID. Full payload
storage can then target Parquet/S3, Arrow IPC, or a future binary-object layer
without forcing camera and point-cloud blobs through `TickMessage`.

---

## 6. Ingest Modes

### 6.1 Scalar MVP

The first live implementation maps one `std_msgs` scalar message per
subscription to the existing tick ingest contract. The supported live message
types are `std_msgs/msg/Float64`, `Float32`, `Int64`, `Int32`, `UInt64`, and
`UInt32`; each uses the message's `data` field.

```yaml
subscriptions:
  - topic: /robot/joint_effort
    message_type: std_msgs/msg/Float64
    mode: scalar_fields
    table: robot_joint_scalar
    fields:
      - name: data
        symbol_id: 1
        value_scale: 1000
```

This is enough to prove `rclcpp subscriber -> ZeptoPipeline -> SQL/Python`
without requiring the full generic table mapper. Rich array/profile messages
such as `sensor_msgs/msg/JointState` remain part of the standard-message
profile phase.

### 6.2 Schema-aware bridge

The production path should write typed rows into user-visible tables, using the
same table-scoped partitioning and routing already used by HTTP, Python,
Kafka, MQTT, and OPC-UA.

Example table families:

```sql
CREATE TABLE ros2_joint_state (
  timestamp_ns INT64,
  robot_id SYMBOL,
  joint_id SYMBOL,
  position DOUBLE,
  velocity DOUBLE,
  effort DOUBLE
);

CREATE TABLE ros2_imu (
  timestamp_ns INT64,
  robot_id SYMBOL,
  frame_id SYMBOL,
  orientation_x DOUBLE,
  orientation_y DOUBLE,
  orientation_z DOUBLE,
  orientation_w DOUBLE,
  angular_velocity_x DOUBLE,
  angular_velocity_y DOUBLE,
  angular_velocity_z DOUBLE,
  linear_acceleration_x DOUBLE,
  linear_acceleration_y DOUBLE,
  linear_acceleration_z DOUBLE
);
```

The exact DDL depends on the current SQL type surface when implementation
starts. If the generic typed ingest path is not ready, the bridge should keep
the scalar MVP small instead of inventing a parallel storage contract.

### 6.3 rosbag2 import and replay

rosbag2 support is a separate mode, not just a live subscriber:

- **Import:** Convert a bag into ZeptoDB tables as fast as possible while
  preserving original timestamps.
- **Replay:** Feed records into ZeptoDB according to source time, wall time,
  or a configured speed multiplier.
- **Deterministic replay:** Same bag + same config must produce identical
  table rows and ordering.
- **Session metadata:** Bag path, hash, ROS distro, message definitions, and
  replay config should be queryable.

The first implemented slice (devlog 146) keeps the scope intentionally aligned
with the scalar MVP:

- `Ros2Consumer::import_bag()` imports configured scalar topics as fast as
  possible through the existing table-aware ZeptoPipeline ingest path.
- `Ros2Consumer::replay_bag()` reads the same scalar bag data and optionally
  sleeps according to preserved source-time deltas divided by
  `Ros2BagConfig::replay_speed`.
- Supported bag message types match the live scalar set:
  `std_msgs/msg/{Float64,Float32,Int64,Int32,UInt64,UInt32}`.
- Bag read order is deterministic by received timestamp; rows preserve
  rosbag send timestamps as `source_ts_ns` and receive timestamps as
  `recv_ts_ns`.
- Topic filters are explicit. Empty bag filters read the configured
  subscriptions; explicit unknown bag topics can be skipped or made fatal with
  `fail_on_unknown_topic`.

Richer session metadata tables, bag hashing, and arbitrary message definition
cataloging remain part of the standard-profile and schema-aware phases.

---

## 7. Query Recipes

### 7.1 Joint feature window

```sql
SELECT joint_id,
       EMA(effort, 10) OVER (PARTITION BY robot_id, joint_id) AS effort_ema,
       DELTA(position) OVER (PARTITION BY robot_id, joint_id) AS velocity_est
FROM ros2_joint_state
WHERE robot_id = 'arm-01'
ORDER BY timestamp_ns DESC
LIMIT 1000;
```

### 7.2 IMU and odometry alignment

```sql
SELECT imu.timestamp_ns,
       imu.robot_id,
       imu.angular_velocity_z,
       odom.twist_linear_x
FROM ros2_imu imu
ASOF JOIN ros2_odometry odom
ON imu.robot_id = odom.robot_id
AND imu.timestamp_ns >= odom.timestamp_ns;
```

### 7.3 Sensor lag diagnosis

```sql
SELECT robot_id,
       frame_id,
       AVG(recv_ts_ns - source_ts_ns) AS avg_lag_ns,
       MAX(recv_ts_ns - source_ts_ns) AS max_lag_ns
FROM ros2_topic_samples
GROUP BY robot_id, frame_id;
```

---

## 8. Observability

The connector should expose Prometheus metrics through the existing metrics
provider extension point.

| Metric | Type | Meaning |
|---|---|---|
| `zepto_ros2_messages_consumed_total` | counter | Messages received from ROS 2 subscriptions |
| `zepto_ros2_rows_ingested_total` | counter | Rows written to ZeptoDB |
| `zepto_ros2_decode_errors_total` | counter | Message mapping failures |
| `zepto_ros2_messages_dropped_total` | counter | Drops from queue/backpressure policy |
| `zepto_ros2_ingest_failures_total` | counter | Pipeline ingest failures |
| `zepto_ros2_route_local_total` | counter | Rows routed to local pipeline |
| `zepto_ros2_route_remote_total` | counter | Rows forwarded through cluster routing |
| `zepto_ros2_source_lag_ns` | gauge or histogram | `recv_ts_ns - source_ts_ns` |
| `zepto_ros2_queue_depth` | gauge | Connector bounded queue occupancy |

Logs must not include full message payloads by default. Topic names, message
types, table names, counters, and error summaries are enough for production
debugging without leaking sensitive robot or factory data.

---

## 9. Safety and Operational Rules

- The default binary is a bridge process, not a composable node loaded into the
  same executor as the robot control loop.
- Actuator command topics are out of scope for the MVP.
- Any future command publishing must require explicit config, RBAC, audit
  logging, and command intent records.
- Dropping data must be visible through metrics and logs. Silent loss is not
  acceptable for robot debugging.
- Replay mode must never accidentally publish back into a live robot graph.
- Config should support topic allowlists. Wildcard subscription is useful for
  discovery, but production configs should be explicit.

---

## 10. Roadmap

| Phase | Status | Milestone | Exit criteria |
|---|---|---|---|
| R0 | Done | Roadmap and product contract | This document exists; BACKLOG tracks implementation slices |
| R1 | Done | ROS 2 connector skeleton | Optional build flag, config validation, stats, no-live-ROS unit tests (devlog 143) |
| R2 | Done | Live scalar subscriber MVP | `std_msgs` scalar subscriptions verified with RoboStack Jazzy and a live `rclcpp` publish -> ZeptoDB table ingest test (devlog 144) |
| R3 | Done | rosbag2 import/replay | Deterministic scalar import and replay with preserved source timestamps (devlog 146) |
| R4 | Open | Standard message profiles | IMU, JointState, Odometry, TF, LaserScan metadata profiles |
| R5 | Open | Schema-aware typed ingest | Wide typed tables for standard messages, no parallel storage contract |
| R6 | Open | Isaac Sim / digital twin recipe | `/clock` aware simulation ingest and replay example |
| R7 | Open | Reference examples | Robot RL replay, LiDAR ASOF JOIN, fleet anomaly detection |
| R8 | Open | Edge deployment guide | Docker Compose, k3s/systemd, bounded resources, metrics dashboard |

---

## 11. Non-goals for the MVP

- Replacing DDS or ROS 2 communication.
- Running ZeptoDB inside a hard real-time control loop.
- Publishing actuator commands.
- Storing raw camera frames or full point clouds in the hot in-memory path by
  default.
- Solving TF trees inside ZeptoDB. The bridge records transform data; ROS 2
  libraries remain the source of truth for transform math.
- Supporting every custom message without a schema mapping contract.

---

## 12. Open Questions

1. Should the first production bridge target scalar-field extraction or typed
   standard message profiles first?
2. Which deployment mode is the first customer path: robot-local edge box,
   central lab server, or cloud replay cluster?
3. Do we treat `frame_id`, `robot_id`, and topic names as `SYMBOL` columns
   everywhere, or do we wait for broader variable-length string support?
4. For scalar MVP import, rosbag2 writes directly through ZeptoDB ingest APIs.
   A later bulk-load path may still build Arrow/Parquet batches for large
   historical backfills.
5. What is the minimum acceptable clock-skew reporting for multi-robot
   deployments?
