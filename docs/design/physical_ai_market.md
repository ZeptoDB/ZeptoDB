# ZeptoDB x Physical AI — Business Opportunity Analysis
# 2026-03-22

---

## 1. Market Overview

Physical AI (AI systems that interact with the physical world) is the fastest-growing technology
sector of 2025~2030. Its core characteristics are structurally identical to HFT:
**high-frequency time-series data + real-time decision making + μs latency**.

### Target Market Size

| Sector | 2025 Market | 2030 Forecast | CAGR |
|---|---|---|---|
| Autonomous driving data infrastructure | $8B | $35B | ~34% |
| Industrial IoT / Smart factory | $110B | $280B | ~20% |
| Robot software infrastructure | $12B | $60B | ~38% |
| Drone control / swarm | $3B | $18B | ~43% |

---

## 2. Business Cases by Sector

---

### A. Autonomous Vehicles

**Customers:** Waymo, Cruise, Hyundai Mobis, NVIDIA DRIVE platform partners, Chinese OEMs

**Data characteristics:**
```
LiDAR:    10Hz × 100,000 points/scan = 1M points/sec
Camera:   30fps × 8 units = 240 frames/sec
IMU/GPS:  1,000Hz
Radar:    100Hz
CAN bus:  1,000+ channels × 100Hz
→ Single vehicle: millions of timestamped datapoints per second
```

**Current pain points:**
- Collected in ROS bag files → offline analysis only
- Redis used for real-time sensor fusion → unstable latency
- Python-based analytics pipeline is separate from C++ real-time loop

**ZeptoDB solution:**
```sql
-- Camera-LiDAR time synchronization (ASOF JOIN)
SELECT c.frame_id, c.timestamp, l.scan_data
FROM camera_frames c
ASOF JOIN lidar_scans l
ON c.sensor_id = l.sensor_id AND c.timestamp >= l.timestamp

-- ±50ms sensor integration (Window JOIN)
SELECT imu.accel_x, wj_avg(gps.latitude) AS smoothed_lat
FROM imu_data imu
WINDOW JOIN gps_data gps
ON imu.vehicle_id = gps.vehicle_id
AND gps.timestamp BETWEEN imu.timestamp - 50000000 AND imu.timestamp + 50000000

-- Real-time anomaly detection
SELECT timestamp, sensor_id, DELTA(vibration) AS vibration_delta
FROM vehicle_sensors
WHERE DELTA(vibration) OVER (ORDER BY timestamp) > 500
```

**Sales message:**
> "The era of ROS bag + offline analysis is over. Fuse vehicle sensor data in real-time with ZeptoDB and connect to Python ML pipeline zero-copy. 522ns latency."

**Competitive advantages:**
- InfluxDB: ms latency, no ASOF JOIN
- TimescaleDB: has SQL but no SIMD, slow
- Redis: no persistent storage, no aggregation
- **ZeptoDB: μs latency + ASOF/Window JOIN + Python zero-copy + HDB history**

---

### B. Smart Factory / Industrial IoT

> **MQTT ingestion: implemented** ✅ — `MqttConsumer` (QoS 0/1/2, topic wildcards,
> shared JSON/BINARY/JSON_HUMAN decoders with Kafka). See
> [`devlog/081_mqtt_consumer.md`](../devlog/081_mqtt_consumer.md).

**Customers:** Samsung Electronics, SK Hynix, TSMC, Siemens, Bosch, POSCO

**Data characteristics:**
```
Semiconductor fab: CMP pressure sensor 10KHz × 500 units = 5M points/sec
Auto factory: welding robot current/voltage 1KHz × 200 units = 200K points/sec
Steel: rolling mill vibration 5KHz × 50 sensors = 250K points/sec
```

**Current pain points:**
- InfluxDB/Prometheus cannot handle high-frequency data above 10KHz
- Anomaly detection is batch processing → post-incident analysis
- Complex to connect predictive maintenance ML models to equipment

**ZeptoDB solution:**
```sql
-- Real-time predictive maintenance (anomaly detection)
SELECT equipment_id, timestamp,
       EMA(vibration, 0.1) OVER (PARTITION BY equipment_id) AS ema_slow,
       EMA(vibration, 20) OVER (PARTITION BY equipment_id) AS ema_fast,
       DELTA(temperature) OVER (PARTITION BY equipment_id) AS temp_rate
FROM factory_sensors
WHERE DELTA(vibration) OVER (...) > threshold

-- Production line xbar aggregation (1-min KPI)
SELECT xbar(timestamp, 60000000000) AS minute,
       avg(yield_rate) AS yield, count(*) AS samples
FROM production_line
GROUP BY xbar(timestamp, 60000000000)
```

**Business value:**
- Semiconductor fab 1-minute downtime = $500K loss
- Real-time anomaly detection cuts detection time from 10 min → 10 sec
- Predictive maintenance reduces unplanned downtime by 80%

**Sales message:**
> "No DB could process semiconductor fab 10KHz sensor data in real-time — until now. ZeptoDB delivers 5.52M points/sec ingestion, 272μs anomaly detection, and direct Python ML connection to make predictive maintenance a reality."

---

