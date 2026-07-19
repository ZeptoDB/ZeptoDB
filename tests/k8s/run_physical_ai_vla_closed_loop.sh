#!/bin/bash
# Run a research-only VLA experiment on temporary EKS CPU and GPU nodes.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPERIMENT="${ZEPTO_VLA_EXPERIMENT:-027}"
PYTHON_SCRIPT="${ZEPTO_VLA_PYTHON_SCRIPT:-physical_ai_vla_closed_loop.py}"
RESULT_STEM="${ZEPTO_VLA_RESULT_STEM:-physical_ai_vla_closed_loop_027}"
DETAIL_RESULT_STEM="${ZEPTO_VLA_DETAIL_RESULT_STEM:-}"
REQUIRE_DIAGNOSTIC_ONLY="${ZEPTO_VLA_REQUIRE_DIAGNOSTIC_ONLY:-0}"
REQUIRE_TASK_MAPPING_CORRECTION="${ZEPTO_VLA_REQUIRE_TASK_MAPPING_CORRECTION:-0}"
EXTRA_ARGS="${ZEPTO_VLA_EXTRA_ARGS:-}"
REGION="ap-northeast-2"
RUN_ID="$(date +%Y%m%d%H%M%S)-$$"
SUFFIX="$(printf '%s' "$RUN_ID" | tr -cd '0-9' | tail -c 10)"
NAMESPACE="zeptodb-vla-$EXPERIMENT-$SUFFIX"
CPU_NODEPOOL="zepto-vla${EXPERIMENT}-cpu-$SUFFIX"
GPU_NODEPOOL="zepto-vla${EXPERIMENT}-gpu-$SUFFIX"
JOB="vla-experiment"
RESULT_DIR="/tmp/zeptodb_vla_${EXPERIMENT}_$RUN_ID"
RESULT_JSON="$RESULT_DIR/result.json"
SAMPLE_MANIFEST="$RESULT_DIR/sample_manifest.json"
MANIFEST_CHECKPOINT="$RESULT_DIR/manifest_checkpoint.json"
OUTPUT="${1:-$PROJECT_ROOT/docs/research/results/$RESULT_STEM.md}"
DETAIL_OUTPUT=""
DETAIL_ARG=""
if [[ -n "$DETAIL_RESULT_STEM" ]]; then
  DETAIL_OUTPUT="${2:-$PROJECT_ROOT/docs/research/results/$DETAIL_RESULT_STEM.json}"
  DETAIL_ARG="--detail-result /tmp/vla-detail.json"
fi
ZEPTO_IMAGE="${ZEPTO_EKS_AGENT_IMAGE_REPO:-060795905711.dkr.ecr.ap-northeast-2.amazonaws.com/zeptodb}:${ZEPTO_EKS_AGENT_X86_TAG:-bench-x86}"
ML_IMAGE="${ZEPTO_VLA_ML_IMAGE:-pytorch/pytorch@sha256:c16f4c749e2d9e96878875cdf6cc45cddda1d1a36fddd371dd6f2360f1b6e2a2}"
INSTANCE_IDS=()

source "$SCRIPT_DIR/require_zepto_bench_context.sh"
zepto_require_bench_context

if [[ -e "$OUTPUT" ]]; then
  echo "Refusing to overwrite immutable experiment result: $OUTPUT" >&2
  exit 1
fi
if [[ -n "$DETAIL_OUTPUT" && -e "$DETAIL_OUTPUT" ]]; then
  echo "Refusing to overwrite immutable detail artifact: $DETAIL_OUTPUT" >&2
  exit 1
fi

mkdir -p "$RESULT_DIR"

publish_no_clobber() {
  python3 - "$1" "$2" <<'PY'
import os
import sys
from pathlib import Path

source = Path(sys.argv[1])
target = Path(sys.argv[2])
target.parent.mkdir(parents=True, exist_ok=True)
temporary = target.with_name(f".{target.name}.{os.getpid()}.tmp")
try:
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(source.read_bytes())
        stream.flush()
        os.fsync(stream.fileno())
    os.link(temporary, target)
finally:
    temporary.unlink(missing_ok=True)
PY
}

