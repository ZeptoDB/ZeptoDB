#!/bin/bash
# =============================================================================
# run_arch_comparison.sh — Build & benchmark x86 vs ARM64, compare results
# =============================================================================
# Usage:
#   ./deploy/scripts/run_arch_comparison.sh [--scenario SCENARIO] [--skip-build]
#
# Flow:
#   1. Sync code to Graviton instance
#   2. Build x86 Docker image locally, arm64 natively on Graviton
#   3. Push both to ECR
#   4. Wake EKS, run x86 benchmark on managed node group
#   5. Run arm64 benchmark on Karpenter arm64 nodes
#   6. Compare results, sleep EKS
# =============================================================================
set -euo pipefail

REGION="ap-northeast-2"
ECR="060795905711.dkr.ecr.${REGION}.amazonaws.com/zeptodb"
GRAVITON_HOST="172.31.71.135"
GRAVITON_KEY="$HOME/ec2-jinmp.pem"
SSH_CMD="ssh -i $GRAVITON_KEY -o StrictHostKeyChecking=no ec2-user@$GRAVITON_HOST"
SCP_CMD="scp -i $GRAVITON_KEY -o StrictHostKeyChecking=no"
SCENARIO="all"
SKIP_BUILD=false
SYMBOLS=50
TICKS=5000
QPS=10
BASELINE_SEC=15

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
step()  { echo -e "\n${CYAN}══════════════════════════════════════════${NC}"; echo -e "${CYAN}  $*${NC}"; echo -e "${CYAN}══════════════════════════════════════════${NC}"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --scenario)    SCENARIO="$2"; shift 2 ;;
        --skip-build)  SKIP_BUILD=true; shift ;;
        --symbols)     SYMBOLS="$2"; shift 2 ;;
        --ticks)       TICKS="$2"; shift 2 ;;
        --baseline)    BASELINE_SEC="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--scenario NAME] [--skip-build] [--symbols N] [--ticks N] [--baseline N]"
            echo "  --scenario   all|basic|add_remove_cycle|pause_resume|heavy_query|back_to_back|status_polling"
            echo "  --skip-build Skip Docker image builds (use existing ECR images)"
            exit 0 ;;
        *)             echo "Unknown: $1"; exit 1 ;;
    esac
done

BENCH_ARGS="--symbols $SYMBOLS --ticks-per-sec $TICKS --query-qps $QPS --baseline-sec $BASELINE_SEC --action remove_node --node-id 2 --ingest-threads 2 --scenario $SCENARIO"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="/tmp/arch_comparison_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

info "Results will be saved to: $RESULT_DIR"
info "Scenario: $SCENARIO | Symbols: $SYMBOLS | Ticks/sec: $TICKS"

# ═══════════════════════════════════════════════════════════════
# Step 1: Sync code to Graviton
# ═══════════════════════════════════════════════════════════════
step "Step 1/8: Sync code to Graviton ($GRAVITON_HOST)"
rsync -az --delete \
    -e "ssh -i $GRAVITON_KEY -o StrictHostKeyChecking=no" \
    --exclude='build/' --exclude='.git/' --exclude='node_modules/' \
    --exclude='web/out/' --exclude='web/.next/' --exclude='__pycache__/' \
    /home/ec2-user/zeptodb/ \
    ec2-user@${GRAVITON_HOST}:~/zeptodb/
info "Code synced"

# ═══════════════════════════════════════════════════════════════
# Step 2: Build Docker images
# ═══════════════════════════════════════════════════════════════
if [ "$SKIP_BUILD" = false ]; then
    step "Step 2/8: Build Docker images"

    # ECR login (local)
    aws ecr get-login-password --region $REGION | \
        docker login --username AWS --password-stdin "${ECR%%/*}" 2>&1 | tail -1

    # x86_64 image (local build)
    info "Building x86_64 image (local)..."
    cd /home/ec2-user/zeptodb
    docker build -f deploy/docker/Dockerfile -t "${ECR}:bench-x86" . 2>&1 | tail -3
    docker push "${ECR}:bench-x86" 2>&1 | tail -3
    info "x86_64 image pushed → ${ECR}:bench-x86"

    # arm64 image (native build on Graviton)
    info "Building arm64 image on Graviton (native, fast)..."
    $SSH_CMD "aws ecr get-login-password --region $REGION | \
        sudo docker login --username AWS --password-stdin ${ECR%%/*} 2>&1 | tail -1"

    # Graviton Dockerfile uses -mcpu=neoverse-n1 instead of -march=x86-64-v3
    $SSH_CMD "cd ~/zeptodb && \
        sed 's/-march=x86-64-v3 -mtune=generic/-mcpu=neoverse-n1/' deploy/docker/Dockerfile > /tmp/Dockerfile.arm64 && \
        sudo docker build -f /tmp/Dockerfile.arm64 -t ${ECR}:bench-arm64 . 2>&1 | tail -5 && \
        sudo docker push ${ECR}:bench-arm64 2>&1 | tail -3"
    info "arm64 image pushed → ${ECR}:bench-arm64"

    # Build bench_rebalance binaries
    info "Building bench_rebalance (x86_64)..."
    cd /home/ec2-user/zeptodb/build && ninja -j$(nproc) bench_rebalance 2>&1 | tail -2

    info "Building bench_rebalance (arm64 on Graviton)..."
    $SSH_CMD "cd ~/zeptodb && mkdir -p build && cd build && \
        cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
            -DZEPTO_USE_S3=OFF -DZEPTO_USE_PARQUET=OFF -DZEPTO_USE_FLIGHT=OFF \
            -DZEPTO_BUILD_PYTHON=OFF -DAPEX_USE_RDMA=OFF -DZEPTO_USE_UCX=OFF \
            -DZEPTO_USE_JIT=ON -DAPEX_USE_IO_URING=ON 2>&1 | tail -3 && \
        ninja -j\$(nproc) bench_rebalance 2>&1 | tail -3"
