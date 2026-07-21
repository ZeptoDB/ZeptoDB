#!/bin/bash
# Run the research-only Physical AI Agent Memory replay on zepto-bench.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
EKS_BENCH="$PROJECT_ROOT/tools/eks-bench.sh"
RUN_ID="$(date +%Y%m%d%H%M%S)-$$"
SUFFIX="$(printf '%s' "$RUN_ID" | tr -cd '0-9' | tail -c 10)"
RESULT_DIR="/tmp/physical_ai_memory_eks_$RUN_ID"
OUTPUT="${1:-$PROJECT_ROOT/docs/research/results/physical_ai_agent_memory_eks_replay_024.md}"
REPORT_DRAFT="$RESULT_DIR/report.md"
AMD_NAMESPACE="zeptodb-vla-memory-amd64-$SUFFIX"
ARM_NAMESPACE="zeptodb-vla-memory-arm64-$SUFFIX"
LOCK_NAMESPACE="default"
LOCK_NAME="zeptodb-physical-ai-shared-bench-lock"
LOCK_HELD=0
BENCH_WOKEN=0

source "$SCRIPT_DIR/require_zepto_bench_context.sh"
zepto_require_bench_context
if ! command -v helm >/dev/null 2>&1; then
  echo "Required command is unavailable: helm" >&2
  exit 1
fi

if [[ -e "$OUTPUT" ]]; then
  echo "Refusing to overwrite immutable experiment result: $OUTPUT" >&2
  exit 1
fi

mkdir -p "$RESULT_DIR"

acquire_bench_lock() {
  if ! kubectl create configmap "$LOCK_NAME" -n "$LOCK_NAMESPACE" \
      --from-literal="owner=$RUN_ID" \
      --from-literal="created_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
      >/dev/null; then
    echo "Shared bench lock is already held; refusing concurrent execution" >&2
    kubectl get configmap "$LOCK_NAME" -n "$LOCK_NAMESPACE" \
      -o jsonpath='{.data}' >&2 || true
    echo >&2
    return 1
  fi
  LOCK_HELD=1
}

release_bench_lock() {
  local owner
  [[ "$LOCK_HELD" -eq 1 ]] || return 0
  owner="$(kubectl get configmap "$LOCK_NAME" -n "$LOCK_NAMESPACE" \
    -o jsonpath='{.data.owner}' 2>/dev/null)"
  if [[ "$owner" != "$RUN_ID" ]]; then
    echo "Shared bench lock ownership changed; refusing to delete it" >&2
    return 1
  fi
  kubectl delete configmap "$LOCK_NAME" -n "$LOCK_NAMESPACE" \
    --wait=true >/dev/null
  LOCK_HELD=0
}

verify_shared_bench_idle() {
  local cpu_limit namespace nodepool release stale_namespace
  for nodepool in zepto-bench-x86 zepto-bench-arm64; do
    cpu_limit="$(kubectl get nodepool "$nodepool" \
      -o jsonpath='{.spec.limits.cpu}')"
    if [[ "$cpu_limit" != "0" ]]; then
      echo "Shared bench NodePool $nodepool is busy (cpu=$cpu_limit)" >&2
      return 1
    fi
  done
  if kubectl get nodeclaim \
      -l 'karpenter.sh/nodepool in (zepto-bench-x86,zepto-bench-arm64)' \
      -o name 2>/dev/null | grep -q .; then
    echo "Shared bench NodeClaims still exist" >&2
    return 1
  fi
  for namespace in zeptodb-x86 zeptodb-arm64 zeptodb; do
    release="$(helm list -n "$namespace" -q 2>/dev/null \
      | grep '^zeptodb' || true)"
    if [[ -n "$release" ]]; then
      echo "Shared bench Helm release is active in $namespace" >&2
      return 1
    fi
    if kubectl get pod bench-loadgen -n "$namespace" \
      -o name 2>/dev/null | grep -q .; then
      echo "Shared bench load generator is active in $namespace" >&2
      return 1
    fi
  done
  stale_namespace="$(kubectl get namespace -o name 2>/dev/null \
    | grep '^namespace/zeptodb-vla-memory-' || true)"
  if [[ -n "$stale_namespace" ]]; then
    echo "Stale Physical AI memory namespace exists: $stale_namespace" >&2
    return 1
  fi
}

cleanup() {
  local cleanup_rc=0
  set +e
  kubectl delete namespace "$AMD_NAMESPACE" "$ARM_NAMESPACE" \
    --ignore-not-found=true --wait=true --timeout=180s >/dev/null 2>&1 \
    || cleanup_rc=1
  if [[ "$BENCH_WOKEN" -eq 1 ]]; then
    "$EKS_BENCH" sleep >/dev/null 2>&1 || cleanup_rc=1
  fi
  release_bench_lock || cleanup_rc=1
  set -e
  return "$cleanup_rc"
}

on_exit() {
  local original_rc=$?
  local cleanup_rc=0
  trap - EXIT INT TERM
  cleanup || cleanup_rc=$?
  if [[ "$original_rc" -ne 0 ]]; then
    exit "$original_rc"
  fi
  exit "$cleanup_rc"
}

trap on_exit EXIT INT TERM
acquire_bench_lock
verify_shared_bench_idle

"$EKS_BENCH" wake
BENCH_WOKEN=1

cd "$PROJECT_ROOT"
python3.13 -u tests/k8s/test_k8s_physical_ai_memory.py \
  --output "$REPORT_DRAFT" \
  --json-output "$RESULT_DIR/result.json" \
  --namespace-suffix "$SUFFIX" \
  2>&1 | tee "$RESULT_DIR/run.log"
cleanup
trap - EXIT INT TERM
zepto_publish_no_clobber "$REPORT_DRAFT" "$OUTPUT"

echo "Result: $OUTPUT"
echo "Raw logs: $RESULT_DIR"
