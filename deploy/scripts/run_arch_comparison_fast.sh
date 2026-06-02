#!/bin/bash
# =============================================================================
# run_arch_comparison_fast.sh — Fast parallel cross-arch pipeline
# =============================================================================
# Replaces run_arch_comparison.sh (sequential, ~60min). Target: ~28min cold,
# ~$1.30/run, fail-fast on local before touching cloud.
#
# 8 stages: preflight → 0 local smoke → 1 build || 2 cluster prep →
#            3 helm install → 4 remote smoke → 5 benchmark → 6 compare → 7 teardown
# =============================================================================
set -euo pipefail

# ── Config ───────────────────────────────────────────────────────────────────
REGION="ap-northeast-2"
CLUSTER="zepto-bench"
ACCOUNT="060795905711"
ECR_REPO="${ACCOUNT}.dkr.ecr.${REGION}.amazonaws.com/zeptodb"
GRAVITON_HOST="${GRAVITON_HOST:-172.31.71.135}"
GRAVITON_KEY="${GRAVITON_KEY:-$HOME/ec2-jinmp.pem}"
SSH="ssh -i $GRAVITON_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=5 ec2-user@$GRAVITON_HOST"
BENCH_LICENSE_FILE="${ZEPTODB_LICENSE_FILE:-keys/bench.license}"

# ── Flags ────────────────────────────────────────────────────────────────────
SCENARIO="all"
SKIP_LOCAL=false
SKIP_BUILD=false
SKIP_REMOTE_SMOKE=false
SKIP_BENCHMARK=false
ARROW_SMOKE=false
DRY_RUN=false
SPOT_ARM64=false
SYMBOLS=50
TICKS=5000
BASELINE=15
REBALANCE_TIMEOUT=120
BENCH_TIMEOUT=900
while [[ $# -gt 0 ]]; do
  case "$1" in
    --scenario)    SCENARIO="$2"; shift 2 ;;
    --skip-local)  SKIP_LOCAL=true; shift ;;
    --skip-build)  SKIP_BUILD=true; shift ;;
    --skip-remote-smoke) SKIP_REMOTE_SMOKE=true; shift ;;
    --skip-benchmark) SKIP_BENCHMARK=true; shift ;;
    --arrow-smoke) ARROW_SMOKE=true; shift ;;
    --dry-run)     DRY_RUN=true; shift ;;
    --spot-arm64)  SPOT_ARM64=true; shift ;;
    --symbols)     SYMBOLS="$2"; shift 2 ;;
    --ticks)       TICKS="$2"; shift 2 ;;
    --baseline)    BASELINE="$2"; shift 2 ;;
    --rebalance-timeout) REBALANCE_TIMEOUT="$2"; shift 2 ;;
    --bench-timeout) BENCH_TIMEOUT="$2"; shift 2 ;;
    -h|--help)
      cat <<EOF
Usage: $0 [options]
  --scenario NAME    all|smoke|basic|add_remove_cycle|pause_resume|heavy_query|back_to_back|status_polling (default: all)
  --skip-local       Skip Stage 0 (local smoke)
  --skip-build       Skip Stage 1 (Docker builds) and preflight SSH check
  --skip-remote-smoke Skip Graviton-host Stage 0 smoke (use when Graviton host offline)
  --skip-benchmark   Stop after Stage 4 remote smoke
  --arrow-smoke      Also POST an Arrow IPC payload to /insert/arrow on both archs
  --dry-run          Stop after Stage 2 (cluster prep) — no helm install, no benchmark
  --spot-arm64       Allow spot for arm64 NodePool
  --symbols N        Benchmark symbols (default: 50)
  --ticks N          Ticks/sec (default: 5000)
  --baseline N       Baseline seconds (default: 15)
  --rebalance-timeout N
                     Seconds bench_rebalance waits for one rebalance (default: 120)
  --bench-timeout N  Hard timeout per scenario in seconds (default: 900)
EOF
      exit 0 ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="/tmp/arch_fast_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; CYN='\033[0;36m'; NC='\033[0m'
info() { echo -e "${GRN}[INFO]${NC} $*"; }
warn() { echo -e "${YLW}[WARN]${NC} $*"; }
err()  { echo -e "${RED}[ERR]${NC} $*" >&2; }
step() { echo -e "\n${CYN}══ $* ══${NC}"; }

