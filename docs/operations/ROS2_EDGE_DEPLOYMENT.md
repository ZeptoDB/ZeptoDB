# ROS 2 Edge Deployment Guide

This guide covers robot-local and lab-edge ZeptoDB deployments for ROS 2 /
Physical AI telemetry.

## Deployment Profiles

| Profile | Use | Recommended runtime |
|---|---|---|
| Robot-local recorder | Single robot, rosbag replay, feature capture | `systemd` or Docker Compose |
| Lab edge gateway | Fleet telemetry, shared dashboards, local replay | Docker Compose |
| Factory edge cluster | Multi-line AGV/PLC/robot telemetry | k3s with local SSD |

## Docker Compose

```yaml
services:
  zeptodb:
    image: zeptodb/zeptodb:latest
    network_mode: host
    environment:
      ZEPTO_USE_ROS2: "ON"
      ZEPTO_LOG_LEVEL: info
    volumes:
      - /var/lib/zeptodb:/var/lib/zeptodb
      - /opt/ros:/opt/ros:ro
    command:
      - zepto_http_server
      - --host
      - 0.0.0.0
      - --port
      - "8123"
```

Use host networking when the ROS graph is local to the edge host. For isolated
replay jobs, bridge networking is fine and avoids DDS discovery noise.

## systemd

```ini
[Unit]
Description=ZeptoDB ROS 2 edge bridge
After=network-online.target
Wants=network-online.target

[Service]
Environment=RMW_IMPLEMENTATION=rmw_fastrtps_cpp
Environment=ZEPTO_LOG_LEVEL=info
ExecStart=/usr/local/bin/zepto_http_server --host 0.0.0.0 --port 8123
Restart=always
RestartSec=5
LimitNOFILE=1048576
MemoryMax=8G
CPUQuota=300%

[Install]
WantedBy=multi-user.target
```

## k3s

Use a `StatefulSet` with local persistent volumes for hot telemetry and expose
the HTTP service through a `ClusterIP` or node-local ingress. Keep ROS 2 DDS
traffic on the robot VLAN; send ZeptoDB writes through HTTP/RPC rather than
bridging DDS across sites.

Minimal resource envelope:

| Component | CPU | Memory | Storage |
|---|---:|---:|---:|
| Robot-local ZeptoDB | 2 vCPU | 4-8 GiB | 100 GiB NVMe |
| Lab edge ZeptoDB | 4-8 vCPU | 16-32 GiB | 1 TiB NVMe |
| Factory edge cluster node | 8-16 vCPU | 32-64 GiB | 2+ TiB NVMe |

## ROS 2 Bridge Policy

- Use `Ros2IngestMode::TypedProfile` for IMU, JointState, Odometry, TF, and
  LaserScan when the target table schema is known.
- Use `StandardProfile` for quick scalar feature extraction.
- Use rosbag2 import for deterministic replay and model training datasets.
- Keep topic allowlists explicit in production.
- Never replay into a live robot graph. Replay into ZeptoDB tables only.

## Monitoring

Scrape the bridge counters:

```text
zepto_ros2_messages_consumed_total
zepto_ros2_rows_ingested_total
zepto_ros2_route_local_total
zepto_ros2_route_remote_total
zepto_ros2_ingest_failures_total
zepto_ros2_source_lag_ns
```

Alert when `ingest_failures_total` increases, `source_lag_ns` exceeds the
robot/factory SLA, or `route_remote_total` drops to zero unexpectedly in a
multi-node deployment.

## Cold-Start Checklist

1. Create typed profile tables before starting the bridge.
2. Verify `ros2 topic list` sees only expected topics.
3. Start ZeptoDB and confirm `/metrics`.
4. Start the bridge with one topic allowlist.
5. Run a one-minute rosbag replay and compare row counts.
6. Enable production topic subscriptions.
