# ZeptoDB Kubernetes Failure Scenarios & Recovery Guide

Last updated: 2026-03-24

---

## Overview

This document covers failure scenarios and auto/manual recovery procedures when operating ZeptoDB in Kubernetes cluster mode.

### ZeptoDB Built-in Protection Mechanisms

```
┌─────────────────────────────────────────────────────────────┐
│  Protection Layer                                            │
│                                                              │
│  HealthMonitor ─── heartbeat 1s ─── SUSPECT 3s ─── DEAD 10s │
│       │                                                      │
│       ▼                                                      │
│  FailoverManager ─── replica promote ─── re-replication      │
│       │                                                      │
│       ▼                                                      │
│  CoordinatorHA ─── active/standby ─── auto promotion         │
│       │                                                      │
│       ▼                                                      │
│  FencingToken ─── monotonic epoch ─── stale write rejection  │
│       │                                                      │
│       ▼                                                      │
│  WalReplicator ─── async/sync ─── RF=2 data redundancy      │
│       │                                                      │
│       ▼                                                      │
│  Auto-Snapshot ─── 60s interval ─── crash recovery           │
│       │                                                      │
│       ▼                                                      │
│  K8s Lease ─── split-brain defense ─── single leader         │
└─────────────────────────────────────────────────────────────┘
```

### Failure Detection Timeline

```
t=0s    Heartbeat stopped (pod crash / node failure)
t=3s    HealthMonitor: ACTIVE → SUSPECT
t=10s   HealthMonitor: SUSPECT → DEAD
t=10s   FailoverManager: replica → primary promotion
t=10s   FencingToken: epoch advance (stale write blocked)
t=10s   PartitionRouter: routing table updated
t=15s   K8s: readinessProbe failed → Service endpoints removed
t=30s   K8s: livenessProbe failed → pod restart
t=~60s  K8s: New pod scheduling + started + readiness passed
```

---

## Scenario 1: Data Node Pod Crash (Ingestion during)

### Situation
One data node pod is OOMKilled or crashes during data ingestion.

### Impact Scope
- Ingestion paused for symbols where this node is primary
- Possible loss of in-flight ticks (in ring buffer but not written to WAL)

### Auto Recovery Flow

```
Pod-1 crash (symbol 1,3,5 primary)
    │
    ▼
HealthMonitor: Pod-1 DEAD (10s)
    │
    ▼
FailoverManager::trigger_failover(pod-1)
    ├── Pod-2 (replica of sym 1,3) → promoted to primary
    ├── Pod-0 (replica of sym 5) → promoted to primary
    └── FencingToken::advance() → stale writes from zombie Pod-1 rejected
    │
    ▼
PartitionRouter updated → queries routed to new primary
    │
    ▼
K8s Deployment: new Pod-1' Scheduling (30~60s)
    │
    ▼
Pod-1' started → CoordinatorHAregistered with → replicajoins as
    │
    ▼
PartitionMigrator: new Pod-1'data to Replication (RF=2 Restore)
```

### Data Loss Scope
| Data State | Loss? |
|---|---|
| WALat Record + replicatransfer complete | ✅ Safe |
| WALat Record + replica not transferred | ✅ WAL replayas Recovery |
| Ring bufferexists only in (WAL Unwritten) | ❌ Lost (max A few ms Worth) |
| Auto-snapshot data after | ✅ snapshot replay |
| Auto-snapshot data before | ❌ max 60s Lost |

### Manual Actions
```bash
# 1. Status Check
kubectl get pods -n zeptodb -o wide
kubectl describe pod <crashed-pod> -n zeptodb

# 2. OOMKilledcase Increase memory
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set resources.limits.memory=64Gi --wait

# 3. cluster Status Check
curl -s http://$LB:8123/health | jq .

# 4. Verify data consistency
curl -X POST http://$LB:8123/ \
  -d 'SELECT symbol, count(*) FROM trades GROUP BY symbol'
```

---

## Scenario 2: Coordinator Pod Crash