# ── Teardown (always runs) ───────────────────────────────────────────────────
cleanup() {
  local ec=$?
  step "Stage 7: Teardown"
  helm uninstall zeptodb-x86   -n zeptodb-x86   2>/dev/null || true
  helm uninstall zeptodb-arm64 -n zeptodb-arm64 2>/dev/null || true
  kubectl delete ns zeptodb-x86 zeptodb-arm64 --wait=false 2>/dev/null || true
  kubectl patch nodepool zepto-bench-x86   --type=merge -p '{"spec":{"limits":{"cpu":"0"}}}' 2>/dev/null || true
  kubectl patch nodepool zepto-bench-arm64 --type=merge -p '{"spec":{"limits":{"cpu":"0"}}}' 2>/dev/null || true
  kubectl get nodeclaims -o name 2>/dev/null \
    | grep -E 'nodeclaim.karpenter.sh/(zepto-bench-x86|zepto-bench-arm64)-' \
    | xargs -r kubectl delete --wait=false 2>/dev/null || true

  local leak_x86 leak_arm
  for _ in $(seq 1 18); do
    leak_x86=$(kubectl get nodes -l zeptodb.com/role=bench-x86   --no-headers 2>/dev/null | wc -l || echo 0)
    leak_arm=$(kubectl get nodes -l zeptodb.com/role=bench-arm64 --no-headers 2>/dev/null | wc -l || echo 0)
    [[ "$leak_x86" -eq 0 && "$leak_arm" -eq 0 ]] && break
    sleep 5
  done
  if [[ "$leak_x86" -gt 0 || "$leak_arm" -gt 0 ]]; then
    err "Nodes still running (x86=$leak_x86 arm64=$leak_arm) — cost leak! Run: kubectl get nodes -l zeptodb.com/role"
    ec=1
  fi
  info "Results: $RESULT_DIR"
  exit "$ec"
}
trap cleanup EXIT

# ── Preflight ────────────────────────────────────────────────────────────────
step "Preflight"
aws sts get-caller-identity --region "$REGION" >/dev/null || { err "AWS creds"; exit 1; }
[[ "$(aws configure get region 2>/dev/null || echo $REGION)" == "$REGION" ]] || warn "AWS region mismatch"
kubectl config current-context | grep -q "$CLUSTER" || { err "kubectl ctx != $CLUSTER"; exit 1; }
aws eks describe-cluster --name "$CLUSTER" --region "$REGION" \
  --query 'cluster.status' --output text | grep -q ACTIVE || { err "cluster not ACTIVE"; exit 1; }
if [[ "$SKIP_BUILD" == false ]]; then
  $SSH "echo ok" >/dev/null || { err "SSH to $GRAVITON_HOST failed"; exit 1; }
fi
aws ecr describe-repositories --repository-names zeptodb --region "$REGION" >/dev/null 2>&1 || \
  aws ecr create-repository --repository-name zeptodb --region "$REGION" >/dev/null
info "Preflight OK"

