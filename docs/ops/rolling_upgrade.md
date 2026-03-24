# ZeptoDB Zero-Downtime Upgrade Guide

Last updated: 2026-03-24

## Overview

ZeptoDB supports zero-downtime rolling upgrades via Helm. The strategy ensures at least `minAvailable` pods remain serving traffic at all times during an upgrade.

### How It Works

```
                    Rolling Update Flow
┌──────────────────────────────────────────────────┐
│  Pod-0 (v1.0) ──serving──┐                       │
│  Pod-1 (v1.0) ──serving──┼── LB ── clients       │
│  Pod-2 (v1.0) ──serving──┘                       │
│                                                   │
│  1. Pod-2 gets preStop (sleep 15s, drain)         │
│  2. Pod-2 removed from Service endpoints          │
│  3. Pod-2 terminated, Pod-2' (v1.1) starts        │
│  4. Pod-2' passes readiness → added to LB         │
│  5. Repeat for Pod-1, then Pod-0                  │
└──────────────────────────────────────────────────┘
```

Key settings that make this safe:
- `maxSurge: 1, maxUnavailable: 0` — never fewer pods than current count
- `PodDisruptionBudget: minAvailable: 2` — k8s won't evict below 2
- `preStop: sleep 15` — in-flight queries finish before pod dies
- `readinessProbe` — new pod only gets traffic after `/ready` returns 200

---

## Standard Upgrade (Image Tag Change)

```bash
# 1. Pre-flight: verify current state
helm list -n zeptodb
kubectl get pods -n zeptodb -o wide
curl -s http://<LB>:8123/health

# 2. Upgrade
helm upgrade zeptodb ./helm/zeptodb \
  -n zeptodb \
  --set image.tag=1.1.0 \
  --wait --timeout 5m

# 3. Monitor rollout
kubectl rollout status deployment/zeptodb -n zeptodb --timeout=5m

# 4. Verify
kubectl get pods -n zeptodb -o wide
curl -s http://<LB>:8123/health
curl -X POST http://<LB>:8123/ -d 'SELECT 1'
```

## Config-Only Upgrade (No Image Change)

ConfigMap changes trigger a rollout automatically via the `checksum/config` annotation.

```bash
helm upgrade zeptodb ./helm/zeptodb \
  -n zeptodb \
  --set config.workerThreads=16 \
  --wait
```

## Canary Upgrade (High-Risk Changes)

For major version bumps or schema changes, use a canary approach:

```bash
# 1. Deploy canary (1 replica with new version)
helm install zeptodb-canary ./helm/zeptodb \
  -n zeptodb \
  --set replicaCount=1 \
  --set image.tag=2.0.0 \
  --set service.type=ClusterIP \
  --set autoscaling.enabled=false \
  --set podDisruptionBudget.enabled=false

# 2. Test canary directly
kubectl port-forward svc/zeptodb-canary 8124:8123 -n zeptodb
curl -X POST http://localhost:8124/ -d 'SELECT vwap(price, volume) FROM trades WHERE symbol = 1'

# 3. If OK, promote
helm upgrade zeptodb ./helm/zeptodb -n zeptodb --set image.tag=2.0.0 --wait
helm uninstall zeptodb-canary -n zeptodb

# 3b. If NOT OK, rollback canary
helm uninstall zeptodb-canary -n zeptodb
```

---

## Rollback

```bash
# Instant rollback to previous revision
helm rollback zeptodb -n zeptodb

# Rollback to specific revision
helm history zeptodb -n zeptodb
helm rollback zeptodb <REVISION> -n zeptodb

# Monitor
kubectl rollout status deployment/zeptodb -n zeptodb
```

---

## Cluster Mode Upgrade

When `cluster.enabled: true`, extra care is needed for distributed state.

```bash
# 1. Check cluster health before upgrade
curl -s http://<LB>:8123/health | jq .

# 2. Pause ingestion (if possible) to reduce in-flight state
#    Or rely on WAL replay for consistency

# 3. Upgrade with extended grace period
helm upgrade zeptodb ./helm/zeptodb \
  -n zeptodb \
  --set image.tag=1.1.0 \
  --set gracefulShutdown.preStopSleepSeconds=30 \
  --set gracefulShutdown.terminationGracePeriodSeconds=60 \
  --wait --timeout 10m

# 4. Verify cluster re-formation
#    Nodes re-register via CoordinatorHA auto re-registration
curl -s http://<LB>:8123/health
```

---

## Pre-Upgrade Checklist

- [ ] Current deployment healthy (`/health` returns 200 on all pods)
- [ ] HDB snapshot taken (backup before upgrade)
- [ ] New image tested locally or in staging
- [ ] `helm diff` reviewed (if helm-diff plugin installed)
- [ ] Monitoring dashboard open (Grafana)
- [ ] Rollback plan confirmed (`helm rollback` ready)

```bash
# Optional: preview changes
helm diff upgrade zeptodb ./helm/zeptodb -n zeptodb --set image.tag=1.1.0
```

## Troubleshooting

### Pod stuck in Pending
```bash
kubectl describe pod <pod> -n zeptodb
# Common: insufficient resources, PVC not bound
```

### Readiness probe failing on new pod
```bash
kubectl logs <pod> -n zeptodb
curl http://<pod-ip>:8123/ready
# Check if new version has startup issues
# Rollback: helm rollback zeptodb -n zeptodb
```

### Rollout stuck (deadline exceeded)
```bash
kubectl rollout status deployment/zeptodb -n zeptodb
# If stuck > 5 min:
kubectl rollout undo deployment/zeptodb -n zeptodb
```
