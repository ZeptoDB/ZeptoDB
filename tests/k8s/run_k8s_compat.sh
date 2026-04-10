#!/bin/bash
# tests/k8s/run_k8s_compat.sh — One-shot script: create cluster → run tests → cleanup
#
# Usage:
#   chmod +x tests/k8s/run_k8s_compat.sh
#   ./tests/k8s/run_k8s_compat.sh              # full run (create + test + delete)
#   ./tests/k8s/run_k8s_compat.sh --keep       # keep cluster after tests
#
# Prerequisites:
#   - AWS credentials with EKS/EC2/IAM/CloudFormation permissions
#   - eksctl, kubectl, helm installed
#
# Estimated cost: ~$0.30/hr (t3.xlarge x2 + EKS control plane)
# Estimated time: ~20min (cluster creation ~15min + tests ~5min)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CLUSTER_CONFIG="$SCRIPT_DIR/eks-compat-cluster.yaml"
KEEP_CLUSTER=false

[[ "${1:-}" == "--keep" ]] && KEEP_CLUSTER=true

cleanup() {
    if $KEEP_CLUSTER; then
        echo ">>> --keep flag set. Cluster preserved."
        echo "    Delete manually: eksctl delete cluster -f $CLUSTER_CONFIG --disable-nodegroup-eviction"
    else
        echo ">>> Cleaning up test resources..."
        python3.13 "$SCRIPT_DIR/test_k8s_compat.py" --cleanup 2>/dev/null || true
        echo ">>> Deleting EKS cluster (this takes ~10min)..."
        eksctl delete cluster -f "$CLUSTER_CONFIG" --disable-nodegroup-eviction --wait || true
    fi
}

trap cleanup EXIT

echo "============================================================"
echo "ZeptoDB K8s Compatibility — Full Run"
echo "============================================================"

# 1. Preflight
echo ">>> Checking tools..."
for cmd in eksctl kubectl helm python3.13 aws; do
    command -v "$cmd" >/dev/null || { echo "ERROR: $cmd not found"; exit 1; }
done
aws sts get-caller-identity >/dev/null || { echo "ERROR: AWS credentials not configured"; exit 1; }

# 2. Create cluster
echo ">>> Creating EKS cluster (takes ~15min)..."
eksctl create cluster -f "$CLUSTER_CONFIG"

echo ">>> Verifying cluster..."
kubectl cluster-info
kubectl get nodes -o wide

# 3. Create gp3 StorageClass (EKS default is gp2)
echo ">>> Creating gp3 StorageClass..."
kubectl apply -f - <<'EOF'
apiVersion: storage.k8s.io/v1
kind: StorageClass
metadata:
  name: gp3
provisioner: ebs.csi.aws.com
parameters:
  type: gp3
  fsType: ext4
volumeBindingMode: WaitForFirstConsumer
allowVolumeExpansion: true
EOF

# 4. Run tests
echo ">>> Running K8s compatibility tests..."
cd "$PROJECT_ROOT"
python3.13 tests/k8s/test_k8s_compat.py
TEST_EXIT=$?

echo ">>> Tests finished with exit code: $TEST_EXIT"
exit $TEST_EXIT
