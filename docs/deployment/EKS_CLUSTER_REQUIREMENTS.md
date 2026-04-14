# EKS Cluster Requirements for ZeptoDB

Last updated: 2026-04-08

---

## 1. Cluster Baseline

| Item | Requirement | Notes |
|------|-------------|-------|
| Kubernetes version | **1.35** (latest standard support) | |
| Mode | **EKS Auto Mode** | AWS manages compute, networking, storage, LB, DNS |
| Provisioning | `eksctl` | `deploy/k8s/eks-bench-cluster.yaml` |

## 2. What EKS Auto Mode Manages (No Manual Install Needed)

| Component | Previously Required | Auto Mode |
|-----------|-------------------|-----------|
| Karpenter | Separate Helm install | Built-in |
| VPC CNI | EKS addon | Built-in |
| CoreDNS | EKS addon | Built-in |
| kube-proxy | EKS addon | Built-in |
| EBS CSI driver | EKS addon | Built-in |
| AWS Load Balancer Controller | Separate install | Built-in |
| Spot interruption handling | SQS queue + controller | Built-in |
| Node patching / AMI updates | Manual | Automatic (21-day max lifetime) |
| Pod Identity Agent | Separate install | Built-in |

## 3. Built-in Node Pools

EKS Auto Mode creates two default node pools:

| Pool | Purpose |
|------|---------|
| `system` | Cluster infrastructure (CoreDNS, etc.) |
| `general-purpose` | General workloads |

## 4. Custom Node Pools for ZeptoDB

Applied via `kubectl` after cluster creation (see `deploy/scripts/setup_eks.sh`).

### 4.1 Realtime Pool

For ingestion and low-latency queries.

```yaml
apiVersion: karpenter.sh/v1
kind: NodePool
metadata:
  name: zepto-realtime
spec:
  weight: 10
  template:
    spec:
      nodeClassRef:
        group: eks.amazonaws.com
        kind: NodeClass
        name: zepto-realtime
      requirements:
        - key: kubernetes.io/arch
          operator: In
          values: ["arm64"]
        - key: karpenter.sh/capacity-type
          operator: In
          values: ["on-demand"]
        - key: eks.amazonaws.com/instance-category
          operator: In
          values: ["c", "i"]
        - key: eks.amazonaws.com/instance-cpu
          operator: In
          values: ["4", "8", "16"]
  limits:
    cpu: "64"
    memory: 128Gi
  disruption:
    consolidationPolicy: WhenEmpty
    consolidateAfter: 30m
    budgets:
      - nodes: "1"
```

| Setting | Value | Rationale |
|---------|-------|-----------|
| Capacity | On-Demand only | No Spot interruption for trading workloads |
| Arch | arm64 (Graviton) | Best price-performance |
| Instance categories | `c` (compute), `i` (storage) | NVMe for HDB, compute for ingestion |
| Consolidation | WhenEmpty, 30m | Conservative — avoid churn during trading hours |
| Disruption budget | 1 node max | Protect availability |

### 4.2 Analytics Pool

For backtesting and batch queries.

```yaml
apiVersion: karpenter.sh/v1
kind: NodePool
metadata:
  name: zepto-analytics
spec:
  weight: 50
  template:
    spec:
      nodeClassRef:
        group: eks.amazonaws.com
        kind: NodeClass
        name: zepto-analytics
      requirements:
        - key: kubernetes.io/arch
          operator: In
          values: ["arm64"]
        - key: karpenter.sh/capacity-type
          operator: In
          values: ["spot", "on-demand"]
        - key: eks.amazonaws.com/instance-category
          operator: In
          values: ["c", "m", "r"]
        - key: eks.amazonaws.com/instance-cpu
          operator: In
          values: ["4", "8", "16", "32"]
  limits:
    cpu: "128"
    memory: 512Gi
  disruption:
    consolidationPolicy: WhenEmptyOrUnderutilized
    consolidateAfter: 5m
    budgets:
      - nodes: "20%"
```

| Setting | Value | Rationale |
|---------|-------|-----------|
| Capacity | Spot + On-Demand | Cost optimization, wide instance selection reduces interruption |
| Instance categories | `c`, `m`, `r` | Wide selection = more Spot pools = lower interruption |
| Consolidation | WhenEmptyOrUnderutilized, 5m | Aggressive — reclaim idle batch nodes fast |
| Disruption budget | 20% | Allow faster consolidation |

### 4.3 Custom NodeClasses

| NodeClass | Ephemeral Storage | IOPS | Throughput |
|-----------|------------------|------|------------|
| `zepto-realtime` | 100 Gi | 6000 | 400 MB/s |
| `zepto-analytics` | 200 Gi | default | default |

Note: EKS Auto Mode uses `eks.amazonaws.com/v1` API for NodeClass (not `karpenter.k8s.aws/v1`). Labels also use `eks.amazonaws.com/*` prefix instead of `karpenter.k8s.aws/*`.

## 5. Scaling Flow

```
Pod demand increases
  → HPA scales Deployment replicas (CPU > 70% or Memory > 80%)
  → New pods are Pending (no capacity)
  → Auto Mode detects Pending pods (built-in Karpenter)
  → EC2 Fleet API → new node in 30-60s
  → Pods scheduled on new node
```

## 6. Disruption Budget Summary

| Component | Type | Setting |
|-----------|------|---------|
| ZeptoDB pods | PDB | `minAvailable: 2` (Helm chart) |
| Realtime NodePool | Karpenter budget | max 1 node at a time |
| Analytics NodePool | Karpenter budget | max 20% nodes |
| Auto Mode default | Node lifetime | 21 days max (auto-replaced) |

## 7. Security Checklist

- [ ] API server endpoint: restrict `publicAccessCidrs` (not `0.0.0.0/0`)
- [ ] Enable private endpoint access
- [ ] Enable control plane logging (api, audit, authenticator)
- [ ] Use Pod Identity for service accounts

## 8. Cost Optimization: Sleep/Wake

For bench clusters, scale node groups to 0 when not in use:

```bash
./tools/eks-bench.sh sleep    # Scale to 0 → ~$0.10/hr (control plane only)
./tools/eks-bench.sh wake     # Restore nodes → ready in 3-5 min
./tools/eks-bench.sh status   # Check current state
```

| State | Hourly Cost | Nodes |
|-------|-------------|-------|
| Wake | ~$3.60/hr | 6 (3× r7i.2xlarge + 2× m7i.large + 1× c7i.xlarge) |
| Sleep | ~$0.10/hr | 0 (control plane only) |

## 9. Related Files

| File | Description |
|------|-------------|
| `tools/eks-bench.sh` | Sleep/wake script for cost optimization |
| `deploy/scripts/setup_eks.sh` | EKS Auto Mode setup script |
| `deploy/k8s/eks-bench-cluster.yaml` | eksctl cluster config |
| `deploy/helm/zeptodb/values.yaml` | Helm values |
| `docs/operations/KUBERNETES_OPERATIONS.md` | Day-2 operations guide |
