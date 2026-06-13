# Logistics Physical AI Design

Logistics is a first-class Physical AI sector for ZeptoDB: AGVs, sorters,
RFID gates, cold-chain sensors, yard robots, drones, WMS/OMS events, and
digital twins all emit time-series state that needs low-latency replay and
feature extraction.

## Core Entities

| Entity | Table | Key columns |
|---|---|---|
| AGV pose | `agv_pose` | `agv_id`, `lat`, `lon`, `heading_deg`, `battery_pct` |
| Pallet state | `pallet_events` | `pallet_id`, `order_id`, `zone`, `state` |
| Sorter lane | `sorter_metrics` | `line_id`, `lane_id`, `throughput`, `jam_count` |
| RFID read | `rfid_reads` | `tag_id`, `gate_id`, `rssi`, `quality` |
| Cold-chain sensor | `cold_chain_events` | `shipment_id`, `temperature_c`, `humidity_pct` |

## Entity Timeline Recipe

Use symbol columns for entity IDs and query each entity as a time-ordered
timeline:

```sql
SELECT timestamp, state, zone
FROM pallet_events
WHERE pallet_id = 10042
ORDER BY timestamp ASC
```

Join event timelines with sensor state:

```sql
SELECT p.timestamp, p.state, p.zone, a.lat, a.lon
FROM pallet_events p
ASOF JOIN agv_pose a
ON p.agv_id = a.agv_id AND p.timestamp >= a.timestamp
WHERE p.pallet_id = 10042
```

## Spatial Workflows

AGV collision and geofence checks use the spatial SQL functions:

```sql
SELECT a.agv_id, b.agv_id, a.timestamp
FROM agv_pose a
JOIN agv_pose b ON a.timestamp = b.timestamp
WHERE a.agv_id != b.agv_id
  AND ST_Distance(a.lat, a.lon, b.lat, b.lon) < 5
```

## Market Position

ZeptoDB's logistics wedge is high-frequency physical operations data:

- AGV and AMR fleets need millisecond replay and spatial filters.
- Cold-chain lanes need audit-grade retention and table ACL isolation.
- Sorters and RFID gates need high-throughput ingest and anomaly queries.
- Digital twin teams need the same data model for simulation and production.

Primary buyers are warehouse automation teams, robotics platform teams,
cold-chain operators, and logistics AI teams building ETA, anomaly, and
optimization models.
