#!/bin/bash
# eks-bench.sh — Auto Mode bench cluster control
# Usage:
#   ./eks-bench.sh wake    — set NodePool limits.cpu=64 (Karpenter provisions)
#   ./eks-bench.sh sleep   — helm uninstall + NodePool limits.cpu=0 (drain)
#   ./eks-bench.sh status  — show NodePools, bench-labeled nodes, zeptodb pods
set -euo pipefail

CLUSTER="zepto-bench"
REGION="ap-northeast-2"
NODEPOOLS=(zepto-bench-x86 zepto-bench-arm64)
NAMESPACES=(zeptodb-x86 zeptodb-arm64)

patch_cpu() {
  local np=$1 cpu=$2
  kubectl patch nodepool "$np" --type=merge -p "{\"spec\":{\"limits\":{\"cpu\":\"$cpu\"}}}" 2>/dev/null \
    || echo "  (NodePool $np not found — run the fast script once to create it)"
}

case "${1:-status}" in
  wake)
    echo "☀️  Waking bench NodePools (limits.cpu=64 each)..."
    for np in "${NODEPOOLS[@]}"; do echo -n "  $np → 64... "; patch_cpu "$np" 64 && echo ok; done
    echo "Karpenter will provision nodes on demand when pods are scheduled."
    ;;
  sleep)
    echo "💤 Sleeping bench cluster..."
    for ns in "${NAMESPACES[@]}"; do
      for r in $(helm list -n "$ns" -q 2>/dev/null | grep '^zeptodb' || true); do
        echo "  helm uninstall $r -n $ns"; helm uninstall "$r" -n "$ns" 2>/dev/null || true
      done
      kubectl delete pod bench-loadgen -n "$ns" --force --grace-period=0 2>/dev/null || true
    done
    for np in "${NODEPOOLS[@]}"; do echo -n "  $np → 0... "; patch_cpu "$np" 0 && echo ok; done
    echo "Nodes drain in ~1 minute."
    ;;
  status)
    echo "📊 NodePools:"
    kubectl get nodepool "${NODEPOOLS[@]}" -o custom-columns=NAME:.metadata.name,CPU_LIMIT:.spec.limits.cpu 2>/dev/null || true
    echo
    echo "📊 Bench-labeled nodes:"
    kubectl get nodes -l 'zeptodb.com/role in (bench-x86,bench-arm64)' \
      -o custom-columns=NAME:.metadata.name,ARCH:.status.nodeInfo.architecture,ROLE:.metadata.labels.'zeptodb\.com/role' 2>/dev/null \
      || echo "  (none)"
    echo
    echo "📊 ZeptoDB pods:"
    kubectl get pods -A -l app.kubernetes.io/name=zeptodb 2>/dev/null || echo "  (none)"
    ;;
  *)
    echo "Usage: $0 {wake|sleep|status}"; exit 1 ;;
esac