verify_cluster_idle() {
  local nodepool cpu_limit namespace release stale instance_ids
  for nodepool in zepto-bench-x86 zepto-bench-arm64; do
    if ! cpu_limit="$(kubectl get nodepool "$nodepool" \
      -o jsonpath='{.spec.limits.cpu}' 2>/dev/null)"; then
      echo "Shared bench NodePool $nodepool is missing" >&2
      return 1
    fi
    if [[ "$cpu_limit" != "0" ]]; then
      echo "Shared bench NodePool $nodepool is busy (cpu=$cpu_limit)" >&2
      return 1
    fi
  done
  if kubectl get nodeclaim \
    -l 'karpenter.sh/nodepool in (zepto-bench-x86,zepto-bench-arm64)' \
    -o name 2>/dev/null | grep -q .; then
    echo "Shared bench NodeClaims are still present" >&2
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
  stale="$(kubectl get namespace -o name 2>/dev/null \
    | grep "^namespace/zeptodb-vla-$EXPERIMENT-" || true)"
  if [[ -n "$stale" ]]; then
    echo "A stale or concurrent Experiment $EXPERIMENT namespace exists" >&2
    return 1
  fi
  stale="$(kubectl get nodepool -o name 2>/dev/null \
    | grep "^nodepool.karpenter.sh/zepto-vla${EXPERIMENT}-" || true)"
  if [[ -n "$stale" ]]; then
    echo "A stale or concurrent Experiment $EXPERIMENT NodePool exists" >&2
    return 1
  fi
  if kubectl get nodes \
    -l "zeptodb.com/role in (vla-cpu-$EXPERIMENT,vla-gpu-$EXPERIMENT)" \
    -o name 2>/dev/null | grep -q .; then
    echo "A stale or concurrent Experiment $EXPERIMENT Kubernetes node exists" >&2
    return 1
  fi
  if kubectl get nodeclaims -o json 2>/dev/null \
    | jq -e --arg prefix "zepto-vla${EXPERIMENT}-" '
      [.items[]
       | select((.metadata.labels["karpenter.sh/nodepool"] // "")
         | startswith($prefix))] | length > 0
    ' >/dev/null; then
    echo "A stale or concurrent Experiment $EXPERIMENT NodeClaim exists" >&2
    return 1
  fi
  instance_ids="$(aws ec2 describe-instances --region "$REGION" \
    --filters \
      "Name=tag:eks:kubernetes-node-pool-name,Values=zepto-vla${EXPERIMENT}-*" \
      'Name=instance-state-name,Values=pending,running,stopping,stopped' \
    --query 'Reservations[].Instances[].InstanceId' --output text)"
  if [[ -n "$instance_ids" ]]; then
    echo "Active stale Experiment $EXPERIMENT EC2 instances exist: $instance_ids" >&2
    return 1
  fi
}

cleanup() {
  local cleanup_rc=0
  set +e
  if kubectl get nodeclaim \
    -l "karpenter.sh/nodepool in ($CPU_NODEPOOL,$GPU_NODEPOOL)" \
    -o json >"$RESULT_DIR/nodeclaims.json" 2>/dev/null; then
    mapfile -t INSTANCE_IDS < <(
      jq -r '.items[].status.providerID // empty | split("/")[-1]' \
        "$RESULT_DIR/nodeclaims.json"
    )
  fi
  if [[ "${#INSTANCE_IDS[@]}" -eq 0 ]]; then
    mapfile -t INSTANCE_IDS < <(
      aws ec2 describe-instances --region "$REGION" \
        --filters \
          "Name=tag:eks:kubernetes-node-pool-name,Values=$CPU_NODEPOOL,$GPU_NODEPOOL" \
        --query 'Reservations[].Instances[].InstanceId' --output text 2>/dev/null \
        | tr '\t' '\n' | sed '/^$/d'
    )
  fi
  kubectl delete namespace "$NAMESPACE" --ignore-not-found=true \
    --wait=true --timeout=180s >/dev/null 2>&1 \
    || true
  kubectl delete nodepool "$CPU_NODEPOOL" "$GPU_NODEPOOL" \
    --ignore-not-found=true --wait=true --timeout=180s >/dev/null 2>&1 \
    || true
  for _ in $(seq 1 180); do
    namespace_exists=0
    nodepool_exists=0
    nodeclaim_exists=0
    node_exists=0
    kubectl get namespace "$NAMESPACE" >/dev/null 2>&1 && namespace_exists=1
    if kubectl get nodepool "$CPU_NODEPOOL" >/dev/null 2>&1 \
      || kubectl get nodepool "$GPU_NODEPOOL" >/dev/null 2>&1; then
      nodepool_exists=1
    fi
    if kubectl get nodeclaim \
      -l "karpenter.sh/nodepool in ($CPU_NODEPOOL,$GPU_NODEPOOL)" \
      -o name 2>/dev/null | grep -q .; then
      nodeclaim_exists=1
    fi
    if kubectl get nodes \
      -l "zeptodb.com/run-id in ($SUFFIX-cpu,$SUFFIX-gpu)" \
      -o name 2>/dev/null | grep -q .; then
      node_exists=1
    fi
    if [[ "$namespace_exists" -eq 0 && "$nodepool_exists" -eq 0 \
      && "$nodeclaim_exists" -eq 0 && "$node_exists" -eq 0 ]]; then
      break
    fi
    sleep 2
  done
  if kubectl get nodeclaim \
    -l "karpenter.sh/nodepool in ($CPU_NODEPOOL,$GPU_NODEPOOL)" \
    -o name 2>/dev/null | grep -q .; then
    cleanup_rc=1
  fi
  if kubectl get namespace "$NAMESPACE" >/dev/null 2>&1; then
    cleanup_rc=1
  fi
  if kubectl get nodepool "$CPU_NODEPOOL" >/dev/null 2>&1 \
    || kubectl get nodepool "$GPU_NODEPOOL" >/dev/null 2>&1; then
    cleanup_rc=1
  fi
  if kubectl get nodes \
    -l "zeptodb.com/run-id in ($SUFFIX-cpu,$SUFFIX-gpu)" \
    -o name 2>/dev/null | grep -q .; then
    cleanup_rc=1
  fi
  for instance_id in "${INSTANCE_IDS[@]}"; do
    local terminated=0
    for _ in $(seq 1 120); do
      state="$(aws ec2 describe-instances --region "$REGION" \
        --instance-ids "$instance_id" \
        --query 'Reservations[0].Instances[0].State.Name' --output text 2>/dev/null)"
      if [[ "$state" == "terminated" || "$state" == "None" ]]; then
        terminated=1
        break
      fi
      sleep 5
    done
    if [[ "$terminated" -ne 1 ]]; then
      cleanup_rc=1
    fi
  done
  verify_cluster_idle || cleanup_rc=1
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

verify_cluster_idle

python3 "$PROJECT_ROOT/docs/research/tools/physical_ai_vla_early_exit.py" \
  --prepare-manifest "$SAMPLE_MANIFEST" \
  --manifest-checkpoint "$MANIFEST_CHECKPOINT" \
  --manifest-workers 3 \
  --rows-request-interval 1.5

configmap_bytes="$(wc -c \
  "$PROJECT_ROOT/docs/research/tools/physical_ai_vla_closed_loop.py" \
  "$PROJECT_ROOT/docs/research/tools/physical_ai_vla_skip_region.py" \
  "$PROJECT_ROOT/docs/research/tools/physical_ai_vla_trajectory_fork.py" \
  "$PROJECT_ROOT/docs/research/tools/physical_ai_vla_safety_gate.py" \
  "$PROJECT_ROOT/docs/research/tools/physical_ai_vla_risk_router.py" \
  "$PROJECT_ROOT/docs/research/tools/physical_ai_vla_calibration_failure_attribution.py" \
  "$PROJECT_ROOT/docs/research/tools/physical_ai_vla_veto_separability.py" \
  "$PROJECT_ROOT/docs/research/tools/physical_ai_vla_task_mapping_correction.py" \
  "$PROJECT_ROOT/docs/research/tools/physical_ai_vla_early_exit.py" \
  "$SAMPLE_MANIFEST" | awk 'END {print $1}')"
if [[ "$configmap_bytes" -gt 900000 ]]; then
  echo "VLA ConfigMap input is $configmap_bytes bytes; refusing to exceed the 900000-byte guard" >&2
  exit 1
fi

trap on_exit EXIT INT TERM

cat <<EOF | kubectl apply -f -
apiVersion: karpenter.sh/v1
kind: NodePool
metadata:
  name: $CPU_NODEPOOL
spec:
  disruption:
    consolidationPolicy: WhenEmpty
    consolidateAfter: 30s
  limits:
    cpu: "4"
  template:
    metadata:
      labels:
        zeptodb.com/role: vla-cpu-$EXPERIMENT
        zeptodb.com/run-id: "$SUFFIX-cpu"
    spec:
      nodeClassRef:
        group: eks.amazonaws.com
        kind: NodeClass
        name: default
      requirements:
        - key: kubernetes.io/arch
          operator: In
          values: [amd64]
        - key: karpenter.sh/capacity-type
          operator: In
          values: [on-demand]
        - key: eks.amazonaws.com/instance-family
          operator: In
          values: [c7i]
        - key: eks.amazonaws.com/instance-size
          operator: In
          values: [xlarge]
---
apiVersion: karpenter.sh/v1
kind: NodePool
metadata:
  name: $GPU_NODEPOOL
spec:
  disruption:
    consolidationPolicy: WhenEmpty
    consolidateAfter: 30s
  limits:
    cpu: "4"
  template:
    metadata:
      labels:
        zeptodb.com/role: vla-gpu-$EXPERIMENT
        zeptodb.com/run-id: "$SUFFIX-gpu"
    spec:
      nodeClassRef:
        group: eks.amazonaws.com
        kind: NodeClass
        name: default
      requirements:
        - key: kubernetes.io/arch
          operator: In
          values: [amd64]
        - key: karpenter.sh/capacity-type
          operator: In
          values: [on-demand]
        - key: eks.amazonaws.com/instance-family
          operator: In
          values: [g6e]
        - key: eks.amazonaws.com/instance-size
          operator: In
          values: [xlarge]
---
apiVersion: v1
kind: Namespace
metadata:
  name: $NAMESPACE
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: zepto-agent-memory
  namespace: $NAMESPACE
spec:
  replicas: 1
  selector:
    matchLabels:
      app: zepto-agent-memory
  template:
    metadata:
      labels:
        app: zepto-agent-memory
    spec:
      nodeSelector:
        zeptodb.com/run-id: "$SUFFIX-cpu"
      containers:
        - name: zeptodb
          image: $ZEPTO_IMAGE
          imagePullPolicy: Always
          args:
            - --port
            - "8123"
            - --no-auth
            - --agent-memory-dir
            - /opt/zeptodb/data/agent_memory
            - --agent-memory-flush-every
            - "1"
            - --agent-memory-ann
            - "off"
            - --agent-memory-max-memories
            - "512"
            - --agent-memory-max-cache-entries
            - "128"
          ports:
            - name: http
              containerPort: 8123
          readinessProbe:
            httpGet:
              path: /ready
              port: http
            periodSeconds: 3
            failureThreshold: 60
          resources:
            requests:
              cpu: 250m
              memory: 512Mi
            limits:
              cpu: "1"
              memory: 3Gi
          volumeMounts:
            - name: data
              mountPath: /opt/zeptodb/data
      volumes:
        - name: data
          emptyDir: {}
---
apiVersion: v1
kind: Service
metadata:
  name: zepto-agent-memory
  namespace: $NAMESPACE
spec:
  selector:
    app: zepto-agent-memory
  ports:
    - name: http
      port: 8123
      targetPort: http
EOF

kubectl -n "$NAMESPACE" create configmap vla-closed-loop-script \
  --from-file=physical_ai_vla_closed_loop.py="$PROJECT_ROOT/docs/research/tools/physical_ai_vla_closed_loop.py" \
  --from-file=physical_ai_vla_skip_region.py="$PROJECT_ROOT/docs/research/tools/physical_ai_vla_skip_region.py" \
  --from-file=physical_ai_vla_trajectory_fork.py="$PROJECT_ROOT/docs/research/tools/physical_ai_vla_trajectory_fork.py" \
  --from-file=physical_ai_vla_safety_gate.py="$PROJECT_ROOT/docs/research/tools/physical_ai_vla_safety_gate.py" \
  --from-file=physical_ai_vla_risk_router.py="$PROJECT_ROOT/docs/research/tools/physical_ai_vla_risk_router.py" \
  --from-file=physical_ai_vla_calibration_failure_attribution.py="$PROJECT_ROOT/docs/research/tools/physical_ai_vla_calibration_failure_attribution.py" \
  --from-file=physical_ai_vla_veto_separability.py="$PROJECT_ROOT/docs/research/tools/physical_ai_vla_veto_separability.py" \
  --from-file=physical_ai_vla_task_mapping_correction.py="$PROJECT_ROOT/docs/research/tools/physical_ai_vla_task_mapping_correction.py" \
  --from-file=physical_ai_vla_early_exit.py="$PROJECT_ROOT/docs/research/tools/physical_ai_vla_early_exit.py" \
  --from-file=sample_manifest.json="$SAMPLE_MANIFEST"

kubectl -n "$NAMESPACE" rollout status deployment/zepto-agent-memory --timeout=600s

cat <<EOF | kubectl apply -f -
apiVersion: batch/v1
kind: Job
metadata:
  name: $JOB
  namespace: $NAMESPACE
spec:
  backoffLimit: 0
  activeDeadlineSeconds: 10800
  template:
    metadata:
      labels:
        app: vla-closed-loop
    spec:
      restartPolicy: Never
      nodeSelector:
        zeptodb.com/run-id: "$SUFFIX-gpu"
      tolerations:
        - key: nvidia.com/gpu
          operator: Exists
          effect: NoSchedule
      containers:
        - name: ml
          image: $ML_IMAGE
          imagePullPolicy: IfNotPresent
          env:
            - name: MUJOCO_GL
              value: egl
            - name: PYOPENGL_PLATFORM
              value: egl
            - name: NVIDIA_DRIVER_CAPABILITIES
              value: all
            - name: CMAKE_POLICY_VERSION_MINIMUM
              value: "3.5"
            - name: LIBERO_CONFIG_PATH
              value: /tmp/libero-config
          command: ["/bin/bash", "-lc"]
          args:
            - >-
              (DEBIAN_FRONTEND=noninteractive
              apt-get update -qq &&
              DEBIAN_FRONTEND=noninteractive
              apt-get install -y -qq --no-install-recommends
              cmake g++ gcc make libc6-dev linux-libc-dev
              libegl1 libgl1 libglib2.0-0 &&
              python -m pip install --quiet --no-cache-dir
              torchvision==0.22.1
              'lerobot[libero,smolvla]==0.4.4'
              hf-libero==0.1.4 pyarrow==21.0.0 pillow==11.3.0
              sentencepiece==0.2.0 nvidia-ml-py==13.580.65)
              >/tmp/install.log 2>&1 ||
              { tail -c 3000 /tmp/install.log > /dev/termination-log; exit 1; };
              python -c 'from importlib.metadata import distribution;
              from pathlib import Path; import os, yaml;
              root = Path(distribution("hf-libero").locate_file("libero/libero")).resolve();
              target = Path(os.environ["LIBERO_CONFIG_PATH"]) / "config.yaml";
              target.parent.mkdir(parents=True, exist_ok=True);
              target.write_text(yaml.safe_dump({
              "benchmark_root": str(root),
              "bddl_files": str(root / "bddl_files"),
              "init_states": str(root / "init_files"),
              "datasets": str(root.parent / "datasets"),
              "assets": str(root / "assets")}))';
              python /scripts/$PYTHON_SCRIPT
              --agent-url http://zepto-agent-memory:8123
              --run-id "$SUFFIX"
              --sample-manifest /scripts/sample_manifest.json
              --result /dev/termination-log $EXTRA_ARGS $DETAIL_ARG;
              experiment_rc=\$?;
              if [[ -f /tmp/vla-detail.json && -n "$DETAIL_RESULT_STEM" ]]; then
              printf '%s\n' ZEPTO_VLA_DETAIL_BASE64_BEGIN;
              base64 -w 76 /tmp/vla-detail.json;
              printf '\n%s\n' ZEPTO_VLA_DETAIL_BASE64_END;
              fi;
              exit \$experiment_rc
          resources:
            requests:
              cpu: "2"
              memory: 20Gi
              nvidia.com/gpu: "1"
            limits:
              cpu: "3"
              memory: 28Gi
              nvidia.com/gpu: "1"
          volumeMounts:
            - name: script
              mountPath: /scripts
              readOnly: true
      volumes:
        - name: script
          configMap:
            name: vla-closed-loop-script
EOF

deadline=$((SECONDS + 10800))
while (( SECONDS < deadline )); do
  job_json="$(kubectl -n "$NAMESPACE" get job "$JOB" -o json)"
  if [[ "$(jq -r '.status.succeeded // 0' <<<"$job_json")" -ge 1 ]]; then
    break
  fi
  if [[ "$(jq -r '.status.failed // 0' <<<"$job_json")" -ge 1 ]]; then
    break
  fi
  sleep 15
done

pod_json="$(kubectl -n "$NAMESPACE" get pod -l job-name="$JOB" -o json)"
message="$(jq -r '
  [.items[].status.containerStatuses[]?
   | select(.name == "ml")
   | .state.terminated.message // empty] | last // empty
' <<<"$pod_json")"
if [[ -z "$message" ]]; then
  kubectl -n "$NAMESPACE" describe pod -l job-name="$JOB" >"$RESULT_DIR/pod-describe.txt"
  echo "Experiment $EXPERIMENT produced no termination result; see $RESULT_DIR/pod-describe.txt" >&2
  exit 1
fi
if ! printf '%s\n' "$message" | jq . >"$RESULT_JSON" 2>/dev/null; then
  printf '%s\n' "$message" >"$RESULT_DIR/install-error.txt"
  echo "Experiment $EXPERIMENT dependency setup failed:" >&2
  cat "$RESULT_DIR/install-error.txt" >&2
  exit 1
fi

expected_experiment=$((10#$EXPERIMENT))
result_experiment="$(jq -r '.experiment' "$RESULT_JSON")"
if [[ "$result_experiment" != "$expected_experiment" ]]; then
  echo "Experiment identity mismatch: expected $expected_experiment, got $result_experiment" >&2
  exit 1
fi

if [[ "$REQUIRE_DIAGNOSTIC_ONLY" == "1" ]]; then
  if ! jq -e '
    .diagnostic_kind == "veto_separability" and
    .execution.diagnostic_only == true and
    .execution.routed_started == false and
    .acceptance.routed_actions_zero == true
  ' "$RESULT_JSON" >/dev/null; then
    echo "Experiment $EXPERIMENT did not satisfy the forced diagnostic-only schema" >&2
    exit 1
  fi
fi

if [[ "$REQUIRE_TASK_MAPPING_CORRECTION" == "1" ]]; then
  if ! jq -e --argjson experiment "$expected_experiment" '
    .experiment == $experiment and
    .diagnostics.schema == 2 and
    .acceptance.task_mapping_consistent == true and
    .acceptance.trace_replication_consistent == true and
    .scope.suite_tasks == [0, 5] and
    .scope.steps == 595 and
    .scope.task_map == [
      [0, 5, "put both the alphabet soup and the tomato sauce in the basket"],
      [5, 9, "pick up the book and place it in the back compartment of the caddy"]
    ] and
    ((.memory.availability | keys | sort) == ["5", "9"]) and
    ([.memory.admission[][0]] == [5, 9]) and
    .detail.rows == 127 and
    ([.diagnostics.groups[][1]] == [0, 4, 123]) and
    ([.diagnostics.candidate_phases[][0]] | all(. == 0 or . == 5)) and
    ([.diagnostics.query_support[][0]] | all(. == 0 or . == 5)) and
    ([.diagnostics.task_structural[][0]] | all(. == 0 or . == 5))
  ' "$RESULT_JSON" >/dev/null; then
    echo "Experiment $EXPERIMENT did not satisfy the exact task-mapping correction schema" >&2
    exit 1
  fi
fi

DETAIL_JSON=""
if [[ -n "$DETAIL_RESULT_STEM" ]]; then
  pod_logs="$RESULT_DIR/pod.log"
  kubectl -n "$NAMESPACE" logs -l job-name="$JOB" --tail=-1 >"$pod_logs"
  detail_base64="$(awk '
    /ZEPTO_VLA_DETAIL_BASE64_BEGIN/ { capture=1; next }
    /ZEPTO_VLA_DETAIL_BASE64_END/ { capture=0; exit }
    capture { print }
  ' "$pod_logs" | tr -d '\r\n')"
  if [[ -z "$detail_base64" ]]; then
    echo "Experiment $EXPERIMENT produced no candidate detail artifact" >&2
    exit 1
  fi
  DETAIL_JSON="$RESULT_DIR/detail.json"
  printf '%s' "$detail_base64" | base64 -d >"$DETAIL_JSON"
  jq -e . "$DETAIL_JSON" >/dev/null
  expected_detail_sha="$(jq -r '.detail.sha256' "$RESULT_JSON")"
  actual_detail_sha="$(sha256sum "$DETAIL_JSON" | awk '{print $1}')"
  expected_detail_rows="$(jq -r '.detail.rows' "$RESULT_JSON")"
  actual_detail_rows="$(jq -r '.rows | length' "$DETAIL_JSON")"
  if [[ "$actual_detail_sha" != "$expected_detail_sha" \
    || "$actual_detail_rows" != "$expected_detail_rows" ]]; then
    echo "Experiment $EXPERIMENT candidate detail integrity check failed" >&2
    exit 1
  fi
  if [[ "$REQUIRE_TASK_MAPPING_CORRECTION" == "1" ]]; then
    if ! jq -e --argjson experiment "$expected_experiment" '
      .schema == 2 and
      .experiment == $experiment and
      .fields[0] == "suite_task_id" and
      .task_map_fields == [
        "suite_task_id",
        "manifest_task_index",
        "task_description"
      ] and
      .task_map == [
        [0, 5, "put both the alphabet soup and the tomato sauce in the basket"],
        [5, 9, "pick up the book and place it in the back compartment of the caddy"]
      ] and
      (.rows | all(.[0] == 0 or .[0] == 5))
    ' "$DETAIL_JSON" >/dev/null; then
      echo "Experiment $EXPERIMENT candidate detail has invalid task mapping" >&2
      exit 1
    fi
    detail_rows_json="$(jq -c '.rows' "$DETAIL_JSON")"
    detail_rows_sha="$(printf '%s' "$detail_rows_json" | sha256sum | awk '{print $1}')"
    if [[ "$detail_rows_sha" != "466467ef024f62bc815069dfa849838cd3a2ec3408319c022fa88a85b6f4552a" ]]; then
      echo "Experiment $EXPERIMENT candidate rows did not replicate Experiment 033" >&2
      exit 1
    fi
  fi
fi

status="$(jq -r '.status' "$RESULT_JSON")"
if [[ "$status" == "error" ]]; then
  echo "Experiment $EXPERIMENT stopped with a runtime error" >&2
  jq . "$RESULT_JSON" >&2
  exit 1
fi

cleanup
trap - EXIT INT TERM
jq -cS '.acceptance.aws_resources_deleted = true' "$RESULT_JSON" >"$RESULT_JSON.tmp"
mv "$RESULT_JSON.tmp" "$RESULT_JSON"

REPORT_OUTPUT="$RESULT_DIR/report.md"
python3 "$PROJECT_ROOT/docs/research/tools/$PYTHON_SCRIPT" \
  --render-json "$RESULT_JSON" \
  --output "$REPORT_OUTPUT"
publish_no_clobber "$REPORT_OUTPUT" "$OUTPUT"

if [[ -n "$DETAIL_JSON" ]]; then
  publish_no_clobber "$DETAIL_JSON" "$DETAIL_OUTPUT"
fi

echo "Status: $status"
echo "Result: $OUTPUT"
echo "Raw compact result: $RESULT_JSON"
if [[ -n "$DETAIL_JSON" ]]; then
  echo "Candidate detail: $DETAIL_OUTPUT"
fi
if [[ "$status" != "pass" ]]; then
  exit 2
fi