### Situation
The active coordinator pod running QueryCoordinator crashes.

### Auto Recovery Flow

```
Active Coordinator crash
    │
    ▼
Standby CoordinatorHA: monitor_loop() ping failed
    │  (config.check_interval_ms Intervalwith Detection)
    ▼
Standby → ACTIVE promotion (CoordinatorHA::role_ = ACTIVE)
    │
    ├── K8s Lease acquired (split-brain prevention)
    ├── FencingToken::advance() → new epoch
    ├── registered_nodes_ node re-registration from list
    └── promotion_cb_() called → external notification
    │
    ▼
Client queries: Service LBroutes to alive pods
    │
    ▼
K8s: new pod started → standby coordinatorjoins as
```

### Impact
- Distributed queries may fail for a few seconds during promotion
- Single-node queries unaffected as data nodes handle them directly
- K8s Lease Simultaneously two coordinator active is prevented

### Manual Actions
```bash
# promotion Check
kubectl logs <standby-pod> -n zeptodb | grep -i "promotion\|active"

# Verify query normal operation
curl -X POST http://$LB:8123/ -d 'SELECT count(*) FROM trades'
```

---

## Scenario 3: Node Drain (Maintenance / K8s upgrade)

### Situation
`kubectl drain` for node maintenance. PDB guarantees minAvailable: 2.

### Flow

```
kubectl drain <node>
    │
    ▼
K8s: PDB Check → minAvailable: 2 check if satisfied
    │
    ├── satisfied → pod eviction proceed
    │     │
    │     ▼
    │   preStop: sleep 15s (in-flight query complete wait)
    │     │
    │     ▼
    │   Pod terminated → HealthMonitor DEAD → failover
    │     │
    │     ▼
    │   K8s: on another node New pod scheduling
    │
    └── not satisfied → eviction rejected (pod maintained)
```

### Precautions
- Cannot drain 2 nodes simultaneously in 3-replica setup (PDB blocks)
- HDB snapshot recommended before drain

```bash
# Safe Drain Procedure
# 1. current Status Check
kubectl get pods -n zeptodb -o wide

# 2. Snapshot trigger (possible case)
curl -X POST http://$LB:8123/admin/snapshot \
  -H "Authorization: Bearer $ADMIN_KEY"

# 3. Drain
kubectl drain <node> --ignore-daemonsets --delete-emptydir-data

# 4. Verify new pod is healthy
kubectl rollout status deployment/zeptodb -n zeptodb

# 5. Uncordon
kubectl uncordon <node>
```

---

## Scenario 4: Split-Brain (Network Partition)

### Situation
Network partition disrupts pod-to-pod communication. Risk of both sides claiming primary.

### Defense Mechanisms

```
┌──────────────┐          ┌──────────────┐
│  Partition A  │    ✕     │  Partition B  │
│  Pod-0, Pod-1 │◄──────►│  Pod-2        │
└──────────────┘          └──────────────┘
       │                         │
       ▼                         ▼
  K8s Lease Competition            K8s Lease Competition
       │                         │
       ▼                         ▼
  Lease Acquired (majority)     Lease failed
  → ACTIVE maintained             → STANDBY demoted
       │                         │
       ▼                         ▼
  FencingToken: epoch=5     epoch=4 (stale)
  → Write allowed               → Write rejected
```

**Triple Defense:**

| Layer | Mechanism | Effect |
|-------|-----------|--------|
| K8s Lease | Single leader guarantee | only one coordinator Role |
| FencingToken | monotonic epoch | stale epoch's WAL/tick rejected |
| HealthMonitor | SUSPECT → DEAD | Marks minority partition nodes as DEAD |

### After Network Recovery
```bash
# Verify partition recovery
kubectl get pods -n zeptodb -o wide

# Check all pod health
for pod in $(kubectl get pods -n zeptodb -o name); do
  echo "--- $pod ---"
  kubectl exec -n zeptodb $pod -- curl -s localhost:8123/health
done

# Verify data consistency (compare row counts from both sides)
curl -X POST http://$LB:8123/ \
  -d 'SELECT symbol, count(*) FROM trades GROUP BY symbol ORDER BY symbol'
```

