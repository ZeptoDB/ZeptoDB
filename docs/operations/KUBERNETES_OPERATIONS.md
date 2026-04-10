# ZeptoDB Kubernetes Operations Guide

Last updated: 2026-03-24

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
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐              │   │
│  │  │ Pod-0   │  │ Pod-1   │  │ Pod-2   │  ← Deployment│   │
│  │  │ ZeptoDB │  │ ZeptoDB │  │ ZeptoDB │    (3 replicas)│  │
│  │  │ :8123   │  │ :8123   │  │ :8123   │              │   │
│  │  └────┬────┘  └────┬────┘  └────┬────┘              │   │
│  │       │             │            │                    │   │
│  │  ┌────┴─────────────┴────────────┴────┐              │   │
│  │  │  Service (LoadBalancer :8123)       │              │   │
│  │  │  + Headless Service (pod discovery) │              │   │
│  │  └────────────────────────────────────┘              │   │
│  │                                                       │   │
│  │  ConfigMap │ PVC (gp3 500Gi) │ PDB │ HPA │ ServiceMon│   │
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
| Deployment | `deployment.yaml` | ZeptoDB pods (rolling update) |
| Service | `service.yaml` | LoadBalancer + Headless |
| ConfigMap | `configmap.yaml` | `zeptodb.conf` |
| PVC | `pvc.yaml` | gp3 500Gi persistent storage |
| HPA | `hpa.yaml` | Auto-scaling (3–10 replicas) |
| PDB | `pdb.yaml` | minAvailable: 2 |
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

### Production values override

`values-prod.yaml`:

```yaml
replicaCount: 3

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
  storageClass: gp3
  size: 500Gi

config:
  workerThreads: 8
  parallelThreshold: 100000

autoscaling:
  enabled: true
  minReplicas: 3
  maxReplicas: 10

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

# Health check
export LB=$(kubectl get svc zeptodb -n zeptodb \
  -o jsonpath='{.status.loadBalancer.ingress[0].hostname}')
curl -s http://$LB:8123/health
curl -s http://$LB:8123/ready

# Test query
curl -X POST http://$LB:8123/ -d 'SELECT 1'
```

---

## 3. Day-2 Operations

### Daily Checks

```bash
#!/bin/bash
# daily-check.sh — run from cron or manually

NS=zeptodb
LB=$(kubectl get svc zeptodb -n $NS \
  -o jsonpath='{.status.loadBalancer.ingress[0].hostname}')

echo "=== Pod Status ==="
kubectl get pods -n $NS -o wide

echo "=== Health ==="
curl -sf http://$LB:8123/health && echo " OK" || echo " FAIL"

echo "=== Readiness ==="
curl -sf http://$LB:8123/ready && echo " OK" || echo " FAIL"

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
# Full rolling restart (zero-downtime)
kubectl rollout restart deployment/zeptodb -n zeptodb

# Delete a specific pod only (Deployment auto-recreates it)
kubectl delete pod <pod-name> -n zeptodb
```

---

## 4. Monitoring & Alerting

### Prometheus Setup

```bash
# Enable ServiceMonitor (requires Prometheus Operator)
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set serviceMonitor.enabled=true \
  --set serviceMonitor.interval=15s
```

In environments without ServiceMonitor, use Pod annotation-based scraping:

```yaml
# Already included in deployment.yaml
annotations:
  prometheus.io/scrape: "true"
  prometheus.io/port: "8123"
  prometheus.io/path: "/metrics"
```

### Key Metrics

```bash
# Check directly
curl -s http://$LB:8123/metrics
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
| **zepto-realtime** | Pending pods with `zeptodb.io/role: realtime` | On-Demand only | WhenEmpty, after 30m |
| **zepto-analytics** | Pending pods with `zeptodb.io/role: analytics` | Spot + On-Demand | WhenEmptyOrUnderutilized, after 5m |

Scaling flow: HPA increases replicas → pods pending → Auto Mode provisions node (30-60s) → pods scheduled.

### Horizontal Pod Autoscaler (HPA)

Default configuration: Auto-scales between 3–10 replicas based on CPU 70% / Memory 80% thresholds.

```bash
# Check HPA status
kubectl get hpa -n zeptodb
kubectl describe hpa zeptodb -n zeptodb

# Manual scale
kubectl scale deployment zeptodb -n zeptodb --replicas=5

# Change HPA settings
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set autoscaling.minReplicas=5 \
  --set autoscaling.maxReplicas=20 \
  --set autoscaling.targetCPU=60
```

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

---

## 6. Backup & Recovery

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

### Recovery from Snapshot

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

# Replace PVC in Deployment
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set persistence.existingClaim=zeptodb-data-restored \
  --wait
```

---

