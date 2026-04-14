#!/bin/bash
# Scale zepto-bench EKS node groups for cost optimization
# Usage:
#   ./eks-bench.sh sleep   — Scale all node groups to 0 (stop paying)
#   ./eks-bench.sh wake    — Scale back to operational size
#   ./eks-bench.sh status  — Show current state
set -euo pipefail

CLUSTER="zepto-bench"
REGION="ap-northeast-2"
NODEGROUPS=("system-v135" "data-nodes-v135" "loadgen-v135")

# Operational sizes: name:min:max:desired
declare -A SIZES=(
  ["system-v135"]="0:3:2"
  ["data-nodes-v135"]="0:3:3"
  ["loadgen-v135"]="0:1:1"
)

scale() {
  local ng=$1 min=$2 max=$3 desired=$4
  aws eks update-nodegroup-config \
    --cluster-name "$CLUSTER" \
    --nodegroup-name "$ng" \
    --scaling-config "minSize=$min,maxSize=$max,desiredSize=$desired" \
    --region "$REGION" \
    --query "update.status" --output text
}

case "${1:-status}" in
  sleep)
    echo "💤 Scaling all node groups to 0..."
    for ng in "${NODEGROUPS[@]}"; do
      echo -n "  $ng → 0... "
      scale "$ng" 0 3 0
    done
    echo "Done. Nodes will terminate in ~2 minutes."
    echo "Estimated savings: ~\$0/hr (was ~\$3.5/hr)"
    ;;

  wake)
    echo "☀️  Scaling node groups to operational size..."
    for ng in "${NODEGROUPS[@]}"; do
      IFS=: read -r min max desired <<< "${SIZES[$ng]}"
      echo -n "  $ng → $desired... "
      scale "$ng" "$min" "$max" "$desired"
    done
    echo "Done. Nodes will be ready in ~3-5 minutes."
    ;;

  status)
    echo "📊 zepto-bench node group status:"
    printf "  %-20s %-8s %-8s %-8s %-10s\n" "NAME" "MIN" "MAX" "DESIRED" "STATUS"
    for ng in "${NODEGROUPS[@]}"; do
      aws eks describe-nodegroup \
        --cluster-name "$CLUSTER" \
        --nodegroup-name "$ng" \
        --region "$REGION" \
        --query "nodegroup.[nodegroupName,scalingConfig.minSize,scalingConfig.maxSize,scalingConfig.desiredSize,status]" \
        --output text | while read -r name min max desired status; do
          printf "  %-20s %-8s %-8s %-8s %-10s\n" "$name" "$min" "$max" "$desired" "$status"
        done
    done
    ;;

  *)
    echo "Usage: $0 {sleep|wake|status}"
    exit 1
    ;;
esac