---

## Scenario 5: PVC / Storage Failure

### Situation
EBS volume failure or PVC inaccessible.

### Symptoms
- Pod CrashLoopBackOff (data dir mount failed)
- HDB flush failed logs
- Queries continue with in-memory data (only HDB queries fail)

### Recovery
```bash
# 1. PVC Status Check
kubectl describe pvc zeptodb-data -n zeptodb
kubectl get events -n zeptodb | grep -i pvc

# 2. EBS volume Status Check (AWS)
aws ec2 describe-volumes --volume-ids <vol-id>

# 3a. PVC If normal, restart pod
kubectl delete pod <pod> -n zeptodb

# 3b. PVC If corrupted, recover from snapshot
kubectl apply -f - <<EOF
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
    name: zeptodb-snap-latest
    kind: VolumeSnapshot
    apiGroup: snapshot.storage.k8s.io
EOF

# 4. Switch to new PVC
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set persistence.existingClaim=zeptodb-data-restored --wait
```

---

## Scenario 6: Rolling Upgrade during failure

### Situation
`helm upgrade` during upgrade, new version pod fails readiness probe.

### Flow

```
helm upgrade --set image.tag=1.1.0
    │
    ▼
New Pod-2' (v1.1.0) started
    │
    ▼
readinessProbe /ready failed (bug, config error etc.)
    │
    ├── maxUnavailable: 0 → existing Pod-2 (v1.0.0) maintained
    ├── rollout proceed stopped (new pod ready not so)
    └── All 3 existing pods continue serving normally
    │
    ▼
Manual intervention needed after timeout
```

### Recovery
```bash
# 1. Status Check
kubectl rollout status deployment/zeptodb -n zeptodb
kubectl get pods -n zeptodb

# 2. Check new pod logs
kubectl logs <new-pod> -n zeptodb

# 3. Immediate rollback
helm rollback zeptodb -n zeptodb

# 4. Verify rollback
kubectl rollout status deployment/zeptodb -n zeptodb
curl -s http://$LB:8123/health
```

### Safety Mechanism Summary
| Setting | Value | Role |
|---------|-------|------|
| `maxSurge` | 1 | Creates only 1 additional new pod |
| `maxUnavailable` | 0 | Never reduces existing pod count |
| `PDB minAvailable` | 2 | Guarantees minimum 2 pods serving |
| `preStop sleep` | 15s | graceful drain |

---

## Scenario 7: full cluster failure (Disaster Recovery)

### Situation
Complete K8s cluster loss (AZ failure, cluster deletion, etc.).

### Recovery Procedure

```bash
# 1. Provision new cluster
eksctl create cluster -f cluster-config.yaml

# 2. S3 from backup Check latest backup
aws s3 ls s3://your-zeptodb-backups/backups/ --recursive | tail -5

# 3. PVC Create + backup Restore
kubectl create namespace zeptodb

# Restore data from S3 using temporary pod
kubectl run restore --image=amazon/aws-cli -n zeptodb \
  --overrides='{
    "spec": {
      "containers": [{
        "name": "restore",
        "image": "amazon/aws-cli",
        "command": ["sh", "-c",
          "aws s3 cp s3://your-zeptodb-backups/backups/LATEST.tar.gz /tmp/ && tar -xzf /tmp/LATEST.tar.gz -C /data/"],
        "volumeMounts": [{"name":"data","mountPath":"/data"}]
      }],
      "volumes": [{"name":"data","persistentVolumeClaim":{"claimName":"zeptodb-data"}}],
      "restartPolicy": "Never"
    }
  }'

# 4. ZeptoDB Deploy
helm install zeptodb ./deploy/helm/zeptodb -n zeptodb -f values-prod.yaml --wait

# 5. Verify data
curl -X POST http://$LB:8123/ \
  -d 'SELECT count(*) FROM trades'
```

### RPO / RTO

