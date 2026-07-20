# ZeptoDB Kubernetes Operations Guide

Last updated: 2026-07-19

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Initial Deployment](#2-initial-deployment)
3. [Day-2 Operations](#3-day-2-operations)
4. [Monitoring & Alerting](#4-monitoring--alerting)
5. [Scaling](#5-scaling)
6. [Backup & Recovery](#6-backup--recovery)
7. [Upgrades & Rollback](#7-upgrades--rollback)
8. [Security](#8-security)
9. [Cluster Mode](#9-cluster-mode)
10. [Troubleshooting](#10-troubleshooting)
11. [Runbooks](#11-runbooks)

> **See also:** [Failure Scenarios & Recovery Guide](KUBERNETES_FAILURE_SCENARIOS.md) — Automatic/manual recovery procedures for 8 failure scenarios

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│  Kubernetes Cluster                                          │
│                                                              │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  Namespace: zeptodb                                   │   │
│  │                                                       │   │
│  │  ┌─────────┐                                          │   │
│  │  │ Pod-0   │  ← standalone Deployment (default: 1)   │   │
│  │  │ ZeptoDB │     or opt-in cluster StatefulSet        │   │
│  │  │ :8123   │                                          │   │
│  │  └────┬────┘                                          │   │
│  │       │                                                │   │
│  │  ┌────┴─────────────────────────────────┐              │   │
│  │  │  Service (ClusterIP :8123)            │              │   │
│  │  │  + Headless Service (pod discovery)   │              │   │
│  │  └──────────────────────────────────────┘              │   │
│  │                                                       │   │
│  │ ConfigMap │ optional eval PVC │ PDB │ authenticated SM│   │
│  └──────────────────────────────────────────────────────┘   │
│                                                              │
│  ┌──────────────────────┐  ┌─────────────────────────────┐  │
│  │ Prometheus           │  │ Grafana                      │  │
│  │ ServiceMonitor 15s   │  │ Dashboard + 9 Alert Rules    │  │
│  └──────────────────────┘  └─────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### Helm Chart Components

| Resource | Template | Purpose |
|----------|----------|---------|
| Deployment | `deployment.yaml` | Single standalone ZeptoDB pod by default |
| StatefulSet | `statefulset.yaml` | Opt-in distributed cluster (`cluster.enabled=true`) |
| Service | `service.yaml` | Private ClusterIP + Headless |
| Secrets | `secret.yaml` | Bootstrap API key store + cluster peer secret |
| ConfigMap | `configmap.yaml` | `zeptodb.conf` |
| PVC | `pvc.yaml` | Opt-in tiered-storage evaluation; not a durability guarantee |
| HPA | `hpa.yaml` | Template retained, but rendering is blocked until peer discovery is dynamic |
| PDB | `pdb.yaml` | minAvailable: 1 by default |
| ServiceMonitor | `servicemonitor.yaml` | Prometheus scrape config |

---

## 2. Initial Deployment

### Prerequisites

```bash
# Required
kubectl version --client    # 1.26+
helm version                # 3.x

# Verify cluster access
kubectl cluster-info
kubectl get nodes
```

### Deploy with Helm (Recommended)

```bash
# Create namespace
kubectl create namespace zeptodb

# Install
helm install zeptodb ./deploy/helm/zeptodb \
  -n zeptodb \
  --set image.repository=your-registry/zeptodb \
  --set image.tag=1.0.0

# Verify
kubectl get all -n zeptodb
```

The portable default is intentionally in-memory. It survives neither process
nor pod replacement. Enabling a PVC is gated because hot-partition WAL/recovery
coverage and merging HDB rows into general SQL reads are not complete; no
current chart profile should be presented as data-durable production.

### Release-candidate values (durability blocked)

The following is suitable only for controlled topology/security evaluation,
not durable production:

```yaml
replicaCount: 3

service:
  type: ClusterIP

auth:
  enabled: true
  existingSecret: zeptodb-auth

# The cluster Secret is separate so peer credentials can rotate
# independently from client API credentials.
cluster:
  enabled: true
  security:
    enabled: true
    existingSecret: zeptodb-cluster

image:
  repository: your-registry/zeptodb
  tag: "1.0.0"

resources:
  requests:
    cpu: "4"
    memory: "16Gi"
  limits:
    cpu: "8"
    memory: "32Gi"

persistence:
  enabled: true
  # Explicit evaluation acknowledgement; not a durability assertion.
  acknowledgeIncompleteDurability: true
  storageClass: gp3
  size: 500Gi

config:
  workerThreads: 8
  parallelThreshold: 100000

autoscaling:
  enabled: false

podDisruptionBudget:
  enabled: true
  minAvailable: 2

# Graviton (ARM) nodes
nodeSelector:
  kubernetes.io/arch: arm64
  # or for x86:
  # kubernetes.io/arch: amd64
```

```bash
helm install zeptodb ./deploy/helm/zeptodb \
  -n zeptodb \
  -f values-prod.yaml \
  --wait --timeout 5m
```

### Post-Deploy Verification

```bash
# All pods running
kubectl get pods -n zeptodb -o wide

# Keep the chart Service private; use port-forward for initial verification.
kubectl port-forward svc/zeptodb 8123:8123 -n zeptodb &
export ZEPTO_ADMIN_KEY="$(kubectl get secret zeptodb-auth -n zeptodb \
  -o jsonpath="{.data['admin-api-key']}" | base64 --decode)"
curl -s http://127.0.0.1:8123/health
curl -s http://127.0.0.1:8123/ready

# Test query
curl -H "Authorization: Bearer $ZEPTO_ADMIN_KEY" \
  -X POST http://127.0.0.1:8123/ -d 'SELECT 1'
```

---

## 3. Day-2 Operations

### Daily Checks

```bash
#!/bin/bash
# daily-check.sh — run from cron or manually

NS=zeptodb
# The default Service is private. Start `kubectl port-forward
# svc/zeptodb 8123:8123 -n zeptodb` or set this to the approved HTTPS ingress.
ZEPTO_URL=${ZEPTO_URL:-http://127.0.0.1:8123}

echo "=== Pod Status ==="
kubectl get pods -n $NS -o wide

echo "=== Health ==="
curl -sf "$ZEPTO_URL/health" && echo " OK" || echo " FAIL"

echo "=== Readiness ==="
curl -sf "$ZEPTO_URL/ready" && echo " OK" || echo " FAIL"

echo "=== HPA ==="
kubectl get hpa -n $NS

echo "=== PVC ==="
kubectl get pvc -n $NS

echo "=== Recent Events ==="
kubectl get events -n $NS --sort-by='.lastTimestamp' | tail -10
```

### Configuration Changes

When a ConfigMap is changed, the `checksum/config` annotation automatically triggers a rollout.

```bash
# Change worker threads
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set config.workerThreads=16 \
  --wait

# Change multiple settings at once
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  -f values-prod.yaml \
  --set config.workerThreads=16 \
  --set config.queryCacheSize=2000 \
  --wait
```

### Checking Logs

```bash
# Logs for a specific pod
kubectl logs -f <pod-name> -n zeptodb

# Logs for all pods (stern recommended)
stern zeptodb -n zeptodb

# Previous crash logs
kubectl logs <pod-name> -n zeptodb --previous

# Logs since a specific time
kubectl logs <pod-name> -n zeptodb --since=1h
```

### Pod Restart

```bash
# Standalone restart (one-pod maintenance window)
kubectl rollout restart deployment/zeptodb -n zeptodb

# Cluster rolling restart
kubectl rollout restart statefulset/zeptodb -n zeptodb

# Delete a specific pod only (the owning controller recreates it)
kubectl delete pod <pod-name> -n zeptodb
```

---

## 4. Monitoring & Alerting

### Prometheus Setup

```bash
# Enable the credential-aware ServiceMonitor (requires Prometheus Operator).
# The chart generates a dedicated metrics-role token and references it from
# the auth Secret; the raw token is not mounted into the ZeptoDB pod.
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set serviceMonitor.enabled=true \
  --set serviceMonitor.interval=15s
```

Do not enable `prometheus.scrape` while authentication is enabled. Kubernetes
pod scrape annotations cannot attach the required Bearer token, and the chart
rejects that combination. Without the Prometheus Operator, configure the
scraper externally with a metrics-role Bearer credential from an approved
secret provider.

### Key Metrics

```bash
# Check directly with the generated or operator-managed metrics credential.
export ZEPTO_METRICS_KEY="$(kubectl get secret zeptodb-auth -n zeptodb \
  -o jsonpath="{.data['metrics-api-key']}" | base64 --decode)"
curl -s -H "Authorization: Bearer $ZEPTO_METRICS_KEY" "$ZEPTO_URL/metrics"
```

| Metric | Type | Alert Threshold |
|--------|------|-----------------|
| `zepto_server_up` | gauge | == 0 → critical |
| `zepto_server_ready` | gauge | == 0 for 5m → warning |
| `zepto_ticks_ingested_total` | counter | rate < 1000/s → warning |
| `zepto_ticks_dropped_total` | counter | rate > 1000/s → warning |
| `zepto_queries_executed_total` | counter | rate > 100/s → info |
| `zepto_rows_scanned_total` | counter | rate > 10M/s → warning |

### Alert Rules (9 rules)

Defined in `monitoring/zeptodb-alerts.yml`:

| Alert | Severity | Condition |
|-------|----------|-----------|
| ApexDBDown | critical | `zepto_server_up == 0` for 1m |
| ApexDBNotReady | warning | `zepto_server_ready == 0` for 5m |
| HighTickDropRate | warning | drop rate > 1000/s for 2m |
| HighQueryRate | info | query rate > 100/s for 5m |
| HighRowScanRate | warning | scan rate > 10M/s for 5m |
| LowIngestionRate | warning | ingestion < 1000/s for 10m |
| HighDiskUsage | warning | disk < 20% free for 5m |
| HighMemoryUsage | warning | memory < 10% free for 5m |
| HighCPUUsage | warning | CPU > 90% for 10m |

### Grafana Dashboard

```bash
# Import dashboard
kubectl create configmap grafana-zeptodb \
  -n monitoring \
  --from-file=monitoring/grafana-dashboard.json

# Or import via Grafana UI → Import → monitoring/grafana-dashboard.json
```

Grafana can connect directly as a ClickHouse data source (port 8123, ClickHouse compatible API).

---

## 5. Scaling

### Cluster Requirements

See [EKS Cluster Requirements](../deployment/EKS_CLUSTER_REQUIREMENTS.md) for full cluster setup including K8s version, Auto Mode, and custom NodePool configuration.

### EKS Auto Mode (Node Auto-Scaling)

EKS Auto Mode includes built-in Karpenter — no separate install needed. Nodes are provisioned via EC2 Fleet API when pods are pending.

```bash
# Check node pools (built-in + custom)
kubectl get nodepools
kubectl get nodeclasses

# Check node claims (active nodes)
kubectl get nodeclaims

# Monitor scaling events
kubectl describe nodepool zepto-realtime
kubectl describe nodepool zepto-analytics
```

Two custom node pools are configured:

| Pool | Trigger | Capacity | Consolidation |
|------|---------|----------|---------------|
| **zepto-realtime** | Pending pods with `zeptodb.com/role: realtime` | On-Demand only | WhenEmpty, after 30m |
| **zepto-analytics** | Pending pods with `zeptodb.com/role: analytics` | Spot + On-Demand | WhenEmptyOrUnderutilized, after 5m |

Node autoscaling still provisions capacity for operator-approved static replica
changes. Automatic ZeptoDB pod-count changes remain blocked as described below.

### Horizontal Pod Autoscaler (HPA)

The HPA is disabled and rejected at Helm render time in both standalone and
cluster modes. A standalone Deployment cannot exceed one replica because each
process owns an independent database. The current StatefulSet generates a
static peer list from `replicaCount`; an HPA-created ordinal would therefore
start with incomplete membership. Until dynamic peer discovery lands, use only
an explicitly reviewed static cluster topology and coordinated membership
changes.

```bash
# Check HPA status
kubectl get hpa -n zeptodb
kubectl describe hpa zeptodb -n zeptodb

# A replica-count change must be performed through a reviewed values update so
# every ordinal receives the same static peer topology. Do not use kubectl scale.
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set cluster.enabled=true \
  --set replicaCount=5
```

### Ingest-rate HPA (P8-I4, devlog 117)

For production ingest workloads, CPU/memory utilization is a poor proxy:
a pod can be CPU-idle while its ring buffer saturates, or CPU-busy on
queries while ingest is light. ZeptoDB exposes
`zepto_ingest_ticks_per_sec` on `GET /metrics` (a per-pod gauge of the
instantaneous ingest rate) so the HPA can autoscale on real ingest load.
CPU/memory metrics remain configured on the same HPA as a safety net.

**Prerequisites.** The custom Pods metric requires
[`prometheus-adapter`](https://github.com/kubernetes-sigs/prometheus-adapter)
to expose `zepto_ingest_ticks_per_sec` as `pods/zepto_ingest_ticks_per_sec`.
A minimal rule snippet:

```yaml
# prometheus-adapter ConfigMap
rules:
  - seriesQuery: 'zepto_ingest_ticks_per_sec{namespace!="",pod!=""}'
    resources:
      overrides:
        namespace: { resource: namespace }
        pod:       { resource: pod }
    name:
      matches: "^(.*)$"
      as: "$1"
    metricsQuery: |
      avg_over_time(<<.Series>>{<<.LabelMatchers>>}[1m])
```

The metric and HPA template wiring remain available for future promotion, but
`autoscaling.enabled=true` is currently rejected before either CPU/memory or
ingest-rate scaling can deploy. Do not enable `ingestRateEnabled` in production
until dynamic StatefulSet peer discovery and membership convergence are
verified.

`targetIngestRate` is `AverageValue` for the HPA Pods metric — scale-out
is triggered when the per-pod 1-minute average ingest rate exceeds it.
Tune to ~70–80% of a single pod's measured sustained ingest ceiling
(see `docs/devlog/102_ingest_scale_phase1.md` for the underlying
ingest-path tunables and `pipeline.drainThreads` /
`pipeline.ringBufferCapacity`).

**Karpenter compatibility.** Karpenter may provision nodes for a reviewed
static replica increase. The HPA-to-Karpenter path is intentionally unavailable
while automatic database membership is blocked.

### Scale-Down Protection

```yaml
# Already configured in values.yaml
autoscaling:
  scaleDown:
    stabilizationSeconds: 300   # Scale down after 5-minute stabilization
  scaleUp:
    stabilizationSeconds: 60    # Scale up after 1-minute stabilization
```

### Vertical Scaling

```bash
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set resources.requests.cpu=8 \
  --set resources.requests.memory=32Gi \
  --set resources.limits.cpu=16 \
  --set resources.limits.memory=64Gi \
  --wait
```

### Node Selection (Graviton / x86)

```bash
# Graviton (ARM) nodes
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set nodeSelector."kubernetes\.io/arch"=arm64

# Dedicated instance type
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set nodeSelector."node\.kubernetes\.io/instance-type"=c7g.4xlarge
```

### Vertical ingest tuning (devlog 102)

Phase 1 of the ingest scale-out plan exposes two single-pod ingest
knobs via Helm. Both default to `0` (engine default) so existing
charts are unchanged.

| Helm value | Engine default (when `0`) | Purpose |
|---|---|---|
| `pipeline.drainThreads` | `max(2, hw_concurrency() / 4)` | Number of drain threads moving ticks from `TickPlant` → storage. Lock-free MPMC → scales near-linearly. |
| `pipeline.ringBufferCapacity` | `65536` slots | `TickPlant` ring-buffer size. Absorbs ingest bursts before the synchronous `store_tick()` fallback (~34× slower) kicks in. Must be a power of two in `[4096, 16777216]`. |

#### When to tune

| Workload | Tags × rate | `pipeline.drainThreads` | `pipeline.ringBufferCapacity` |
|---|---|---|---|
| IoT pilot | 1 k × 1 Hz | `0` (auto) | `65536` (default) |
| Auto factory | 5 k × 100 Hz | `4` | `262144` |
| Semi fab (CMP burst) | 30 k × 10 kHz | `8` | `1048576` |

```bash
# Raise both for a CMP-burst semi-fab workload
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set pipeline.drainThreads=8 \
  --set pipeline.ringBufferCapacity=1048576
```

#### How to observe

Both effective values are emitted on `ZeptoPipeline::start()`:

```bash
kubectl logs -n zeptodb zeptodb-0 | grep "drain_threads="
# [info] ZeptoPipeline 시작 완료 (drain_threads=8, ring_capacity=1048576)
```

Also exposed as pod env vars for K8s-side inspection:
`ZEPTO_DRAIN_THREADS`, `ZEPTO_RING_BUFFER_CAPACITY`.

When `TickPlant queue full! Dropping tick seq=…` appears in logs,
raise `pipeline.ringBufferCapacity` first (power of two), then
`pipeline.drainThreads`. A non-power-of-two or out-of-range capacity
causes the pod to fail fast at startup with a clear
`std::invalid_argument` in the crash log.

This is a **single-pod vertical** scaling knob. Horizontal scale-out
— a stateless `zepto_ingest_node` tier plus an ingest-rate HPA — is
Phase 2 and is tracked in `docs/BACKLOG.md` under **P8 — Cluster**.

---

### Sizing and placement for enterprise factory workloads

Horizontal scale-out only delivers linear ingest gain when each replica lands on a
**distinct node**. The Helm chart defaults enforce this via hard podAntiAffinity plus
topologySpread; see `docs/devlog/104_pod_placement_hardening.md` for the root-cause
analysis. Use this table as a starting point and right-size per sector.

| Sector | Replicas | Nodes | `resources.requests` (cpu / memory) | `podAntiAffinity.required` |
|--------|----------|-------|-------------------------------------|----------------------------|
| Dev / sandbox | 2 | 1 is OK | 1c / 2 Gi | `false` |
| Small IoT pilot | 3 | 3 | 2c / 4 Gi *(default)* | `true` |
| Auto factory | 5 | 5 | 4c / 8 Gi | `true` |
| Semi fab (CMP / lithography) | 10 | 10 | 8c / 16 Gi | `true` |

#### Why `required: true` is the production default

A soft `preferredDuringSchedulingIgnoredDuringExecution` lets Kubernetes co-locate
two pods on the same node as soon as HPA scales `replicas > nodes`. When that happens
the two ZeptoDB processes fight for the same CPU, halving ingest throughput, and a
single node failure takes down both replicas at once — breaking the scale-out
guarantee silently.

Flip to `required: false` only on dev clusters where a tight fixed node count makes
co-location acceptable (e.g. a 3-replica chart on a 1-node kind cluster). Everywhere
else, leave it on and rely on EKS Auto Mode / Karpenter to provision the Nth node
(typically 30–60 s per §5 above) when a hard antiAffinity leaves a pod `Pending`.

#### Why `topologySpread` + `maxSkew: 1` alongside

`required: true` alone refuses to schedule extras beyond the current node count.
`topologySpreadConstraints` with `maxSkew: 1` is the smarter complement when
`replicas > nodes` is a legitimate transient state (brief HPA spike ahead of node
provision, planned drain): it spreads pods as evenly as possible across hostnames
and still allows more replicas than nodes. Set
`topologySpread.whenUnsatisfiable: ScheduleAnyway` if you want the spread hint
without the scheduling block.

#### Resource sizing rules of thumb

- **CPU request** = 1 core for HTTP/RPC + `pipeline.drainThreads` cores for ingest
  draining. If `drainThreads` is `0` (auto), the engine picks `max(2, hw_concurrency / 4)`,
  so plan for 1 + 2 = 3 cores minimum on any node smaller than 16 vCPU. The 2c/4c
  default covers a 2-core drain pool plus HTTP/RPC, sized for ~200K–500K ticks/s.
- **Memory request** = ~100 MB baseline + 32 MB per active arena + `ringBufferCapacity × 64` bytes
  for the `TickPlant` ring. Example: 200 active partitions + 1 M-slot ring ≈
  100 MB + 6.4 GB + 64 MB ≈ 6.6 GB — round up to 8 GB limit for headroom. Raise
  `limits.memory` before raising `pipeline.ringBufferCapacity`.
- **Bare-metal trading (Guaranteed QoS)** — pin `requests.cpu == limits.cpu` and
  `requests.memory == limits.memory` in an overlay. Portable chart defaults do
  not request HugePages: opt in only after the target nodes have a verified
  pre-reserved pool, and add the same `hugepages-2Mi` quantity to both requests
  and limits together with `performanceTuning.hugepages.enabled=true`.

```bash
# Auto-factory profile (5 replicas × 5 nodes × 4c/8G)
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set replicaCount=5 \
  --set autoscaling.minReplicas=5 \
  --set resources.requests.cpu=4000m \
  --set resources.requests.memory=8Gi \
  --set resources.limits.cpu=8000m \
  --set resources.limits.memory=16Gi

# Dev overlay (2 replicas on 1 node, co-location OK)
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set replicaCount=2 \
  --set podAntiAffinity.required=false \
  --set resources.requests.cpu=1000m \
  --set resources.requests.memory=2Gi
```

---

## 6. Backup & Recovery

> **Release blocker:** The examples in this section can snapshot or copy PVC
> bytes, but they do not establish an application-consistent ZeptoDB backup.
> Hot-partition WAL/recovery and SQL-visible HDB merge must be completed and an
> end-to-end insert-stop-restart-query test must pass before these procedures
> can be used as a production recovery promise.

### In-Cluster Backup (CronJob)

```yaml
# deploy/k8s/backup-cronjob.yaml
apiVersion: batch/v1
kind: CronJob
metadata:
  name: zeptodb-backup
  namespace: zeptodb
spec:
  schedule: "0 2 * * *"    # Daily at 02:00 UTC
  concurrencyPolicy: Forbid
  jobTemplate:
    spec:
      template:
        spec:
          restartPolicy: OnFailure
          containers:
          - name: backup
            image: amazon/aws-cli:latest
            env:
            - name: S3_BUCKET
              value: "your-zeptodb-backups"
            - name: DATA_DIR
              value: "/opt/zeptodb/data"
            command:
            - /bin/sh
            - -c
            - |
              TIMESTAMP=$(date +%Y%m%d_%H%M%S)
              tar -czf /tmp/zeptodb-${TIMESTAMP}.tar.gz -C ${DATA_DIR} .
              aws s3 cp /tmp/zeptodb-${TIMESTAMP}.tar.gz \
                s3://${S3_BUCKET}/backups/zeptodb-${TIMESTAMP}.tar.gz \
                --storage-class STANDARD_IA
              echo "Backup completed: zeptodb-${TIMESTAMP}.tar.gz"
            volumeMounts:
            - name: data
              mountPath: /opt/zeptodb/data
              readOnly: true
          volumes:
          - name: data
            persistentVolumeClaim:
              claimName: zeptodb-data
```

```bash
kubectl apply -f deploy/k8s/backup-cronjob.yaml

# Trigger manual backup
kubectl create job --from=cronjob/zeptodb-backup zeptodb-backup-manual -n zeptodb

# Check backup status
kubectl get jobs -n zeptodb
kubectl logs job/zeptodb-backup-manual -n zeptodb
```

### PVC Snapshot (EBS)

```bash
# VolumeSnapshot (requires CSI driver)
cat <<EOF | kubectl apply -f -
apiVersion: snapshot.storage.k8s.io/v1
kind: VolumeSnapshot
metadata:
  name: zeptodb-snap-$(date +%Y%m%d)
  namespace: zeptodb
spec:
  volumeSnapshotClassName: ebs-csi-snapclass
  source:
    persistentVolumeClaimName: zeptodb-data
EOF

# Verify snapshot
kubectl get volumesnapshot -n zeptodb
```

### Recovery from Snapshot (infrastructure-only)

```bash
# Create new PVC from snapshot
cat <<EOF | kubectl apply -f -
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: zeptodb-data-restored
  namespace: zeptodb
spec:
  accessModes: [ReadWriteOnce]
  storageClassName: gp3
  resources:
    requests:
      storage: 500Gi
  dataSource:
    name: zeptodb-snap-20260324
    kind: VolumeSnapshot
    apiGroup: snapshot.storage.k8s.io
EOF

# Do not attach this restored PVC to production yet. The chart has no validated
# existing-claim restore workflow, and SQL-visible restart recovery remains a
# release blocker.
```

---

## 7. Upgrades & Rollback

> For details: [Rolling Upgrade Guide](../ops/rolling_upgrade.md)

### Standard Upgrade

```bash
# 1. Pre-flight
kubectl get pods -n zeptodb -o wide
curl -s "$ZEPTO_URL/health"

# 2. Upgrade
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set image.tag=1.1.0 \
  --wait --timeout 5m

# 3. Monitor the production cluster StatefulSet
kubectl rollout status statefulset/zeptodb -n zeptodb

# 4. Verify
curl -s "$ZEPTO_URL/health"
curl -H "Authorization: Bearer $ZEPTO_ADMIN_KEY" \
  -X POST "$ZEPTO_URL/" -d 'SELECT 1'
```

### Availability During Upgrade

Standalone mode uses `maxSurge: 0` / `maxUnavailable: 1` so a ReadWriteOnce
volume is detached before the replacement pod starts. Plan a maintenance
window; standalone upgrades are not zero-downtime. Cluster mode rolls the
StatefulSet one pod at a time. A production overlay with at least three pods,
`PDB minAvailable: 2`, healthy readiness probes, and validated replication is
required before claiming service continuity.

### Rollback

```bash
# Immediate rollback
helm rollback zeptodb -n zeptodb

# Rollback to a specific revision
helm history zeptodb -n zeptodb
helm rollback zeptodb <REVISION> -n zeptodb

# kubectl rollback (without Helm, cluster mode)
kubectl rollout undo statefulset/zeptodb -n zeptodb
```

### Canary Deployment

```bash
# 1. Canary deployment (1 replica)
helm install zeptodb-canary ./deploy/helm/zeptodb -n zeptodb \
  --set replicaCount=1 \
  --set image.tag=2.0.0 \
  --set service.type=ClusterIP \
  --set autoscaling.enabled=false \
  --set podDisruptionBudget.enabled=false

# 2. Canary testing
kubectl port-forward svc/zeptodb-canary 8124:8123 -n zeptodb
curl -X POST http://localhost:8124/ -d 'SELECT vwap(price, volume) FROM trades WHERE symbol = 1'

# 3a. Success → promote
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb --set image.tag=2.0.0 --wait
helm uninstall zeptodb-canary -n zeptodb

# 3b. Failure → remove
helm uninstall zeptodb-canary -n zeptodb
```

---

## 8. Security

### TLS Termination

```bash
# Create TLS Secret
kubectl create secret tls zeptodb-tls \
  -n zeptodb \
  --cert=/path/to/cert.pem \
  --key=/path/to/key.pem

# Ingress with TLS
cat <<EOF | kubectl apply -f -
apiVersion: networking.k8s.io/v1
kind: Ingress
metadata:
  name: zeptodb
  namespace: zeptodb
  annotations:
    nginx.ingress.kubernetes.io/backend-protocol: "HTTP"
spec:
  tls:
  - hosts:
    - zeptodb.example.com
    secretName: zeptodb-tls
  rules:
  - host: zeptodb.example.com
    http:
      paths:
      - path: /
        pathType: Prefix
        backend:
          service:
            name: zeptodb
            port:
              number: 8123
EOF
```

### API Key / JWT Secrets

```bash
# Existing API-key Secret contract. api-keys contains the hash-only key store;
# admin-api-key and metrics-api-key are the corresponding raw operator and
# Prometheus credentials. The store must contain the SHA-256 hash of each raw
# key with `admin` and `metrics` roles respectively.
kubectl create secret generic zeptodb-auth \
  -n zeptodb \
  --from-file=api-keys=/secure/path/api_keys.txt \
  --from-literal=admin-api-key="$ZEPTO_ADMIN_KEY" \
  --from-literal=metrics-api-key="$ZEPTO_METRICS_KEY"

# Cluster peer secret (minimum 32 random bytes).
openssl rand -base64 48 > /secure/path/cluster-secret
kubectl create secret generic zeptodb-cluster \
  -n zeptodb \
  --from-file=cluster-secret=/secure/path/cluster-secret

# JWT secret
kubectl create secret generic zeptodb-jwt \
  -n zeptodb \
  --from-literal=JWT_SECRET='your-jwt-secret'

# Vault integration (Secrets Store CSI)
# → SecretsProvider chain: Vault KV v2 → K8s file → env var
```

Set `auth.existingSecret=zeptodb-auth` and
`cluster.security.existingSecret=zeptodb-cluster` in production values. When
those settings are empty, the chart generates valid Secrets and preserves them
across upgrades with `lookup`; retrieve the bootstrap admin key only with the
explicit command printed by `helm install`. Secret volumes are read-only and
the process loads them only at startup. For an external API-key rotation, keep
old and new hashes in the store during overlap and change
`auth.rolloutChecksum` to force a rollout; move clients, remove the old hash,
then change the checksum again. Cluster peer secrets have no overlap mechanism:
change `cluster.security.rolloutChecksum` only during a coordinated maintenance
window because mixed old/new processes cannot authenticate.

The read-only store makes Kubernetes Secret rotation the source of truth;
runtime `/admin/keys` mutations cannot persist back into that volume. If the
deployment requires dynamic key creation/revocation, configure the documented
Vault/writable key backend instead of relying on the mounted bootstrap file.

### Network Policy

The chart enables an ingress NetworkPolicy by default. HTTP is allowed from
pods in the release namespace, while cluster RPC and heartbeat ports accept
only pods belonging to the same Helm release. To admit an approved TLS ingress
controller or Prometheus deployment from another namespace, add a narrowly
scoped NetworkPolicy peer:

```yaml
networkPolicy:
  enabled: true
  http:
    allowSameNamespace: true
    additionalSources:
      - namespaceSelector:
          matchLabels:
            kubernetes.io/metadata.name: ingress-nginx
        podSelector:
          matchLabels:
            app.kubernetes.io/name: ingress-nginx
```

Add a second peer for the actual Prometheus namespace/labels when the
ServiceMonitor scraper runs outside the release namespace. Verify that the CNI
enforces Kubernetes NetworkPolicy. The policy reduces unauthorized reachability
but does not encrypt HTTP or cluster RPC payloads; TLS termination and a private
or encrypted cluster network remain required.

### RBAC (Kubernetes)

```yaml
# Role for operators
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: zeptodb-operator
  namespace: zeptodb
rules:
- apiGroups: ["", "apps", "autoscaling"]
  resources: ["pods", "deployments", "services", "configmaps", "hpa"]
  verbs: ["get", "list", "watch", "update", "patch"]
- apiGroups: [""]
  resources: ["pods/log", "pods/exec"]
  verbs: ["get", "create"]
```

---

## 9. Cluster Mode

For operating a ZeptoDB distributed cluster on Kubernetes.

### Enable Cluster

```bash
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set cluster.enabled=true \
  --set replicaCount=3 \
  --set cluster.rpcPortOffset=100 \
  --set cluster.heartbeatPort=9100 \
  --set headless.enabled=true
```

Direct pod-to-pod communication via Headless Service:
- RPC: `<pod-name>.zeptodb-headless.zeptodb.svc:8223`
- Heartbeat: UDP `:9100`

The chart mounts one shared peer secret into every RPC-capable pod through
`ZEPTO_CLUSTER_SECRET_FILE`. The HMAC challenge-response authenticates peers
and rejects replayed handshakes; it does not encrypt RPC payloads. Keep peer
ports private and enforce a namespace/VPC network policy. Disabling
`cluster.security.enabled` is an explicit development-only choice.

### Write-path routing (devlog 111)

When cluster mode is enabled, every pod automatically wires a
`CoordinatorRoutingAdapter` so that HTTP/SQL `INSERT` statements and
Python `Pipeline.ingest_*` calls are routed to the partition owner via
the `PartitionRouter` consistent-hash ring. Without this wire-up (the
state before devlog 111), writes would land on whichever pod the Service
LoadBalancer happened to pick, silently mis-partitioning data.

Verify the routing is live by checking any pod's startup log:

```bash
kubectl logs -n zeptodb zeptodb-0 | grep -E 'Cluster routing|Peer RPC'
# Expected output:
#   Peer RPC server: port 8223
#   Cluster routing: enabled (N remote nodes)
```

**Feed consumers** (`KafkaConsumer`, `MqttConsumer`, `OpcUaConsumer`)
route through their own `set_routing()` hook, bypassing the HTTP LB
entirely — use them as the primary ingest path for production multi-pod
deployments.

**DDL replication (devlog 112).** `CREATE / DROP / ALTER TABLE` sent to
any pod is fire-and-forget replicated to every remote pod via
`QueryCoordinator::forward_ddl_to_remotes`. Per-remote failures emit
`ZEPTO_WARN` but never fail the client request, so operators should
still pre-provision critical tables at deploy time if a pod might be
unreachable at DDL time.

### Stateless ingest tier (optional)

For workloads where ingest load scales independently of query/storage
load, deploy a dedicated stateless ingest tier (P8-I3, devlog 113).
Each ingest pod runs the `zepto_ingest_node` binary, holds zero data,
and forwards every HTTP INSERT to the correct storage pod via
`CoordinatorRoutingAdapter` (same routing path as devlog 111).

Topology:

```
clients ──► ingest Service (ClusterIP) ──► N × zepto_ingest_node pods
                                                │  (owns no data,
                                                │   node_id=99999)
                                                ▼
                                            TCP RPC fan-out
                                                ▼
                                         storage StatefulSet (zeptodb-N)
                                         — owns partitions, runs queries
```

Production rendering is blocked until the ingest binary can load the shared
API-key store (or an authenticated proxy mode is implemented). The only
currently accepted Helm form is an explicit isolated benchmark exception:

```bash
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set ingest.enabled=true \
  --set ingest.noAuth=true \
  --set ingest.replicas=3 \
  --set-string 'ingest.extraArgs={--add-node,0:zeptodb-0.zeptodb-headless:8123,--add-node,1:zeptodb-1.zeptodb-headless:8123,--add-node,2:zeptodb-2.zeptodb-headless:8123}'
```

Notes:

- Storage-pod discovery is currently manual via `ingest.extraArgs`. A
  future init container will generate `--add-node` flags from the
  headless service automatically.
- `ingest.noAuth: false` is the fail-closed default and causes a clear Helm
  render failure while `ingest.enabled=true`. `ingest.noAuth: true` is an
  explicit isolated benchmark override, not a production setting.
- The chart also rejects an empty `ingest.extraArgs`; storage routes must be
  explicit until discovery is implemented.
- DDL (`CREATE / DROP / ALTER TABLE`) sent to an ingest pod replicates
  to every storage pod automatically via devlog 112's
  `forward_ddl_to_remotes`.

### Cluster Health

```bash
# Check cluster status for each pod
for pod in $(kubectl get pods -n zeptodb -l app.kubernetes.io/name=zeptodb -o name); do
  echo "--- $pod ---"
  kubectl exec -n zeptodb $pod -- curl -s http://localhost:8123/health
  echo
done
```

### Cluster Upgrade Considerations

- CoordinatorHA handles automatic re-registration
- FencingToken prevents split-brain
- Increase `gracefulShutdown` time during upgrades to ensure WAL flush

```bash
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set image.tag=1.1.0 \
  --set gracefulShutdown.preStopSleepSeconds=30 \
  --set gracefulShutdown.terminationGracePeriodSeconds=60 \
  --wait --timeout 10m
```

---

## 10. Troubleshooting

### Pod Fails to Start

```bash
# Check status
kubectl describe pod <pod> -n zeptodb

# Common causes:
# - ImagePullBackOff → Check image path/authentication
# - Pending → Insufficient resources (kubectl describe node)
# - CrashLoopBackOff → Check logs (kubectl logs --previous)
```

### Readiness Probe Failure

```bash
kubectl logs <pod> -n zeptodb | grep -i "error\|fail\|ready"

# Check directly from inside the pod
kubectl exec -n zeptodb <pod> -- curl -s http://localhost:8123/ready
```

### PVC Not Bound

```bash
kubectl describe pvc zeptodb-data -n zeptodb

# Check StorageClass
kubectl get sc
# If gp3 StorageClass does not exist, it needs to be created
```

### OOMKilled

```bash
# Check memory usage
kubectl top pods -n zeptodb

# Increase limits
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set resources.limits.memory=64Gi \
  --wait
```

### Slow Queries

```bash
# Check query plan with EXPLAIN
curl -H "Authorization: Bearer $ZEPTO_ADMIN_KEY" \
  -X POST "$ZEPTO_URL/" -d 'EXPLAIN SELECT ...'

# Check running queries via Admin API
curl -H "Authorization: Bearer $ZEPTO_ADMIN_KEY" "$ZEPTO_URL/admin/queries"

# Kill slow query
curl -X DELETE -H "Authorization: Bearer $ZEPTO_ADMIN_KEY" \
  "$ZEPTO_URL/admin/queries/<query-id>"
```

### HPA Not Scaling

This is intentional for the current release. Helm rejects
`autoscaling.enabled=true` because the StatefulSet startup peer list is static.
Do not bypass the validation; use a reviewed `replicaCount` change and verify
membership on every ordinal.

---

## 11. Runbooks

### Runbook: Emergency Restart

```bash
# 1. Record current state
kubectl get pods -n zeptodb -o wide > /tmp/zeptodb-state.txt

# 2. Cluster rolling restart. Standalone mode requires a maintenance window.
kubectl rollout restart statefulset/zeptodb -n zeptodb
kubectl rollout status statefulset/zeptodb -n zeptodb --timeout=5m

# 3. Verify
curl -s "$ZEPTO_URL/health"
curl -H "Authorization: Bearer $ZEPTO_ADMIN_KEY" \
  -X POST "$ZEPTO_URL/" -d 'SELECT 1'
```

### Runbook: Disk Full

```bash
# 1. Check
kubectl exec -n zeptodb <pod> -- df -h /opt/zeptodb/data

# 2. Clean up old HDB data (TTL setting)
curl -H "Authorization: Bearer $ZEPTO_ADMIN_KEY" -X POST "$ZEPTO_URL/" \
  -d "ALTER TABLE trades SET TTL 90 DAYS"

# 3. Expand PVC (if StorageClass has allowVolumeExpansion: true)
kubectl patch pvc zeptodb-data -n zeptodb \
  -p '{"spec":{"resources":{"requests":{"storage":"1Ti"}}}}'
```

### Runbook: Node Drain (Maintenance)

```bash
# PDB guarantees minAvailable: 2, so drain is safe
kubectl drain <node> --ignore-daemonsets --delete-emptydir-data

# After maintenance is complete
kubectl uncordon <node>
```

### Runbook: Complete Redeployment

```bash
# 1. Backup
kubectl create job --from=cronjob/zeptodb-backup zeptodb-pre-redeploy -n zeptodb
kubectl wait --for=condition=complete job/zeptodb-pre-redeploy -n zeptodb --timeout=10m

# 2. Delete
helm uninstall zeptodb -n zeptodb
# PVC is preserved (not deleted by helm uninstall)

# 3. Redeploy
helm install zeptodb ./deploy/helm/zeptodb -n zeptodb -f values-prod.yaml --wait

# 4. Verify
curl -s "$ZEPTO_URL/health"
```

---

## Quick Reference

```bash
# === Status ===
kubectl get all -n zeptodb
kubectl get hpa -n zeptodb
kubectl get pvc -n zeptodb
kubectl get events -n zeptodb --sort-by='.lastTimestamp' | tail -20

# === Logs ===
kubectl logs -f statefulset/zeptodb -n zeptodb
kubectl logs <pod> -n zeptodb --previous

# === Health ===
curl "$ZEPTO_URL/health"
curl "$ZEPTO_URL/ready"
curl -H "Authorization: Bearer $ZEPTO_METRICS_KEY" "$ZEPTO_URL/metrics"

# === Helm ===
helm list -n zeptodb
helm history zeptodb -n zeptodb
helm get values zeptodb -n zeptodb

# === Upgrade ===
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb --set image.tag=X.Y.Z --wait
helm rollback zeptodb -n zeptodb

# === Reviewed static scale (cluster.enabled=true only) ===
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --reuse-values --set replicaCount=5
kubectl top pods -n zeptodb
```