# ═══════════════════════════════════════════════════════════════════════════
# Stage 0 — Local smoke (Docker-free)
# ═══════════════════════════════════════════════════════════════════════════
if [[ "$SKIP_LOCAL" == false ]]; then
  step "Stage 0: Local smoke"
  cd "$(dirname "$0")/../../build"

  # 0-remote: Graviton smoke in parallel (skipped if SSH unavailable or flagged off)
  REMOTE_PID=""
  if [[ "$SKIP_BUILD" == false && "$SKIP_REMOTE_SMOKE" == false ]]; then
    stage0_remote() {
      rsync -az --delete -e "ssh -i $GRAVITON_KEY -o StrictHostKeyChecking=no" \
        --exclude=build/ --exclude=.git/ --exclude=node_modules/ --exclude=web/out/ --exclude=web/.next/ \
        "$(dirname "$0")/../../" "ec2-user@$GRAVITON_HOST:~/zeptodb/"
      $SSH bash -s <<'REMOTE'
set -euo pipefail
sudo install -d -o "$(id -u)" -g "$(id -g)" /var/log/zeptodb 2>/dev/null || mkdir -p /var/log/zeptodb 2>/dev/null || true
cd ~/zeptodb && mkdir -p build && cd build
if [ ! -f CMakeCache.txt ] || ! grep -q 'ZEPTO_BUILD_BENCH:BOOL=ON' CMakeCache.txt; then
  cmake -DZEPTO_BUILD_BENCH=ON .. >/dev/null
fi
ninja -j"$(nproc)" zepto_tests zepto_http_server zepto_data_node bench_rebalance
./tests/zepto_tests --gtest_brief=1
./zepto_http_server --port 18123 --no-auth >/tmp/smoke_http.log 2>&1 &
H=$!; trap 'kill $H 2>/dev/null||true' EXIT
for i in $(seq 1 10); do curl -sf http://127.0.0.1:18123/ping >/dev/null && break; sleep 1; [ $i = 10 ] && exit 1; done
curl -sf "http://127.0.0.1:18123/?query=INSERT+INTO+trades+VALUES+(1,100,10,1000)" >/dev/null
curl -sf "http://127.0.0.1:18123/?query=SELECT+COUNT(*)+FROM+trades+FORMAT+TSV" >/dev/null
curl -sf http://127.0.0.1:18123/health >/dev/null
kill $H 2>/dev/null||true; wait $H 2>/dev/null||true
./zepto_http_server --port 18123 --no-auth --add-node 2:127.0.0.1:18124 --add-node 3:127.0.0.1:18125 >/tmp/n1.log 2>&1 &
N1=$!; ./zepto_data_node 18124 --node-id 2 >/tmp/n2.log 2>&1 & N2=$!
./zepto_data_node 18125 --node-id 3 >/tmp/n3.log 2>&1 & N3=$!
trap 'kill $N1 $N2 $N3 2>/dev/null||true' EXIT
tcp_probe() { (exec 3<>/dev/tcp/127.0.0.1/"$1") 2>/dev/null && exec 3>&- 3<&-; }
for i in $(seq 1 15); do
  curl -sf http://127.0.0.1:18123/health >/dev/null 2>&1 && tcp_probe 18124 && tcp_probe 18125 && break
  sleep 1; [ $i = 15 ] && exit 1
done
./bench_rebalance --host 127.0.0.1 --port 18123 --symbols 10 --ticks-per-sec 1000 --baseline-sec 3 --scenario smoke --rebalance-timeout-sec 5 2>&1 | grep -qE "smoke: PASS|FINAL RESULT"
kill $N1 $N2 $N3 2>/dev/null||true; wait 2>/dev/null||true
REMOTE
    }
    stage0_remote > "$RESULT_DIR/stage0_remote.log" 2>&1 &
    REMOTE_PID=$!
  else
    info "Stage 0 remote (Graviton) skipped"
  fi

  # 0a. Build
  if ! grep -q 'ZEPTO_BUILD_BENCH:BOOL=ON' CMakeCache.txt 2>/dev/null; then
    cmake -DZEPTO_BUILD_BENCH=ON . >/dev/null
  fi
  ninja -j"$(nproc)" zepto_tests zepto_http_server zepto_data_node bench_rebalance

  # 0b. Unit tests
  ./tests/zepto_tests --gtest_brief=1

  # 0c. Single HTTP server smoke
  ./zepto_http_server --port 18123 --no-auth >"$RESULT_DIR/smoke_http.log" 2>&1 &
  HTTP_PID=$!
  trap 'kill $HTTP_PID 2>/dev/null || true; cleanup' EXIT
  for i in $(seq 1 10); do
    curl -sf http://127.0.0.1:18123/ping >/dev/null && break
    sleep 1
    [[ $i == 10 ]] && { err "HTTP /ping timeout"; exit 1; }
  done
  curl -sf "http://127.0.0.1:18123/?query=INSERT+INTO+trades+VALUES+(1,100,10,1000)" >/dev/null
  COUNT=$(curl -sf "http://127.0.0.1:18123/?query=SELECT+COUNT(*)+FROM+trades+FORMAT+TSV" | head -1)
  [[ -n "$COUNT" ]] || { err "SELECT COUNT failed"; exit 1; }
  curl -sf http://127.0.0.1:18123/health >/dev/null || { err "/health failed"; exit 1; }
  kill $HTTP_PID 2>/dev/null || true
  wait $HTTP_PID 2>/dev/null || true
  trap cleanup EXIT

  # 0d. 3-node cluster smoke
  ./zepto_http_server --port 18123 --no-auth \
    --add-node 2:127.0.0.1:18124 --add-node 3:127.0.0.1:18125 \
    >"$RESULT_DIR/smoke_node1.log" 2>&1 &
  N1=$!
  ./zepto_data_node 18124 --node-id 2 >"$RESULT_DIR/smoke_node2.log" 2>&1 &
  N2=$!
  ./zepto_data_node 18125 --node-id 3 >"$RESULT_DIR/smoke_node3.log" 2>&1 &
  N3=$!
  trap 'kill $N1 $N2 $N3 2>/dev/null || true; cleanup' EXIT
  tcp_probe() { (exec 3<>/dev/tcp/127.0.0.1/"$1") 2>/dev/null && exec 3>&- 3<&-; }
  for i in $(seq 1 15); do
    curl -sf http://127.0.0.1:18123/health >/dev/null 2>&1 && \
    tcp_probe 18124 && tcp_probe 18125 && break
    sleep 1
    [[ $i == 15 ]] && { err "cluster smoke: not all nodes up"; exit 1; }
  done
  ./bench_rebalance --host 127.0.0.1 --port 18123 \
    --symbols 10 --ticks-per-sec 1000 --baseline-sec 3 --scenario smoke --rebalance-timeout-sec 5 \
    2>&1 | tee "$RESULT_DIR/smoke_bench.log" | grep -qE "smoke: PASS|FINAL RESULT" \
    || { err "bench_rebalance smoke produced no output"; exit 1; }
  # The loopback 3-node setup verifies baseline ingest/query wiring only. Live
  # rebalance scenarios need a cluster topology that can actually accept the
  # requested add/remove action.
  kill $N1 $N2 $N3 2>/dev/null || true
  wait 2>/dev/null || true
  trap cleanup EXIT
  if [[ -n "$REMOTE_PID" ]]; then
    wait "$REMOTE_PID" || { err "Stage 0 remote (Graviton) failed — see $RESULT_DIR/stage0_remote.log"; exit 1; }
    info "Stage 0 OK (x86_64 + aarch64)"
  else
    info "Stage 0 OK"
  fi
  cd - >/dev/null
