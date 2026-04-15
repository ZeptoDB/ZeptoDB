#!/usr/bin/env bash
# deploy/scripts/setup_eks.sh — EKS Auto Mode cluster setup for ZeptoDB
# Usage: ./setup_eks.sh [CLUSTER_NAME] [REGION]
set -euo pipefail

CLUSTER_NAME="${1:-zeptodb-prod}"
REGION="${2:-us-east-1}"

echo "=== ZeptoDB EKS Auto Mode Setup ==="
echo "Cluster: ${CLUSTER_NAME}"
echo "Region:  ${REGION}"
echo ""

# ── 1. Create cluster with Auto Mode ──────────────────────────────────────
if ! aws eks describe-cluster --name "${CLUSTER_NAME}" --region "${REGION}" &>/dev/null; then
  echo "[1/4] Creating EKS Auto Mode cluster..."
  cat > /tmp/zepto-eks-config.yaml <<EOF
apiVersion: eksctl.io/v1alpha5
kind: ClusterConfig

metadata:
  name: ${CLUSTER_NAME}
  region: ${REGION}
  version: "1.35"

autoModeConfig:
  enabled: true
  # Default node pools: general-purpose + system
  # Custom pools applied in step 2 via kubectl
EOF
  eksctl create cluster -f /tmp/zepto-eks-config.yaml
else
  echo "[1/4] Cluster ${CLUSTER_NAME} already exists, updating kubeconfig..."
  aws eks update-kubeconfig --name "${CLUSTER_NAME}" --region "${REGION}"
fi

echo "Waiting for nodes to be ready..."
kubectl wait --for=condition=Ready nodes --all --timeout=300s 2>/dev/null || true

# ── 2. Apply custom NodeClass + NodePools ──────────────────────────────────
echo "[2/4] Applying ZeptoDB custom NodeClass and NodePools..."
kubectl apply -f - <<'EOF'
---
apiVersion: eks.amazonaws.com/v1
kind: NodeClass
metadata:
  name: zepto-realtime
spec:
  ephemeralStorage:
    size: "100Gi"
    iops: 6000
    throughput: 400
  tags:
    zeptodb.com/role: realtime
---
apiVersion: eks.amazonaws.com/v1
kind: NodeClass
metadata:
  name: zepto-analytics
spec:
  ephemeralStorage:
    size: "200Gi"
  tags:
    zeptodb.com/role: analytics
---
# Realtime pool: ingestion + low-latency queries (On-Demand only)
apiVersion: karpenter.sh/v1
kind: NodePool
metadata:
  name: zepto-realtime
spec:
  weight: 10
  template:
    metadata:
      labels:
        zeptodb.com/role: realtime
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
---
# Analytics pool: backtesting + batch queries (Spot preferred)
apiVersion: karpenter.sh/v1
kind: NodePool
metadata:
  name: zepto-analytics
spec:
  weight: 50
  template:
    metadata:
      labels:
        zeptodb.com/role: analytics
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
EOF

# ── 3. Deploy ZeptoDB ──────────────────────────────────────────────────────
echo "[3/4] Deploying ZeptoDB Helm chart..."
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HELM_DIR="${SCRIPT_DIR}/../helm/zeptodb"

kubectl create namespace zeptodb 2>/dev/null || true

helm upgrade --install zeptodb "${HELM_DIR}" \
  --namespace zeptodb \
  --set image.repository=zeptodb/zeptodb \
  --set image.tag=0.0.2 \
  --set karpenter.enabled=false \
  --wait --timeout 5m

# ── 4. Verify ──────────────────────────────────────────────────────────────
echo "[4/4] Verification..."
echo ""
echo "--- Node Pools ---"
kubectl get nodepools
echo ""
echo "--- Node Classes ---"
kubectl get nodeclasses
echo ""
echo "--- ZeptoDB ---"
kubectl get pods -n zeptodb -o wide
kubectl get svc -n zeptodb
echo ""
echo "--- PDBs ---"
kubectl get pdb -A
echo ""
echo "=== Setup complete ==="
echo ""
echo "EKS Auto Mode manages: compute scaling, VPC CNI, CoreDNS, kube-proxy,"
echo "EBS CSI, load balancing, node patching, and Spot interruption handling."
echo "No separate Karpenter install needed — it's built into Auto Mode."
