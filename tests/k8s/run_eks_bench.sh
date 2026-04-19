#!/bin/bash
# tests/k8s/run_eks_bench.sh — Full EKS benchmark: wake → test amd64+arm64 → engine bench → sleep
#
# Usage:
#   ./tests/k8s/run_eks_bench.sh              # full run
#   ./tests/k8s/run_eks_bench.sh --keep       # don't sleep after tests
#   ./tests/k8s/run_eks_bench.sh --skip-wake  # cluster already running
#   ./tests/k8s/run_eks_bench.sh --k8s-only   # skip engine bench
#   ./tests/k8s/run_eks_bench.sh --engine-only # skip K8s tests
#
# Cost: ~$4.60/hr. Full run ~20min ≈ $1.50
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
EKS_BENCH="$PROJECT_ROOT/tools/eks-bench.sh"
KEEP=false
SKIP_WAKE=false
K8S_ONLY=false
ENGINE_ONLY=false
RESULT_DIR="/tmp/eks_bench_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULT_DIR"

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
step()  { echo -e "\n${CYAN}══ $* ══${NC}"; }
fail()  { echo -e "${RED}[FAIL]${NC} $*"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --keep)        KEEP=true; shift ;;
    --skip-wake)   SKIP_WAKE=true; shift ;;
    --k8s-only)    K8S_ONLY=true; shift ;;
    --engine-only) ENGINE_ONLY=true; shift ;;
    -h|--help)
      echo "Usage: $0 [--keep] [--skip-wake] [--k8s-only] [--engine-only]"
      exit 0 ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

# ── Cleanup function ──────────────────────────────────────
force_clean_namespaces() {
  for NS in zeptodb-test zeptodb-ha zeptodb-test-arm64 zeptodb-ha-arm64 karpenter-trigger; do
    REL=$(helm list -n "$NS" -q 2>/dev/null || true)
    [ -n "$REL" ] && { helm uninstall "$REL" -n "$NS" 2>/dev/null || true; }
    kubectl delete namespace "$NS" --wait=false 2>/dev/null || true
  done
  return 0
}

cleanup() {
  step "Cleanup"
  force_clean_namespaces
  kubectl delete nodepool arm64-bench 2>/dev/null || true
  kubectl delete ec2nodeclass arm64-bench 2>/dev/null || true
  if ! $KEEP; then
    step "Sleep EKS"
    "$EKS_BENCH" sleep || true
  else
    info "--keep: cluster stays running. Sleep: $EKS_BENCH sleep"
  fi
  return 0
}
trap cleanup EXIT

RC_AC=0; RC_AH=0; RC_ARM=0; RC_BENCH=0

# ═══════════════════════════════════════════════════════════
# 1. Wake EKS
# ═══════════════════════════════════════════════════════════
if ! $SKIP_WAKE && ! $ENGINE_ONLY; then
  step "1. Wake EKS cluster"
  "$EKS_BENCH" wake
  info "Waiting for nodes..."
  for i in $(seq 1 24); do
    READY=$(kubectl get nodes --no-headers 2>/dev/null | grep -c " Ready " || true)
    [ "$READY" -ge 3 ] && break
    sleep 10
  done
  kubectl get nodes -L kubernetes.io/arch --no-headers | awk '{printf "  %-55s %s\n", $1, $6}'
fi

# ═══════════════════════════════════════════════════════════
# 2. Provision arm64 nodes
# ═══════════════════════════════════════════════════════════
if ! $ENGINE_ONLY; then
  step "2. Provision arm64 nodes (Karpenter)"

  # devlog 094 #10: skip Karpenter provision when nodes already present
  # (saves 1–5 min on subsequent runs against a warm cluster).
  EXISTING_ARM=$(kubectl get nodes -l kubernetes.io/arch=arm64 --no-headers 2>/dev/null | grep -c Ready || true)
  if [ "$EXISTING_ARM" -ge 3 ]; then
    info "arm64 nodes already present ($EXISTING_ARM ready) — skipping Karpenter provision"
  else

  cat <<'KEOF' | kubectl apply -f -
apiVersion: karpenter.k8s.aws/v1
kind: EC2NodeClass
metadata:
  name: arm64-bench
spec:
  role: "KarpenterNodeRole-zepto-bench"
  amiSelectorTerms:
    - alias: al2023@latest
  subnetSelectorTerms:
    - tags:
        karpenter.sh/discovery: zepto-bench
  securityGroupSelectorTerms:
    - tags:
        karpenter.sh/discovery: zepto-bench
---
apiVersion: karpenter.sh/v1
kind: NodePool
metadata:
  name: arm64-bench
