# 153 — P9 Physical AI Closeout

**Date:** 2026-06-02
**Scope:** Remaining ROS 2 roadmap items, typed-profile cluster forwarding,
spatial SQL functions, Physical AI/logistics docs and examples.

## What Shipped

### TypedProfile Cluster RPC Forwarding

`Ros2IngestMode::TypedProfile` no longer fails closed when the
table-scoped partition owner is remote.

- `RpcClientBase` now exposes `ingest_typed_row()`.
- `TcpRpcClient` and `TcpRpcServer` support `TYPED_ROW_INGEST` /
  `TYPED_ROW_ACK`.
- `rpc_protocol.h` serializes `TypedRowMessage` with column names, types, and
  typed value slots.
- `ClusterNode` registers a typed-row ingest callback that writes into the
  local pipeline.
- `Ros2Consumer::on_typed_row()` routes remote typed rows through RPC and
  updates `route_remote`, `rows_ingested`, drop, and failure counters.

### Spatial SQL

The SQL value-expression path now supports:

- `haversine(lat1, lon1, lat2, lon2)`
- `ST_Distance(lat1, lon1, lat2, lon2)`
- `ST_Within(lat, lon, center_lat, center_lon, radius_m)`

The parser now preserves floating-point numeric literals in arithmetic
expressions, and WHERE supports function-valued left-hand predicates such as:

```sql
WHERE haversine(lat, lon, 37.7749, -122.4194) < 50
WHERE ST_Within(lat, lon, center_lat, center_lon, radius_m)
```

Distances use the haversine formula with Earth radius 6,371,008.8 meters and
return rounded meters through the current integer result path. Predicate
evaluation uses float-aware column reads so `FLOAT32`/`FLOAT64` coordinates do
not lose fractional precision.

### Docs And Examples

New artifacts:

- `docs/operations/ROS2_EDGE_DEPLOYMENT.md`
- `docs/usecases/physical_ai.md`
- `docs/design/logistics_physical_ai.md`
- `docs/bench/logistics_benchmark_suite.md`
- `examples/ros2/README.md`
- `examples/ros2/isaac_sim_typed_profiles.sql`
- `examples/ros2/robot_rl_replay.sql`
- `examples/ros2/lidar_asof_join.sql`
- `examples/ros2/fleet_anomaly_detection.sql`
- `examples/ros2/logistics_entity_timeline.sql`

Updated artifacts:

- `README.md`
- `docs/api/SQL_REFERENCE.md`
- `docs/operations/ROS2_SETUP.md`
- `docs/design/ros2_physical_ai_roadmap.md`
- `docs/BACKLOG.md`
- `docs/COMPLETED.md`

## Tests

Focused verification:

```bash
cmake --build build --target zepto_tests -j$(nproc)
./build/tests/zepto_tests --gtest_filter='Parser.Spatial*:SqlExecutorTest.Spatial*'
./build/tests/zepto_tests --gtest_filter='Ros2ConsumerTest.TypedProfileRemoteRouteForwardsTypedRow:Ros2ConsumerTest.TypedRowRpcProtocolRoundTripsValues:Ros2ConsumerTest.TypedRowIngestWritesSchemaAwareImuTable:Ros2ConsumerTest.TypedRowIngestRejectsSchemaMismatch'
```

All focused tests passed.

Full C++ suite verification:

```bash
./build/tests/zepto_tests
```

Result: 1414 tests ran, 1413 passed, and 1 live S3 upload test was skipped
because `ZEPTO_S3_TEST_BUCKET` was unset.

## P9 Closeout Status

The ROS 2 / Physical AI roadmap track is closed. At the time of devlog 153,
remaining P9 work was limited to OPC-UA production extensions and the external
factory competitor benchmark. Devlogs 154-155 later closed the OPC-UA items,
and devlog 159 closed the factory 10 KHz live competitor run against InfluxDB
and TimescaleDB.

Historical list from this checkpoint, now closed:

- OPC-UA browse + auto-discover CLI
- OPC-UA structured and array variants
- OPC-UA Historical Access
- OPC-UA Alarms & Conditions
- OPC-UA string values
- OPC-UA server mode
- Factory 10KHz competitor run vs InfluxDB/TimescaleDB
