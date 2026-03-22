# APEX-DB Production Operations Guide

## Table of Contents
- [1. Initial Setup](#1-initial-setup)
- [2. Monitoring](#2-monitoring)
- [3. Logging](#3-logging)
- [4. Backup & Recovery](#4-backup--recovery)
- [5. Automation](#5-automation)
- [6. Troubleshooting](#6-troubleshooting)

---

## 1. Initial Setup

### 1.1 Service Installation

```bash
# 1. Build APEX-DB
cd /home/ec2-user/apex-db
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 2. Install production service (requires root)
cd ..
sudo ./scripts/install_service.sh
```

Installation includes:
- ✅ `apex` user created
- ✅ Directories: `/opt/apex-db`, `/data/apex-db`, `/var/log/apex-db`
- ✅ systemd service: `apex-db.service`
- ✅ Cron jobs: backup (02:00), EOD (18:00 weekdays)
- ✅ Log rotation: 30-day retention

### 1.2 Start Service

```bash
# Start service
sudo systemctl start apex-db

# Check status
sudo systemctl status apex-db

# View logs
sudo journalctl -u apex-db -f
```

### 1.3 Health Check

```bash
# Liveness probe (server alive?)
curl http://localhost:8123/health
# {"status":"healthy"}

# Readiness probe (ready to serve queries?)
curl http://localhost:8123/ready
# {"status":"ready"}

# Statistics
curl http://localhost:8123/stats
```

---

## 2. Monitoring

### 2.1 Prometheus + Grafana Setup

```bash
# Run monitoring stack with Docker Compose
cd /home/ec2-user/apex-db/monitoring
docker-compose up -d

# Check services
docker-compose ps
```

**Access:**
- Grafana: http://localhost:3000 (admin/apex-admin-2026)
- Prometheus: http://localhost:9090

### 2.2 Grafana Dashboard Configuration

1. Log in to Grafana
2. Configuration → Data Sources → Add Prometheus
   - URL: `http://prometheus:9090`
3. Dashboards → Import
   - File: `grafana-dashboard.json`

### 2.3 Check Metrics

```bash
# Check Prometheus metrics
curl http://localhost:8123/metrics
```

**Key metrics:**

| Metric | Type | Description |
|--------|------|-------------|
| `apex_ticks_ingested_total` | counter | Total ingested ticks |
| `apex_ticks_stored_total` | counter | Stored ticks |
| `apex_ticks_dropped_total` | counter | Dropped ticks |
| `apex_queries_executed_total` | counter | Executed queries |
| `apex_rows_scanned_total` | counter | Scanned rows |
| `apex_server_up` | gauge | Server running state (0/1) |
| `apex_server_ready` | gauge | Readiness state (0/1) |

### 2.4 Alert Configuration

Edit Alertmanager configuration:

```bash
vi /home/ec2-user/apex-db/monitoring/alertmanager.yml
```

**Slack Webhook setup:**
1. Slack → Apps → Incoming Webhooks
2. Copy Webhook URL
3. Replace `YOUR_SLACK_WEBHOOK_URL` in `alertmanager.yml`

**PagerDuty setup:**
1. PagerDuty → Service → Integrations → Prometheus
2. Copy Integration Key
3. Replace `YOUR_PAGERDUTY_SERVICE_KEY` in `alertmanager.yml`

---

## 3. Logging

### 3.1 Structured Logging

APEX-DB produces structured logs in JSON format.

**Log locations:**
- File: `/var/log/apex-db/apex-db.log`
- systemd: `journalctl -u apex-db`

**Log levels:**
- `TRACE` - Detailed debug
- `DEBUG` - Development info
- `INFO` - General info
- `WARN` - Warnings
- `ERROR` - Errors
- `CRITICAL` - Fatal errors

### 3.2 Viewing Logs

```bash
# Recent logs (journalctl)
sudo journalctl -u apex-db -n 100

# Live logs
sudo journalctl -u apex-db -f

# JSON log file
sudo tail -f /var/log/apex-db/apex-db.log | jq .

# Filter errors only
sudo journalctl -u apex-db -p err
```

### 3.3 Log Example

```json
{
  "timestamp": "2026-03-22T14:30:45.123+0900",
  "level": "INFO",
  "message": "Query executed successfully",
  "component": "QueryExecutor",
  "details": "SELECT sum(volume) FROM trades - 1.2ms"
}
```

### 3.4 Log Rotation

Automatically configured (`/etc/logrotate.d/apex-db`):
- Daily rotation
- 30-day retention
- Compressed storage

---

## 4. Backup & Recovery

### 4.1 Automatic Backup

**Cron configuration (auto-runs):**
```
0 2 * * * /opt/apex-db/scripts/backup.sh
```

**Manual backup:**
```bash
sudo -u apex /opt/apex-db/scripts/backup.sh
```

**Backup contents:**
- HDB (Historical Database)
- WAL (Write-Ahead Log)
- Config files
- Metadata

**Backup location:**
- Local: `/backup/apex-db/apex-db-backup-YYYYMMDD_HHMMSS.tar.gz`
- S3: `s3://${S3_BUCKET}/backups/` (optional)

### 4.2 S3 Backup Configuration

```bash
# Set environment variables
export APEX_S3_BACKUP_BUCKET="my-apex-backups"
export AWS_REGION="us-east-1"

# Required IAM permissions:
# - s3:PutObject
# - s3:GetObject
# - s3:ListBucket
# - s3:DeleteObject
```

### 4.3 Disaster Recovery

**Restore from local backup:**
```bash
# 1. Stop APEX-DB
sudo systemctl stop apex-db

# 2. Restore backup
sudo /opt/apex-db/scripts/restore.sh apex-db-backup-20260322_020000

# 3. Restart service
sudo systemctl start apex-db
```

**Restore from S3 backup:**
```bash
sudo /opt/apex-db/scripts/restore.sh apex-db-backup-20260322_020000 --from-s3
```

**Skip WAL replay:**
```bash
sudo /opt/apex-db/scripts/restore.sh apex-db-backup-20260322_020000 --skip-wal-replay
```

### 4.4 Backup Retention Policy

**Local:**
- Retention period: 30 days (default)
- Configure via `BACKUP_RETENTION_DAYS` environment variable

**S3:**
- Lifecycle policy recommended
- STANDARD_IA (Infrequent Access) storage class

---

## 5. Automation

### 5.1 systemd Service

**Service management:**
```bash
# Start
sudo systemctl start apex-db

# Stop
sudo systemctl stop apex-db

# Restart
sudo systemctl restart apex-db

# Status
sudo systemctl status apex-db

# Enable auto-start on boot
sudo systemctl enable apex-db
```

**Service features:**
- ✅ Auto-restart (5 seconds after failure)
- ✅ CPU affinity (cores 0-7)
- ✅ OOM protection (priority -900)
- ✅ Resource limits (1M files, unlimited memory)

### 5.2 EOD (End-of-Day) Process

**Auto-runs (cron):**
```
0 18 * * 1-5 /opt/apex-db/scripts/eod_process.sh
```

**Manual run:**
```bash
sudo -u apex /opt/apex-db/scripts/eod_process.sh
```

**EOD tasks:**
1. RDB → HDB flush
2. Statistics collection
3. WAL cleanup (compress after 7 days, delete after 30 days)
4. Automatic backup
5. Disk usage check

**Logs:**
```bash
tail -f /var/log/apex-db/eod.log
```

### 5.3 Auto Tuning

**Bare metal tuning:**
```bash
sudo /opt/apex-db/scripts/tune_bare_metal.sh
```

Tuning items:
- CPU governor → performance
- Turbo Boost enabled
- Hugepages 32GB
- IRQ affinity
- Network stack

---

## 6. Troubleshooting

### 6.1 Service Won't Start

```bash
# Check logs
sudo journalctl -u apex-db -n 100

# Check config file
cat /opt/apex-db/config.yaml

# Check port conflict
sudo lsof -i :8123

# Check permissions
ls -la /data/apex-db
```

### 6.2 High Tick Drop Rate

**Causes:**
- Ring buffer too small
- CPU overload
- Disk I/O bottleneck

**Resolution:**
```bash
# 1. Check metrics
curl http://localhost:8123/metrics | grep dropped

# 2. Check CPU usage
top -u apex

# 3. Increase ring buffer size (config.yaml)
ring_buffer_size: 1048576  # default 524288

# 4. Restart
sudo systemctl restart apex-db
```

### 6.3 Slow Queries

```bash
# 1. Check rows scanned
curl http://localhost:8123/stats

# 2. Check query execution plan (EXPLAIN)
curl -X POST http://localhost:8123/ -d "EXPLAIN SELECT ..."

# 3. Check HDB compression status
du -sh /data/apex-db/hdb

# 4. Verify parallel query enabled (config.yaml)
query_threads: 8
```

### 6.4 Disk Full

```bash
# 1. Check usage
df -h /data/apex-db

# 2. Clean old HDB
find /data/apex-db/hdb -type d -mtime +90 -exec rm -rf {} \;

# 3. Compress WAL
find /data/apex-db/wal -name "*.wal" -mtime +7 -exec gzip {} \;

# 4. Clean backups
find /backup/apex-db -name "*.tar.gz" -mtime +30 -delete
```

### 6.5 Out of Memory

```bash
# 1. Check memory usage
free -h
pmap -x $(pgrep apex-server)

# 2. OOM killer logs
dmesg | grep -i "out of memory"

# 3. Check hugepages
cat /proc/meminfo | grep Huge

# 4. Restart process (clear memory)
sudo systemctl restart apex-db
```

### 6.6 Prometheus Metrics Not Visible

```bash
# 1. Check /metrics endpoint
curl http://localhost:8123/metrics

# 2. Check Prometheus targets
curl http://localhost:9090/api/v1/targets | jq .

# 3. Prometheus logs
docker logs apex-prometheus

# 4. Check firewall
sudo iptables -L -n | grep 8123
```

---

## 7. Performance Optimization

### 7.1 Bare Metal Environment

```bash
# CPU isolation (GRUB)
vi /etc/default/grub
# GRUB_CMDLINE_LINUX="isolcpus=0-7 nohz_full=0-7"

# Auto tuning
sudo /opt/apex-db/scripts/tune_bare_metal.sh

# Check NUMA topology
numactl --hardware
```

### 7.2 Cloud Environment

**AWS optimization:**
- Instance: `c7g.16xlarge` (64 vCPU, 128GB RAM)
- Storage: `io2` EBS (64K IOPS)
- Network: Enhanced Networking (ENA)
- Placement Group: `cluster`

**Kubernetes optimization:**
```yaml
# Resource allocation
resources:
  requests:
    cpu: "32"
    memory: "64Gi"
  limits:
    cpu: "64"
    memory: "128Gi"

# Node affinity
nodeSelector:
  node.kubernetes.io/instance-type: c7g.16xlarge
```

---

## 8. Security

### 8.1 Network Security

```bash
# Firewall configuration (iptables)
sudo iptables -A INPUT -p tcp --dport 8123 -s 10.0.0.0/8 -j ACCEPT
sudo iptables -A INPUT -p tcp --dport 8123 -j DROP

# UFW
sudo ufw allow from 10.0.0.0/8 to any port 8123
```

### 8.2 TLS Configuration

```yaml
# config.yaml
server:
  tls:
    enabled: true
    cert_file: /etc/apex-db/server.crt
    key_file: /etc/apex-db/server.key
```

### 8.3 Authentication

```yaml
# config.yaml
auth:
  enabled: true
  type: basic  # or jwt
  users:
    - username: admin
      password_hash: "$2a$10$..."
```

---

## 9. Contact

**For issues:**
- GitHub Issues: https://github.com/apex-db/apex-db/issues
- Slack: #apex-db-support
- Email: support@apex-db.io

**Critical incidents:**
- PagerDuty: APEX-DB Critical Alerts
- On-call: +1-XXX-XXX-XXXX