spec:
  weight: 50
  template:
    spec:
      expireAfter: 720h
      nodeClassRef:
        group: karpenter.k8s.aws
        kind: EC2NodeClass
        name: arm64-bench
      requirements:
        - key: kubernetes.io/arch
          operator: In
          values: ["arm64"]
        - key: karpenter.sh/capacity-type
          operator: In
          values: ["on-demand"]
        - key: karpenter.k8s.aws/instance-family
          operator: In
          values: ["m7g", "m6g", "c7g"]
        - key: karpenter.k8s.aws/instance-size
          operator: In
          values: ["xlarge", "2xlarge"]
  limits:
    cpu: "64"
    memory: "128Gi"
  disruption:
    consolidationPolicy: WhenEmpty
    consolidateAfter: 30m
KEOF

  # Trigger 3 arm64 pods spread across nodes
  kubectl create namespace karpenter-trigger --dry-run=client -o yaml | kubectl apply -f -
  kubectl apply -n karpenter-trigger -f - <<'TEOF'
apiVersion: apps/v1
kind: Deployment
metadata:
  name: arm64-trigger
spec:
  replicas: 3
  selector:
    matchLabels:
      app: arm64-trigger
  template:
    metadata:
      labels:
        app: arm64-trigger
    spec:
      nodeSelector:
        kubernetes.io/arch: arm64
      topologySpreadConstraints:
        - maxSkew: 1
          topologyKey: kubernetes.io/hostname
          whenUnsatisfiable: DoNotSchedule
          labelSelector:
            matchLabels:
              app: arm64-trigger
      containers:
        - name: pause
          image: public.ecr.aws/eks-distro/kubernetes/pause:3.9
          resources:
            requests:
              cpu: "500m"
              memory: "512Mi"
TEOF

  info "Waiting for 3+ arm64 nodes (max 5min)..."
  for i in $(seq 1 30); do
    ARM=$(kubectl get nodes -l kubernetes.io/arch=arm64 --no-headers 2>/dev/null | grep -c Ready || true)
    [ "$ARM" -ge 3 ] && break
    sleep 10
  done
  kubectl delete namespace karpenter-trigger --wait=false 2>/dev/null || true

  fi  # end devlog 094 #10 "skip provision when arm64 already present"

  AMD=$(kubectl get nodes -l kubernetes.io/arch=amd64 --no-headers 2>/dev/null | grep -c Ready || true)
  ARM=$(kubectl get nodes -l kubernetes.io/arch=arm64 --no-headers 2>/dev/null | grep -c Ready || true)
  info "Ready: ${AMD} amd64, ${ARM} arm64"
  [ "$AMD" -lt 3 ] || [ "$ARM" -lt 3 ] && fail "Need 3+ per arch" && exit 1
fi

# ═══════════════════════════════════════════════════════════
# 3. K8s tests — amd64 chain ‖ arm64 chain (devlog 094 #9)
# Separate namespaces → no cluster-level state contention, safe to parallelize.
# ═══════════════════════════════════════════════════════════
if ! $ENGINE_ONLY; then
  cd "$PROJECT_ROOT"

  # ── Clean any leftover releases ──
  force_clean_namespaces
  sleep 5

  step "3. K8s tests — amd64 (compat → HA+perf) ‖ arm64 (compat+HA+perf)"

  # amd64 chain
  (
    python3.13 -u tests/k8s/test_k8s_compat.py > "$RESULT_DIR/amd64_compat.log" 2>&1
    RC_AC_IN=$?
    python3.13 -u tests/k8s/test_k8s_ha_perf.py > "$RESULT_DIR/amd64_ha_perf.log" 2>&1
    RC_AH_IN=$?
    echo "$RC_AC_IN $RC_AH_IN" > "$RESULT_DIR/amd64_rc"
  ) &
  PID_AMD=$!

  # arm64 chain
  (
    python3.13 -u -c "
import sys, time, importlib; sys.path.insert(0, '$PROJECT_ROOT')

import tests.k8s.test_k8s_compat as tc
tc.NAMESPACE='zeptodb-test-arm64'; tc.RELEASE='zepto-compat-arm64'
tc.VALUES_PATH='tests/k8s/test-values-arm64.yaml'
tc.setup()
suite=tc.TestSuite()
for t in tc.ALL_TESTS:
    try: t(suite)
    except Exception as e: suite.add(tc.TestResult(t.__name__,False,str(e)))
compat_ok=suite.summary(); tc.cleanup(); time.sleep(5)

import tests.k8s.test_k8s_ha_perf as hp
importlib.reload(hp)
hp.NAMESPACE='zeptodb-ha-arm64'; hp.RELEASE='zepto-ha-arm64'
hp.VALUES_PATH='tests/k8s/test-values-arm64.yaml'
hp.setup()
suite2=hp.TestSuite()
for t in hp.HA_TESTS+hp.PERF_TESTS:
    try: t(suite2)
    except Exception as e: suite2.add(hp.TestResult(t.__name__,False,str(e)))
ha_ok=suite2.summary(); hp.cleanup()
sys.exit(0 if (compat_ok and ha_ok) else 1)
" > "$RESULT_DIR/arm64_all.log" 2>&1
    echo $? > "$RESULT_DIR/arm64_rc"
  ) &
  PID_ARM=$!

  info "forked amd64 pid=$PID_AMD, arm64 pid=$PID_ARM — waiting…"
  wait $PID_AMD || true
  wait $PID_ARM || true

  read RC_AC RC_AH < "$RESULT_DIR/amd64_rc"
  RC_ARM=$(cat "$RESULT_DIR/arm64_rc")
  info "amd64 compat: exit=$RC_AC  amd64 HA+perf: exit=$RC_AH  arm64: exit=$RC_ARM"