else
  info "Stage 0 skipped (--skip-local)"
fi

# ═══════════════════════════════════════════════════════════════════════════
# Stage 2 launched in background (parallel to Stage 1) — cluster prep
# ═══════════════════════════════════════════════════════════════════════════
stage2_cluster_prep() {
  local arm_cap='["on-demand"]'
  [[ "$SPOT_ARM64" == true ]] && arm_cap='["spot","on-demand"]'

  cat <<EOF | kubectl apply -f - >/dev/null
apiVersion: karpenter.sh/v1
kind: NodePool
metadata:
  name: zepto-bench-x86
spec:
  template:
    metadata:
      labels:
        zeptodb.com/role: bench-x86
    spec:
      requirements:
        - {key: kubernetes.io/arch,                operator: In, values: ["amd64"]}
        - {key: karpenter.sh/capacity-type,        operator: In, values: ["on-demand"]}
        - {key: eks.amazonaws.com/instance-family, operator: In, values: ["c7i","m7i","r7i","c6i","m6i"]}
        - {key: eks.amazonaws.com/instance-cpu,    operator: Gt, values: ["2"]}
        - {key: eks.amazonaws.com/instance-cpu,    operator: Lt, values: ["17"]}
      nodeClassRef:
        group: eks.amazonaws.com
        kind: NodeClass
        name: default
  limits:
    cpu: "64"
  disruption:
    consolidationPolicy: WhenEmpty
    consolidateAfter: 5m
---
apiVersion: karpenter.sh/v1
kind: NodePool
metadata:
  name: zepto-bench-arm64
spec:
  template:
    metadata:
      labels:
        zeptodb.com/role: bench-arm64
    spec:
      requirements:
        - {key: kubernetes.io/arch,                operator: In, values: ["arm64"]}
        - {key: karpenter.sh/capacity-type,        operator: In, values: $arm_cap}
        - {key: eks.amazonaws.com/instance-family, operator: In, values: ["c7g","m7g","r7g"]}
        - {key: eks.amazonaws.com/instance-cpu,    operator: Gt, values: ["2"]}
        - {key: eks.amazonaws.com/instance-cpu,    operator: Lt, values: ["17"]}
      nodeClassRef:
        group: eks.amazonaws.com
        kind: NodeClass
        name: default
  limits:
    cpu: "64"
  disruption:
    consolidationPolicy: WhenEmpty
    consolidateAfter: 5m
EOF

  kubectl create ns zeptodb-x86   --dry-run=client -o yaml | kubectl apply -f - >/dev/null
  kubectl create ns zeptodb-arm64 --dry-run=client -o yaml | kubectl apply -f - >/dev/null
  kubectl wait nodepool/zepto-bench-x86   --for=condition=Ready --timeout=180s 2>/dev/null || true
  kubectl wait nodepool/zepto-bench-arm64 --for=condition=Ready --timeout=180s 2>/dev/null || true
}