### C. Robotics (Humanoid / Industrial Robot)

**Customers:** Figure AI, Boston Dynamics, FANUC, ABB, Hyundai Robotics, NVIDIA Isaac partners

**Data characteristics:**
```
Humanoid robot:
  Joint torque: 30 joints × 1KHz = 30K points/sec
  Tactile sensors: fingers × 100Hz
  Vision: 2 cameras × 30fps
  → Real-time policy update (reinforcement learning)

Industrial robot:
  Motor current: 6 axes × 1KHz
  Force/torque: 6DOF × 500Hz
```

**Physical AI special requirements:**
```
Robot learning loop:
Sensor → real-time feature calculation → neural net inference → action → back to sensor
                    ↑
            ZeptoDB needed here
            (Feature Store + Replay Buffer)
```

**ZeptoDB solution:**
```python
import zeptodb

# Robot sensor stream → Feature Store
db = zeptodb.Pipeline()
db.start()

# Real-time feature calculation (C++ engine)
joint_features = db.query("""
    SELECT joint_id,
           EMA(torque, 10) OVER (PARTITION BY joint_id) AS ema_torque,
           DELTA(position) OVER (PARTITION BY joint_id) AS velocity,
           AVG(torque) OVER (PARTITION BY joint_id ROWS 100 PRECEDING) AS ma100
    FROM robot_joints
    ORDER BY timestamp DESC LIMIT 30
""")

# zero-copy → PyTorch tensor (522ns)
torque_array = db.get_column(joint_id=0, name="ema_torque")
tensor = torch.from_numpy(torque_array)  # no copy
action = policy_network(tensor)
```

**Sales message:**
> "The bottleneck in robot reinforcement learning is the Feature Store. Connect sensor → Feature → PyTorch pipeline in 522ns with ZeptoDB Python zero-copy. NVIDIA Isaac compatible."

---

### D. Drone Swarm

**Customers:** Zipline, Wing (Google), defense contractors, agricultural drone companies, logistics automation

**Data characteristics:**
```
100-drone swarm:
  GPS/IMU: 100Hz × 100 drones = 10K points/sec
  Battery/motor: 50Hz × 100 drones = 5K points/sec
  Collision avoidance: each drone needs real-time position of drones within 50m
```

**ZeptoDB distributed cluster:**
```
Ground Station (coordinator node)
    ├── Edge Node 1 (drones 1~25)
    ├── Edge Node 2 (drones 26~50)
    ├── Edge Node 3 (drones 51~75)
    └── Edge Node 4 (drones 76~100)

Each edge node: ZeptoDB LocalQueryScheduler
Inter-node: UCX Transport (future CXL)
```

```sql
-- Collision avoidance: drones within 50m (Window JOIN)
SELECT d1.drone_id, wj_avg(d2.latitude) AS nearby_lat
FROM drone_positions d1
WINDOW JOIN drone_positions d2
ON d2.timestamp BETWEEN d1.timestamp - 100000000 AND d1.timestamp + 100000000
WHERE haversine(d1.lat, d1.lon, d2.lat, d2.lon) < 50
```

---

## 3. ZeptoDB x Physical AI Positioning Matrix

```
                    LOW LATENCY
                        ↑
           ZeptoDB ●    │
            (μs)         │
                         │    ● Redis
                         │      (ms, no persistence)
 FINANCIAL ──────────────┼────────────── PHYSICAL AI
   ONLY                  │                 BOTH
                         │
              ● InfluxDB │ ● TimescaleDB
                (ms)     │   (ms, SQL)
                         │
                    HIGH LATENCY
```

ZeptoDB is the only open-source DB competitive in **both finance and Physical AI**.

---

## 4. Go-To-Market Strategy

### Phase 1: Finance (Current)
- Positioning as kdb+ replacement
- HFT / quant hedge fund target
- Singapore financial hub as base

### Phase 2: Smart Factory (6~12 months)
- Semiconductor fabs (Samsung, SK, TSMC) — leverage Korea/Taiwan network
- Positioning as InfluxDB/Prometheus replacement
- "The only open-source DB that handles 10KHz sensor data"

### Phase 3: Autonomous Driving / Robotics (12~24 months)
- Link with NVIDIA Isaac ecosystem
- Develop ROS2 plugin
- "Sensor Fusion DB" positioning

### Phase 4: Drone / Edge (24+ months)
- After distributed edge cluster completion
- Defense / logistics automation

---

## 5. Website Update Proposal

Expand current 4 target markets → 7:

Existing:
- HFT / Hedge Funds
- Quant Research
- Risk/Compliance
- Crypto Exchanges

Add:
- **Robotics & Physical AI** — Feature Store, real-time sensor processing
- **Smart Factory** — predictive maintenance, real-time anomaly detection
- **Autonomous Vehicles** — Sensor Fusion, multi-sensor ASOF JOIN

---

## 6. Core Message

> **"Technology proven in HFT, expanded to Physical AI."**
>
> μs latency isn't only needed for high-frequency trading.
> It's needed for robots avoiding obstacles, autonomous vehicles fusing sensors,
> and factories predicting failures.
>
> ZeptoDB: wherever time-series data is mission-critical.