fi

# ═══════════════════════════════════════════════════════════
# 4. Native engine benchmarks
# ═══════════════════════════════════════════════════════════
if ! $K8S_ONLY; then
  step "4. Native engine benchmarks (SIMD, ingestion, SQL, parallel, HDB)"
  ARCH_BENCH="$PROJECT_ROOT/tests/bench/run_arch_bench.sh"
  if [ -x "$ARCH_BENCH" ]; then
    "$ARCH_BENCH" --skip-build > "$RESULT_DIR/arch_bench.log" 2>&1 && RC_BENCH=0 || RC_BENCH=$?
    info "Engine bench: exit=$RC_BENCH"
  else
    info "run_arch_bench.sh not found, skipping"
  fi
fi

# ═══════════════════════════════════════════════════════════
# 5. Report
# ═══════════════════════════════════════════════════════════
step "5. Results"

echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║          EKS Benchmark: amd64 vs arm64                  ║"
echo "╚══════════════════════════════════════════════════════════╝"

if ! $ENGINE_ONLY; then
  echo ""
  echo "── K8s Test Results ──"
  for F in amd64_compat amd64_ha_perf; do
    SUMMARY=$(grep -E "Results:|HA Test Results:" "$RESULT_DIR/${F}.log" 2>/dev/null | sed 's/\x1B\[[0-9;]*m//g' | tail -1)
    printf "  %-20s %s\n" "$F:" "$SUMMARY"
  done
  ARM_C=$(grep "Results:" "$RESULT_DIR/arm64_all.log" 2>/dev/null | sed 's/\x1B\[[0-9;]*m//g' | head -1)
  ARM_H=$(grep "HA Test Results:" "$RESULT_DIR/arm64_all.log" 2>/dev/null | sed 's/\x1B\[[0-9;]*m//g' | head -1)
  printf "  %-20s %s\n" "arm64 compat:" "$ARM_C"
  printf "  %-20s %s\n" "arm64 HA+perf:" "$ARM_H"

  echo ""
  echo "── K8s Performance: amd64 ──"
  grep -E "^\s+(drain_|pod_|scale_|rolling_|pod_to_|http_|service_)" "$RESULT_DIR/amd64_ha_perf.log" 2>/dev/null | sed 's/\x1B\[[0-9;]*m//g'
  echo ""
  echo "── K8s Performance: arm64 ──"
  grep -E "^\s+(drain_|pod_|scale_|rolling_|pod_to_|http_|service_)" "$RESULT_DIR/arm64_all.log" 2>/dev/null | sed 's/\x1B\[[0-9;]*m//g'
fi

if ! $K8S_ONLY && [ -f "$RESULT_DIR/arch_bench.log" ]; then
  echo ""
  echo "╔══════════════════════════════════════════════════════════╗"
  echo "║     Native Engine Benchmark: amd64 vs arm64             ║"
  echo "╚══════════════════════════════════════════════════════════╝"
  sed -n '/━━━/,$ p' "$RESULT_DIR/arch_bench.log"
fi

echo ""
echo "Full logs: $RESULT_DIR/"

# ═══════════════════════════════════════════════════════════
# Exit
# ═══════════════════════════════════════════════════════════
TOTAL=$((RC_AC + RC_AH + RC_ARM))
if [ $TOTAL -eq 0 ]; then
  info "ALL K8S TESTS PASSED ✅"
else
  fail "FAILURES: amd64_compat=$RC_AC amd64_ha=$RC_AH arm64=$RC_ARM"
fi
exit $TOTAL
