# Physical AI Use Cases

ZeptoDB is the telemetry and feature-store layer for robots, digital twins,
smart factories, drones, and logistics automation.

## Robotics Feature Store

Typed ROS 2 profiles store high-rate robot state as wide tables:

```sql
SELECT timestamp, orientation_w, angular_velocity_z, linear_acceleration_x
FROM ros_imu
WHERE symbol = 900 AND timestamp BETWEEN 1710000000000000000 AND 1710000005000000000
ORDER BY timestamp ASC
```

Use this for policy replay, anomaly detection, and low-latency feature export
to Python/Arrow.

## Digital Twin Replay

Isaac Sim and Gazebo sessions should write `/clock`-aligned telemetry into the
same schemas used by production robots. Store `session_id`, `robot_id`,
`topic`, and `frame_id` as symbol columns so simulation and physical runs can
be compared by query rather than by file naming convention.

## LiDAR + State Fusion

LaserScan typed profiles provide finite range summaries. Combine them with
robot pose or odometry through ASOF JOIN:

```sql
SELECT o.timestamp, o.pose_position_x, o.pose_position_y,
       l.ranges_min, l.ranges_mean
FROM ros_odometry o
ASOF JOIN ros_laserscan l
ON o.symbol = l.symbol AND o.timestamp >= l.timestamp
WHERE o.symbol = 900
```

## AGV Geofence And Proximity

Spatial SQL functions support AGV, drone, and yard-asset proximity checks:

```sql
SELECT agv_id, timestamp
FROM agv_pose
WHERE ST_Within(lat, lon, 37.7749, -122.4194, 50)
```

`haversine()` and `ST_Distance()` return distance in rounded meters.
`ST_Within()` returns true when the computed distance is within the supplied
radius in meters.

## Cold-Chain Audit Trail

Cold-chain telemetry should use append-only ingest plus operational controls:

- Use one table per regulated lane, shipment class, or tenant namespace.
- Restrict API keys with `allowed_tables` so writer keys cannot touch unrelated
  tables.
- Do not grant UPDATE/DELETE permissions to ingestion clients.
- Mirror snapshots to an S3 bucket with Object Lock / retention enabled.
- Store `shipment_id`, `pallet_id`, `sensor_id`, and `route_id` as symbol
  columns for audit queries.

Example table:

```sql
CREATE TABLE cold_chain_events (
  timestamp TIMESTAMP_NS,
  shipment_id SYMBOL,
  pallet_id SYMBOL,
  sensor_id SYMBOL,
  route_id SYMBOL,
  temperature_c FLOAT64,
  humidity_pct FLOAT64,
  door_open BOOL,
  quality INT32
)
```

This is an audit-grade recipe using existing ACL and retention controls, not a
new SQL `IMMUTABLE TABLE` syntax.
