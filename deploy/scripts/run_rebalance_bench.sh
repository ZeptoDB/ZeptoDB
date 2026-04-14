#!/bin/bash
# Run live rebalancing load test on EKS
# Prerequisites: EKS cluster running, Helm chart deployed with bench-rebalance-values.yaml
# Usage: ./deploy/scripts/run_rebalance_bench.sh [--action add_node|remove_node] [--node-id N]
set -euo pipefail

NAMESPACE="zeptodb"
LOADGEN_POD="bench-loadgen"
SVC="zeptodb.${NAMESPACE}.svc.cluster.local"
PORT=8123
BENCH_BINARY="build/bench_rebalance"
ACTION_VAL="add_node"
NODE_ID_VAL="0"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --action)    ACTION_VAL="$2"; shift 2 ;;
        --node-id)   NODE_ID_VAL="$2"; shift 2 ;;
        *)           echo "Usage: $0 [--action add_node|remove_node] [--node-id N]"; exit 1 ;;
    esac
done

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

# 1. Check cluster is ready
info "Checking ZeptoDB pods..."
kubectl get pods -n "$NAMESPACE" -l app.kubernetes.io/name=zeptodb -o wide
READY=$(kubectl get pods -n "$NAMESPACE" -l app.kubernetes.io/name=zeptodb \
    --field-selector=status.phase=Running --no-headers | wc -l)
if [ "$READY" -lt 3 ]; then
    error "Expected at least 3 running pods, found $READY"
fi
info "$READY data pods running"

# Check loadgen pod
kubectl get pod -n "$NAMESPACE" "$LOADGEN_POD" > /dev/null 2>&1 || \
    error "Loadgen pod '$LOADGEN_POD' not found. Deploy with: kubectl apply -f deploy/k8s/bench-loadgen.yaml"
info "Loadgen pod ready"

# 2. Build bench_rebalance if needed
if [ ! -f "$BENCH_BINARY" ]; then
    info "Building bench_rebalance..."
    cd build && cmake .. -G Ninja -DZEPTO_BUILD_BENCH=ON && ninja -j"$(nproc)" bench_rebalance && cd ..
fi

# 3. Copy binary to loadgen pod
info "Copying bench_rebalance to loadgen pod..."
kubectl cp "$BENCH_BINARY" "${NAMESPACE}/${LOADGEN_POD}:/tmp/bench_rebalance"
kubectl exec -n "$NAMESPACE" "$LOADGEN_POD" -- chmod +x /tmp/bench_rebalance

# 4. Run benchmark
info "Starting rebalance load test..."
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_FILE="/tmp/rebalance_bench_${TIMESTAMP}.log"

kubectl exec -n "$NAMESPACE" "$LOADGEN_POD" -- \
    /tmp/bench_rebalance \
        --host "$SVC" --port "$PORT" \
        --symbols 100 --ticks-per-sec 10000 --query-qps 10 \
        --baseline-sec 30 \
        --action "$ACTION_VAL" --node-id "$NODE_ID_VAL" \
    | tee "$RESULT_FILE"

EXIT_CODE=${PIPESTATUS[0]}

# 5. Collect results
info "Results saved to: $RESULT_FILE"

if [ "$EXIT_CODE" -eq 0 ]; then
    info "Benchmark PASSED"
else
    warn "Benchmark FAILED (exit code: $EXIT_CODE)"
fi

# Reminder
echo ""
warn "Remember to sleep the EKS cluster when done: ./tools/eks-bench.sh sleep"

exit "$EXIT_CODE"