# ═══════════════════════════════════════════════════════════════════════════
# Stage 1 — Parallel Docker builds
# ═══════════════════════════════════════════════════════════════════════════
stage1_build_x86() {
  aws ecr get-login-password --region "$REGION" \
    | docker login --username AWS --password-stdin "${ECR_REPO%%/*}" >/dev/null
  docker buildx build --platform=linux/amd64 \
    -f deploy/docker/Dockerfile.bench \
    -t "${ECR_REPO}:bench-x86" --push .
}
stage1_build_arm64() {
  rsync -az --delete -e "ssh -i $GRAVITON_KEY -o StrictHostKeyChecking=no" \
    --exclude=build/ --exclude=.git/ --exclude=node_modules/ --exclude=web/out/ --exclude=web/.next/ \
    "$(dirname "$0")/../../" "ec2-user@$GRAVITON_HOST:~/zeptodb/"
  $SSH "aws ecr get-login-password --region $REGION | sudo docker login --username AWS --password-stdin ${ECR_REPO%%/*} >/dev/null && \
        cd ~/zeptodb && sudo docker build -f deploy/docker/Dockerfile.bench.arm64 -t ${ECR_REPO}:bench-arm64 . && \
        sudo docker push ${ECR_REPO}:bench-arm64"
}

step "Stage 1+2: Parallel build + cluster prep"
cd "$(dirname "$0")/../.."
stage2_cluster_prep &
S2=$!
if [[ "$SKIP_BUILD" == false ]]; then
  stage1_build_x86 > "$RESULT_DIR/build_x86.log" 2>&1 &
  B1=$!
  stage1_build_arm64 > "$RESULT_DIR/build_arm64.log" 2>&1 &
  B2=$!
  # fail-fast: if any of the three fails, kill the rest
  FAILED=0
  for _ in 1 2 3; do
    if ! wait -n; then
      FAILED=1
      kill "$S2" "$B1" "$B2" 2>/dev/null || true
      break
    fi
  done
  [[ "$FAILED" == 1 ]] && { err "Stage 1/2 failed (see $RESULT_DIR/*.log)"; exit 1; }
else
  wait "$S2"
  info "Stage 1 skipped (--skip-build)"
fi
info "Stage 1+2 OK"

if [[ "$DRY_RUN" == true ]]; then
  info "Dry run — stopping after Stage 2"
  exit 0
fi

# ═══════════════════════════════════════════════════════════════════════════
# Stage 3 — Parallel Helm install
# ═══════════════════════════════════════════════════════════════════════════
helm_install() {
  local arch="$1" ns="zeptodb-$1" tag="bench-$1"
  [[ "$arch" == "x86" ]] && tag="bench-x86" || tag="bench-arm64"

  local license_args=()
  if [[ -r "$BENCH_LICENSE_FILE" ]]; then
    kubectl create secret generic zeptodb-bench-license \
      --from-file=license-key="$BENCH_LICENSE_FILE" \
      -n "$ns" --dry-run=client -o yaml | kubectl apply -f - >/dev/null
    license_args=(
      --set extraEnv[0].name=ZEPTODB_LICENSE_KEY
      --set extraEnv[0].valueFrom.secretKeyRef.name=zeptodb-bench-license
      --set extraEnv[0].valueFrom.secretKeyRef.key=license-key
    )
  elif [[ "$SKIP_BENCHMARK" == false ]]; then
    warn "Bench license not found at $BENCH_LICENSE_FILE; Stage 5 rebalance scenarios will fail license gating"
  fi

  helm upgrade --install "zeptodb-${arch}" deploy/helm/zeptodb -n "$ns" \
    --set image.repository="$ECR_REPO" --set image.tag="$tag" --set image.pullPolicy=Always \
    --set nodeSelector."zeptodb\.com/role"="bench-${arch}" \
    --set replicaCount=3 \
    --set cluster.enabled=true \
    --set persistence.enabled=false \
    --set autoscaling.enabled=false \
    --set karpenter.enabled=false \
    --set performanceTuning.hugepages.enabled=false \
    --set resources.requests.cpu=2 --set resources.requests.memory=8Gi \
    --set resources.limits.cpu=4   --set resources.limits.memory=16Gi \
    --set resources.requests."hugepages-2Mi"=0 --set resources.limits."hugepages-2Mi"=0 \
    --set service.type=ClusterIP --set service.port=8123 \
    --set podDisruptionBudget.enabled=false \
    "${license_args[@]}" \
    >/dev/null
  kubectl wait --for=condition=Ready pod -l app.kubernetes.io/name=zeptodb -n "$ns" --timeout=300s
}