## 7. Upgrades & Rollback

> For details: [Rolling Upgrade Guide](../ops/rolling_upgrade.md)

### Standard Upgrade

```bash
# 1. Pre-flight
kubectl get pods -n zeptodb -o wide
curl -s http://$LB:8123/health

# 2. Upgrade
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set image.tag=1.1.0 \
  --wait --timeout 5m

# 3. Monitor
kubectl rollout status deployment/zeptodb -n zeptodb

# 4. Verify
curl -s http://$LB:8123/health
curl -X POST http://$LB:8123/ -d 'SELECT 1'
```

### Zero-Downtime Guarantee Mechanisms

| Setting | Value | Effect |
|---------|-------|--------|
| `maxSurge` | 1 | Create 1 new pod first |
| `maxUnavailable` | 0 | Maintain existing pod count |
| `PDB minAvailable` | 2 | Guarantee minimum 2 pods |
| `preStop sleep` | 15s | Wait for in-flight queries to complete |
| `readinessProbe` | /ready | Only ready pods receive traffic |

### Rollback

```bash
# Immediate rollback
helm rollback zeptodb -n zeptodb

# Rollback to a specific revision
helm history zeptodb -n zeptodb
helm rollback zeptodb <REVISION> -n zeptodb

# kubectl rollback (without Helm)
kubectl rollout undo deployment/zeptodb -n zeptodb
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
# API keys file
kubectl create secret generic zeptodb-auth \
  -n zeptodb \
  --from-file=keys.txt=/path/to/keys.txt

# JWT secret
kubectl create secret generic zeptodb-jwt \
  -n zeptodb \
  --from-literal=JWT_SECRET='your-jwt-secret'

# Vault integration (Secrets Store CSI)
# → SecretsProvider chain: Vault KV v2 → K8s file → env var
```

### Network Policy

```yaml
# Allow access only from same namespace + monitoring
apiVersion: networking.k8s.io/v1
kind: NetworkPolicy
metadata:
  name: zeptodb-netpol
  namespace: zeptodb
spec:
  podSelector:
    matchLabels:
      app.kubernetes.io/name: zeptodb
  policyTypes: [Ingress]
  ingress:
  - from:
    - namespaceSelector:
        matchLabels:
          name: zeptodb
    - namespaceSelector:
        matchLabels:
          name: monitoring
    ports:
    - port: 8123
      protocol: TCP
```

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
  --set cluster.rpcPortOffset=100 \
  --set cluster.heartbeatPort=9100 \
  --set headless.enabled=true
```

Direct pod-to-pod communication via Headless Service:
- RPC: `<pod-name>.zeptodb-headless.zeptodb.svc:8223`
- Heartbeat: UDP `:9100`

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
curl -X POST http://$LB:8123/ -d 'EXPLAIN SELECT ...'

# Check running queries via Admin API
curl -H "Authorization: Bearer $ADMIN_KEY" http://$LB:8123/admin/queries

# Kill slow query
curl -X DELETE -H "Authorization: Bearer $ADMIN_KEY" \
  http://$LB:8123/admin/queries/<query-id>
```

### HPA Not Scaling

```bash
kubectl describe hpa zeptodb -n zeptodb

# Check metrics-server
kubectl top pods -n zeptodb
# "error: Metrics API not available" → metrics-server needs to be installed
```

---

## 11. Runbooks

### Runbook: Emergency Restart

```bash
# 1. Record current state
kubectl get pods -n zeptodb -o wide > /tmp/zeptodb-state.txt

# 2. Rolling restart (zero-downtime)
kubectl rollout restart deployment/zeptodb -n zeptodb
kubectl rollout status deployment/zeptodb -n zeptodb --timeout=5m

# 3. Verify
curl -s http://$LB:8123/health
curl -X POST http://$LB:8123/ -d 'SELECT 1'
```

### Runbook: Disk Full

```bash
# 1. Check
kubectl exec -n zeptodb <pod> -- df -h /opt/zeptodb/data

# 2. Clean up old HDB data (TTL setting)
curl -X POST http://$LB:8123/ \
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
curl -s http://$LB:8123/health
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
kubectl logs -f deployment/zeptodb -n zeptodb
kubectl logs <pod> -n zeptodb --previous

# === Health ===
curl http://$LB:8123/health
curl http://$LB:8123/ready
curl http://$LB:8123/metrics

# === Helm ===
helm list -n zeptodb
helm history zeptodb -n zeptodb
helm get values zeptodb -n zeptodb

# === Upgrade ===
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb --set image.tag=X.Y.Z --wait
helm rollback zeptodb -n zeptodb

# === Scale ===
kubectl scale deployment zeptodb -n zeptodb --replicas=5
kubectl top pods -n zeptodb
```