| Metric | Value | Basis |
|--------|-------|------|
| RPO (Data Loss) | ≤ 60s | auto-snapshot interval |
| RPO (S3 backup) | ≤ 24h | daily backup CronJob |
| RTO (pod crash) | ~60s | K8s Auto restart |
| RTO (node failure) | ~10s | FailoverManager + replica promotion |
| RTO (full cluster) | ~30min | new cluster + S3 Restore |

---

## Scenario 8: HPA Excessive Scale-Out

### Situation
Traffic spike causes HPA to scale to 10 replicas; new pods serve with empty data.

### Impact
- New pods have no in-memory data → query results incomplete
- In cluster mode, coordinator scatters to all nodes → empty nodes also included

### Response
```bash
# 1. HPA Status Check
kubectl get hpa -n zeptodb
kubectl describe hpa zeptodb -n zeptodb

# 2. Emergency: disable HPA + set manual replica count
kubectl patch hpa zeptodb -n zeptodb -p '{"spec":{"minReplicas":3,"maxReplicas":3}}'

# 3. or new poddata to Replication complete after Service Deployment
#    (configure readinessProbe to check data load completion)
```

### Recommended Settings
```yaml
autoscaling:
  scaleUp:
    stabilizationSeconds: 120   # 2min Stabilization (Prevent rapid scaling)
  scaleDown:
    stabilizationSeconds: 300   # 5min Stabilization
```

---

## Scenario 9: Spot Instance Reclamation (Karpenter)

### Situation
Karpenter Analytics NodePool using Spot instances; AWS sends 2-minute reclamation notice.

### Flow

```
AWS: Spot stopped Notice (2min before)
    │
    ▼
Karpenter: immediately requests replacement instance (Fleet API)
    ├── Tries different AZ/size in same instance family
    ├── Falls back to On-Demand if Spot unavailable
    └── Typically new node ready within 30~60s
    │
    ▼
K8s: pod graceful termination (preStop sleep 15s)
    │
    ▼
HealthMonitor: DEAD → FailoverManager → replica promotion
    │
    ▼
Pod scheduling on new node → Service Recovery
```

### Key Points
- Realtime pool must be `on-demand` only → Spot Reclamation Impact None
- Analytics poolonly Spot allowed → batch query with retry Response possible
- Karpenter `consolidateAfter: 5m` → quickly cleans up empty nodes

### Karpenter vs Cluster Autoscaler Comparison

| | Cluster Autoscaler | Karpenter |
|---|---|---|
| Node provisioning | ASG → 2~5min | Fleet API → 30~60sec |
| Instance selection | ASG fixed type | Multiple types/AZ simultaneous request |
| Spot Reclamation Response | ASG rebalance (Slow) | Immediate replacement request |
| Node cleanup | 10~15min | consolidateAfter config |
| Workload separation | ASG multiple groups to manage | NodePool declarative separation |

---

## Scenario Summary Matrix

| # | Scenario | Detection | Auto Recovery | Data Loss | RTO |
|---|----------|------|-----------|------------|-----|
| 1 | Data node crash | HealthMonitor 10s | ✅ replica promotion | ≤ 60s (snapshot) | ~10s |
| 2 | Coordinator crash | CoordinatorHA | ✅ standby promotion | None | ~5s |
| 3 | Node drain | K8s PDB | ✅ reschedule | None | ~60s |
| 4 | Split-brain | K8s Lease + Fencing | ✅ Minority partition demotion | None | ~10s |
| 5 | Storage failure | Pod CrashLoop | ❌ Manual PVC recovery | HDBonly | ~10min |
| 6 | Bad upgrade | readiness failed | ✅ rollout stopped | None | ~30s (rollback) |
| 7 | full cluster | External monitoring | ❌ Manual DR | ≤ 24h (S3) | ~30min |
| 8 | HPA Over-scaling | Empty query results | ❌ Manual adjustment | None | ~1min |
| 9 | Spot Reclamation | AWS 2min Notice | ✅ Karpenter Replacement | ≤ 60s (snapshot) | ~60s |