step "Stage 3: Parallel Helm install"
helm_install x86   > "$RESULT_DIR/helm_x86.log"   2>&1 &
H1=$!
helm_install arm64 > "$RESULT_DIR/helm_arm64.log" 2>&1 &
H2=$!
for _ in 1 2; do
  wait -n || { kill "$H1" "$H2" 2>/dev/null || true; err "Helm failed"; exit 1; }
done
info "Stage 3 OK"

# ═══════════════════════════════════════════════════════════════════════════
# Stage 4+5 — bench-loadgen pod per namespace, smoke + benchmark
# ═══════════════════════════════════════════════════════════════════════════
deploy_loadgen() {
  local arch="$1" ns="zeptodb-$1" tag
  [[ "$arch" == "x86" ]] && tag="bench-x86" || tag="bench-arm64"
  cat <<EOF | kubectl apply -f - >/dev/null
apiVersion: v1
kind: Pod
metadata: {name: bench-loadgen, namespace: $ns}
spec:
  nodeSelector: {zeptodb.com/role: bench-$arch}
  containers:
    - name: loadgen
      image: "${ECR_REPO}:${tag}"
      command: ["/bin/sleep","infinity"]
      resources: {requests: {cpu: "2", memory: "4Gi"}}
  restartPolicy: Never
EOF
  kubectl wait --for=condition=Ready pod/bench-loadgen -n "$ns" --timeout=180s
}

kubectl_exec_retry() {
  local ns="$1"; shift
  local attempt
  for attempt in 1 2 3 4 5 6; do
    if kubectl exec -n "$ns" bench-loadgen -- "$@"; then
      return 0
    fi
    warn "kubectl exec failed in $ns (attempt $attempt/6), retrying..."
    sleep $((attempt * 5))
  done
  return 1
}