else
    info "Skipping builds (--skip-build)"
fi

# Fetch binaries
cp /home/ec2-user/zeptodb/build/bench_rebalance "$RESULT_DIR/bench_rebalance_x86"
$SCP_CMD ec2-user@${GRAVITON_HOST}:~/zeptodb/build/bench_rebalance "$RESULT_DIR/bench_rebalance_arm64"
info "Bench binaries ready"

# ═══════════════════════════════════════════════════════════════
# Helper: deploy loadgen + run benchmark
# ═══════════════════════════════════════════════════════════════
run_bench() {
    local ARCH=$1 BENCH_BIN=$2
    local RESULT_FILE="$RESULT_DIR/result_${ARCH}.txt"

    info "Deploying loadgen pod..."
    kubectl delete pod bench-loadgen -n zeptodb --force 2>/dev/null || true
    sleep 3
    cat <<EOF | kubectl apply -f -
apiVersion: v1
kind: Pod
metadata:
  name: bench-loadgen
  namespace: zeptodb
spec:
  nodeSelector:
    role: loadgen
  containers:
    - name: loadgen
      image: "debian:bookworm-slim"
      command: ["/bin/sleep", "infinity"]
      resources:
        requests: { cpu: "2", memory: "4Gi" }
  restartPolicy: Never
EOF
    kubectl wait --for=condition=Ready pod/bench-loadgen -n zeptodb --timeout=120s 2>&1

    kubectl exec -n zeptodb bench-loadgen -- \
        bash -c "apt-get update -qq && apt-get install -y -qq libssl3 2>&1" | tail -1
    kubectl cp "$BENCH_BIN" zeptodb/bench-loadgen:/tmp/bench_rebalance 2>&1
    kubectl exec -n zeptodb bench-loadgen -- chmod +x /tmp/bench_rebalance

    info "Running $ARCH benchmark..."
    kubectl exec -n zeptodb bench-loadgen -- /tmp/bench_rebalance \
        --host zeptodb.zeptodb.svc.cluster.local --port 8123 \
        $BENCH_ARGS \
        2>&1 | tee "$RESULT_FILE"

    kubectl delete pod bench-loadgen -n zeptodb --force 2>/dev/null || true
    info "$ARCH results → $RESULT_FILE"
}

# ═══════════════════════════════════════════════════════════════
# Step 3: Wake EKS
# ═══════════════════════════════════════════════════════════════
step "Step 3/8: Wake EKS cluster"
cd /home/ec2-user/zeptodb
./tools/eks-bench.sh wake 2>&1
info "Waiting for nodes (120s)..."
sleep 120
kubectl get nodes 2>&1

# ═══════════════════════════════════════════════════════════════
# Step 4: x86_64 benchmark
# ═══════════════════════════════════════════════════════════════
step "Step 4/8: Deploy x86_64 image & benchmark"
helm upgrade zeptodb /home/ec2-user/zeptodb/deploy/helm/zeptodb -n zeptodb \
    --set image.repository="$ECR" --set image.tag=bench-x86 \
    --set image.pullPolicy=Always --set nodeSelector.role=data-node \
    --set replicaCount=3 --set cluster.enabled=true \
    --set cluster.rpcPortOffset=100 --set cluster.heartbeatPort=9100 \
    --set config.workerThreads=6 --set config.parallelThreshold=50000 \
    --set config.rebalanceEnabled=true --set persistence.enabled=false \
    --set autoscaling.enabled=false --set karpenter.enabled=false \
    --set resources.requests.cpu=2 --set resources.requests.memory=8Gi \
    --set resources.limits.cpu=4 --set resources.limits.memory=16Gi \
    --set performanceTuning.hugepages.enabled=false \
    --set resources.limits."hugepages-2Mi"=0 --set resources.requests."hugepages-2Mi"=0 \
    --set podDisruptionBudget.enabled=true --set podDisruptionBudget.minAvailable=2 \
    --set service.type=LoadBalancer --set service.port=8123 --set service.sessionAffinity=None \
    2>&1 | tail -3