remote_smoke() {
  local ns="$1" host="${1}.${1}.svc.cluster.local"
  kubectl_exec_retry "$ns" sh -c "
    set -e
    curl -sf http://$host:8123/health >/dev/null
    t=\$(date +%s%N); curl -sf http://$host:8123/ping >/dev/null; t=\$((\$(date +%s%N) - t))
    [ \$t -lt 100000000 ] || { echo '/ping >100ms'; exit 1; }
    curl -sf \"http://$host:8123/?query=INSERT+INTO+trades+VALUES+(1,100,10,1000)\" >/dev/null
    for i in \$(seq 2 10); do curl -sf \"http://$host:8123/?query=INSERT+INTO+trades+VALUES+(\$i,100,10,1000)\" >/dev/null; done
    raw=\$(curl -sf \"http://$host:8123/?query=SELECT+COUNT(*)+FROM+trades+FORMAT+TSV\" | head -1)
    n=\$(printf '%s\n' \"\$raw\" | sed -n 's/.*\"data\":\[\[\([0-9][0-9]*\)\]\].*/\1/p')
    [ -n \"\$n\" ] || n=\"\$raw\"
    [ \"\$n\" -ge 1 ] || { echo \"insert roundtrip failed: \$n\"; exit 1; }
  "
}

make_arrow_payload() {
  local out="$1"
  python3 - "$out" <<'PY'
import sys
import pyarrow as pa
import pyarrow.ipc as ipc

out = sys.argv[1]
table = pa.table({
    "symbol": pa.array(["EKS_ARROW_X86", "EKS_ARROW_ARM64", "EKS_ARROW_BOTH"], type=pa.string()),
    "price": pa.array([101.25, 202.50, 303.75], type=pa.float64()),
    "volume": pa.array([10, 20, 30], type=pa.int64()),
    "timestamp": pa.array(
        [1700000000000000001, 1700000000000000002, 1700000000000000003],
        type=pa.timestamp("ns"),
    ),
    "msg_type": pa.array([0, 0, 0], type=pa.int64()),
})
with pa.OSFile(out, "wb") as sink:
    with ipc.new_stream(sink, table.schema) as writer:
        writer.write_table(table)
PY
}

arrow_smoke() {
  local ns="$1" host="${1}.${1}.svc.cluster.local" payload="$2"
  kubectl cp "$payload" "$ns/bench-loadgen:/tmp/zepto_arrow_ingest.arrow" >/dev/null
  kubectl_exec_retry "$ns" sh -c "
    set -e
    resp=\$(curl -sf -X POST \
      -H 'Content-Type: application/vnd.apache.arrow.stream' \
      --data-binary @/tmp/zepto_arrow_ingest.arrow \
      \"http://$host:8123/insert/arrow?table=trades&sym_col=symbol&price_scale=100\")
    printf '%s\n' \"\$resp\" | grep -q '\"inserted\":3'
  "
}

SCENARIOS_ALL=(basic add_remove_cycle pause_resume heavy_query back_to_back status_polling)
if [[ "$SCENARIO" == "all" ]]; then
  SCENARIOS=("${SCENARIOS_ALL[@]}")
else
  SCENARIOS=("$SCENARIO")
fi

run_bench() {
  local arch="$1" ns="zeptodb-$1" host="zeptodb-${1}.zeptodb-${1}.svc.cluster.local"
  for sc in "${SCENARIOS[@]}"; do
    local attempt
    for attempt in 1 2 3 4 5 6; do
      if kubectl exec -n "$ns" bench-loadgen -- timeout "${BENCH_TIMEOUT}s" /opt/zeptodb/bench_rebalance \
        --host "$host" --port 8123 \
        --symbols "$SYMBOLS" --ticks-per-sec "$TICKS" --baseline-sec "$BASELINE" \
        --rebalance-timeout-sec "$REBALANCE_TIMEOUT" \
        --scenario "$sc" \
        2>&1 | tee "$RESULT_DIR/result_${arch}_${sc}.txt"; then
        break
      fi
      if [[ "$attempt" == 6 ]]; then
        return 1
      fi
      warn "benchmark exec failed in $ns for scenario $sc (attempt $attempt/6), retrying..."
      sleep $((attempt * 5))
    done
  done
}

step "Stage 4: Parallel remote smoke"
deploy_loadgen x86   &
deploy_loadgen arm64 &
wait
remote_smoke zeptodb-x86   &
remote_smoke zeptodb-arm64 &
for _ in 1 2; do wait -n || { err "Stage 4 smoke failed"; exit 1; }; done
if [[ "$ARROW_SMOKE" == true ]]; then
  ARROW_PAYLOAD="$RESULT_DIR/arrow_ingest.arrow"
  make_arrow_payload "$ARROW_PAYLOAD"
  arrow_smoke zeptodb-x86 "$ARROW_PAYLOAD" &
  arrow_smoke zeptodb-arm64 "$ARROW_PAYLOAD" &
  for _ in 1 2; do wait -n || { err "Stage 4 Arrow smoke failed"; exit 1; }; done
  info "Stage 4 Arrow smoke OK"
fi
info "Stage 4 OK"

if [[ "$SKIP_BENCHMARK" == true ]]; then
  info "Stage 5 skipped (--skip-benchmark)"
  exit 0
fi

step "Stage 5: Parallel benchmark"
run_bench x86   > "$RESULT_DIR/bench_x86.log"   2>&1 &
R1=$!
run_bench arm64 > "$RESULT_DIR/bench_arm64.log" 2>&1 &
R2=$!
for _ in 1 2; do
  wait -n || { kill "$R1" "$R2" 2>/dev/null || true; err "Stage 5 benchmark failed"; exit 1; }
done
info "Stage 5 OK"

# ═══════════════════════════════════════════════════════════════════════════
# Stage 6 — Comparison
# ═══════════════════════════════════════════════════════════════════════════
step "Stage 6: Comparison"
SUMMARY="$RESULT_DIR/summary.md"
{
  echo "# Cross-Arch Benchmark Summary"
  echo "Date: $(date -Iseconds) | Scenario: $SCENARIO | Symbols: $SYMBOLS | Ticks/s: $TICKS"
  echo
  echo "### Build Result"
  echo "| Architecture | Result |"
  echo "|-------------|--------|"
  for a in x86 arm64; do
    if [[ "$SKIP_BUILD" == true ]]; then echo "| $a | ⏭ skipped |"
    elif [[ -s "$RESULT_DIR/build_${a}.log" ]]; then echo "| $a | ✅ PASS |"
    else echo "| $a | ❌ FAIL |"; fi
  done
  echo
  echo "### Test Result (PASS/FAIL per scenario)"
  echo "| Scenario | x86_64 | aarch64 |"
  echo "|----------|--------|---------|"
  scenario_status() {
    local file="$1"
    if [[ ! -s "$file" ]]; then
      echo "?"
    elif grep -qE "FINAL RESULT: .*FAILED|>>> .*FAIL|^[[:space:]]+.*: FAIL$" "$file"; then
      echo "FAIL"
    elif grep -qE "FINAL RESULT: [1-9][0-9]*/[1-9][0-9]* scenarios passed|>>> .*PASS|^[[:space:]]+.*: PASS$" "$file"; then
      echo "PASS"
    else
      echo "?"
    fi
  }
  for sc in "${SCENARIOS[@]}"; do
    rx="$RESULT_DIR/result_x86_${sc}.txt"; ra="$RESULT_DIR/result_arm64_${sc}.txt"
    sx=$(scenario_status "$rx")
    sa=$(scenario_status "$ra")
    echo "| $sc | $sx | $sa |"
  done
  echo
  echo "### Perf (vs baseline: 5.52M ticks/s, VWAP p50 637μs, VWAP 914M rows/s)"
  echo "| Metric | x86_64 | aarch64 | Status |"
  echo "|--------|--------|---------|--------|"
  first_line() {
    local file="$1" pattern="$2"
    grep -E "$pattern" "$file" 2>/dev/null | head -1 || true
  }
  metric_number() {
    local metric="$1" line="$2"
    if [[ "$metric" == "ticks/sec" ]]; then
      echo "$line" | sed -n 's/.*(\([0-9.][0-9.]*\) ticks\/sec.*/\1/p'
    elif [[ "$metric" == "Query latency" ]]; then
      echo "$line" | sed -n 's/.*p50=\([0-9.][0-9.]*\)ms.*/\1/p'
    else
      echo "$line" | grep -Eo '[0-9.]+' | head -1 || true
    fi
  }
  for sc in "${SCENARIOS[@]}"; do
    rx="$RESULT_DIR/result_x86_${sc}.txt"
    ra="$RESULT_DIR/result_arm64_${sc}.txt"
    for metric in "ticks/sec" "Query latency" "Data loss"; do
      if [[ "$metric" == "ticks/sec" ]]; then
        vx=$(first_line "$rx" "  Inserts: .*ticks/sec")
        va=$(first_line "$ra" "  Inserts: .*ticks/sec")
      else
        vx=$(first_line "$rx" "$metric")
        va=$(first_line "$ra" "$metric")
      fi
      [[ -z "$vx" && -z "$va" ]] && continue
      # crude >20% check: compare first number on each side
      nx=$(metric_number "$metric" "$vx")
      na=$(metric_number "$metric" "$va")
      status="?"
      if [[ -n "$nx" && -n "$na" && "$nx" != 0 && "$na" != 0 ]]; then
        status="✅"
        awk -v a="$nx" -v b="$na" 'BEGIN{r=(a>b?a/b:b/a); exit (r>1.2)?0:1}' && status="⚠️ >20%"
      fi
      echo "| $sc/$metric | ${vx//|/} | ${va//|/} | $status |"
    done
  done
  echo
  echo "### Perf Regression (vs baseline)"
  echo "| Metric | Baseline | x86_64 | arm64 | x86 status | arm64 status |"
  echo "|--------|----------|--------|-------|------------|--------------|"
  # Extract first matching number from basic scenario output; higher-is-better unless noted.
  extract() {
    local arch="$1" pattern="$2" file
    file="$RESULT_DIR/result_${arch}_basic.txt"
    [[ -s "$file" ]] || return 0
    grep -E "$pattern" "$file" 2>/dev/null | grep -Eo '[0-9.]+' | head -1 || true
  }
  status_hib() { awk -v v="$1" -v b="$2" 'BEGIN{ if(v==""||v+0==0){print"?"} else if(v+0 < 0.8*b){print"⚠️ >20%"} else {print"✅"} }'; }
  status_lib() { awk -v v="$1" -v b="$2" 'BEGIN{ if(v==""||v+0==0){print"?"} else if(v+0 > 1.2*b){print"⚠️ >20%"} else {print"✅"} }'; }
  ix=$(extract x86 "ticks/sec");   ia=$(extract arm64 "ticks/sec")
  px=$(extract x86 "Query latency"); pa=$(extract arm64 "Query latency")
  tx=$(extract x86 "rows/sec");    ta=$(extract arm64 "rows/sec")
  echo "| Ingestion (ticks/s) | 5520000 | ${ix:-?} | ${ia:-?} | $(status_hib "${ix:-0}" 5520000) | $(status_hib "${ia:-0}" 5520000) |"
  echo "| VWAP p50 (μs)       | 637     | ${px:-?} | ${pa:-?} | $(status_lib "${px:-0}" 637)     | $(status_lib "${pa:-0}" 637)     |"
  echo "| VWAP thpt (rows/s)  | 914000000 | ${tx:-?} | ${ta:-?} | $(status_hib "${tx:-0}" 914000000) | $(status_hib "${ta:-0}" 914000000) |"
} > "$SUMMARY"
cat "$SUMMARY"
info "Summary: $SUMMARY"

# Stage 7 runs via trap cleanup EXIT