kubectl rollout status statefulset/zeptodb -n zeptodb --timeout=5m 2>&1 || true
sleep 10
kubectl get pods -n zeptodb 2>&1

run_bench "x86_64" "$RESULT_DIR/bench_rebalance_x86"

# ═══════════════════════════════════════════════════════════════
# Step 5: Switch to ARM64 NodePool
# ═══════════════════════════════════════════════════════════════
step "Step 5/8: Switch to ARM64 (Karpenter)"

# Ensure Karpenter NodePool is arm64
cat <<'NPEOF' | kubectl apply -f -
apiVersion: karpenter.sh/v1
kind: NodePool
metadata:
  name: zeptodb-analytics
spec:
  weight: 50
  template:
    metadata:
      labels:
        zeptodb.com/role: analytics
    spec:
      expireAfter: 720h
      nodeClassRef:
        group: karpenter.k8s.aws
        kind: EC2NodeClass
        name: zeptodb-analytics
      requirements:
        - key: kubernetes.io/arch
          operator: In
          values: ["arm64"]
        - key: karpenter.sh/capacity-type
          operator: In
          values: ["spot", "on-demand"]
        - key: karpenter.k8s.aws/instance-family
          operator: In
          values: ["r7g", "r6g", "m7g", "m6g", "c7g", "c6g"]
        - key: karpenter.k8s.aws/instance-size
          operator: In
          values: ["xlarge", "2xlarge", "4xlarge"]
  limits:
    cpu: "128"
    memory: "512Gi"
  disruption:
    consolidationPolicy: WhenEmptyOrUnderutilized
    consolidateAfter: 5m
    budgets:
      - nodes: "20%"
NPEOF

# ═══════════════════════════════════════════════════════════════
# Step 6: Deploy ARM64 image & benchmark
# ═══════════════════════════════════════════════════════════════
step "Step 6/8: Deploy arm64 image & benchmark"
helm upgrade zeptodb /home/ec2-user/zeptodb/deploy/helm/zeptodb -n zeptodb \
    --set image.repository="$ECR" --set image.tag=bench-arm64 \
    --set image.pullPolicy=Always \
    --set nodeSelector."zeptodb\.io/role"=analytics \
    --set replicaCount=3 --set cluster.enabled=true \
    --set cluster.rpcPortOffset=100 --set cluster.heartbeatPort=9100 \
    --set config.workerThreads=6 --set config.parallelThreshold=50000 \
    --set config.rebalanceEnabled=true --set persistence.enabled=false \
    --set autoscaling.enabled=false --set karpenter.enabled=true \
    --set karpenter.nodeClass.clusterName=zepto-bench \
    --set karpenter.nodeClass.role=KarpenterNodeRole-zepto-bench \
    --set resources.requests.cpu=2 --set resources.requests.memory=8Gi \
    --set resources.limits.cpu=4 --set resources.limits.memory=16Gi \
    --set performanceTuning.hugepages.enabled=false \
    --set resources.limits."hugepages-2Mi"=0 --set resources.requests."hugepages-2Mi"=0 \
    --set podDisruptionBudget.enabled=true --set podDisruptionBudget.minAvailable=2 \
    --set service.type=LoadBalancer --set service.port=8123 --set service.sessionAffinity=None \
    2>&1 | tail -3

info "Waiting for Karpenter to provision arm64 nodes (180s)..."
sleep 180
kubectl get nodes 2>&1
kubectl get pods -n zeptodb 2>&1

run_bench "arm64" "$RESULT_DIR/bench_rebalance_arm64"

# ═══════════════════════════════════════════════════════════════
# Step 7: Compare results
# ═══════════════════════════════════════════════════════════════
step "Step 7/8: Comparison"
echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║     Architecture Comparison: x86_64 vs ARM64 Graviton   ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

for ARCH in x86_64 arm64; do
    echo "── $ARCH ──"
    RFILE="$RESULT_DIR/result_${ARCH}.txt"
    if [ -f "$RFILE" ]; then
        grep -E "FINAL RESULT|ticks/sec\)|Query latency:|Data loss:|PASS|FAIL" "$RFILE" | head -30
    else
        echo "  (no results)"
    fi
    echo ""
done

echo "Full results: $RESULT_DIR/"
echo ""

# ═══════════════════════════════════════════════════════════════
# Step 8: Sleep EKS
# ═══════════════════════════════════════════════════════════════
step "Step 8/8: Sleep EKS cluster"
kubectl delete pod bench-loadgen -n zeptodb --force 2>/dev/null || true
./tools/eks-bench.sh sleep 2>&1

info "All done! Results in $RESULT_DIR/"
