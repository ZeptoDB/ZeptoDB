#!/bin/bash
# Run research-only Experiment 025 on a temporary EKS GPU node.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
REGION="ap-northeast-2"
RUN_ID="$(date +%Y%m%d%H%M%S)-$$"
SUFFIX="$(printf '%s' "$RUN_ID" | tr -cd '0-9' | tail -c 10)"
NAMESPACE="zeptodb-vla-025-$SUFFIX"
CPU_NODEPOOL="zepto-vla-cpu-$SUFFIX"
GPU_NODEPOOL="zepto-vla-gpu-$SUFFIX"
JOB="vision-retrieval"
RESULT_DIR="/tmp/zeptodb_vla_025_$RUN_ID"
RESULT_JSON="$RESULT_DIR/result.json"
SAMPLE_MANIFEST="$RESULT_DIR/sample_manifest.json"
REPORT_DRAFT="$RESULT_DIR/report.md"
OUTPUT="${1:-$PROJECT_ROOT/docs/research/results/physical_ai_vision_retrieval_025.md}"
ZEPTO_IMAGE="${ZEPTO_EKS_AGENT_IMAGE_REPO:-060795905711.dkr.ecr.ap-northeast-2.amazonaws.com/zeptodb}:${ZEPTO_EKS_AGENT_X86_TAG:-bench-x86}"
ML_IMAGE="${ZEPTO_VLA_ML_IMAGE:-pytorch/pytorch:2.5.1-cuda12.4-cudnn9-runtime}"
INSTANCE_IDS=()

source "$SCRIPT_DIR/require_zepto_bench_context.sh"
zepto_require_bench_context

if [[ -e "$OUTPUT" ]]; then
  echo "Refusing to overwrite immutable experiment result: $OUTPUT" >&2
  exit 1
fi

mkdir -p "$RESULT_DIR"

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
  kubectl delete namespace "$NAMESPACE" --wait=true --timeout=180s >/dev/null 2>&1 \
    || cleanup_rc=1
  kubectl delete nodepool "$CPU_NODEPOOL" "$GPU_NODEPOOL" \
    --wait=true --timeout=180s >/dev/null 2>&1 \
    || cleanup_rc=1

  for _ in $(seq 1 90); do
    if ! kubectl get nodeclaim \
      -l "karpenter.sh/nodepool in ($CPU_NODEPOOL,$GPU_NODEPOOL)" \
      -o name 2>/dev/null | grep -q .; then
      break
    fi
    sleep 2
  done
  if kubectl get nodeclaim \
    -l "karpenter.sh/nodepool in ($CPU_NODEPOOL,$GPU_NODEPOOL)" \
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

python3 "$PROJECT_ROOT/docs/research/tools/physical_ai_vision_retrieval.py" \
  --prepare-manifest "$SAMPLE_MANIFEST"

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
        zeptodb.com/role: vla-cpu-025
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
        zeptodb.com/role: vla-gpu-025
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
            - auto
            - --agent-memory-ann-min-records
            - "32"
            - --agent-memory-ann-max-candidates
            - "128"
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

kubectl -n "$NAMESPACE" create configmap vision-retrieval-script \
  --from-file=physical_ai_vision_retrieval.py="$PROJECT_ROOT/docs/research/tools/physical_ai_vision_retrieval.py" \
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
  activeDeadlineSeconds: 3600
  template:
    metadata:
      labels:
        app: vision-retrieval
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
          command: ["/bin/bash", "-lc"]
          args:
            - >-
              python -m pip install --quiet --no-cache-dir
              transformers==4.48.3 pyarrow==21.0.0 pillow==11.1.0
              protobuf==5.29.3 safetensors==0.5.2 sentencepiece==0.2.0 &&
              python /scripts/physical_ai_vision_retrieval.py
              --agent-url http://zepto-agent-memory:8123
              --run-id "$SUFFIX"
              --sample-manifest /scripts/sample_manifest.json
              --result /dev/termination-log
          resources:
            requests:
              cpu: "2"
              memory: 10Gi
              nvidia.com/gpu: "1"
            limits:
              cpu: "3"
              memory: 14Gi
              nvidia.com/gpu: "1"
          volumeMounts:
            - name: script
              mountPath: /scripts
              readOnly: true
      volumes:
        - name: script
          configMap:
            name: vision-retrieval-script
EOF

deadline=$((SECONDS + 3600))
while (( SECONDS < deadline )); do
  job_json="$(kubectl -n "$NAMESPACE" get job "$JOB" -o json)"
  if [[ "$(jq -r '.status.succeeded // 0' <<<"$job_json")" -ge 1 ]]; then
    break
  fi
  if [[ "$(jq -r '.status.failed // 0' <<<"$job_json")" -ge 1 ]]; then
    break
  fi
  sleep 10
done

pod_json="$(kubectl -n "$NAMESPACE" get pod -l job-name="$JOB" -o json)"
message="$(jq -r '
  [.items[].status.containerStatuses[]?
   | select(.name == "ml")
   | .state.terminated.message // empty] | last // empty
' <<<"$pod_json")"
if [[ -z "$message" ]]; then
  kubectl -n "$NAMESPACE" describe pod -l job-name="$JOB" >"$RESULT_DIR/pod-describe.txt"
  echo "Experiment 025 produced no termination result; see $RESULT_DIR/pod-describe.txt" >&2
  exit 1
fi
printf '%s\n' "$message" | jq . >"$RESULT_JSON"

status="$(jq -r '.status' "$RESULT_JSON")"
if [[ "$status" != "pass" ]]; then
  echo "Experiment 025 stopped with status=$status" >&2
  jq . "$RESULT_JSON" >&2
  exit 1
fi

cleanup
trap - EXIT INT TERM
jq '.acceptance.aws_resources_deleted = true' "$RESULT_JSON" >"$RESULT_JSON.tmp"
mv "$RESULT_JSON.tmp" "$RESULT_JSON"

python3 "$PROJECT_ROOT/docs/research/tools/physical_ai_vision_retrieval.py" \
  --render-json "$RESULT_JSON" \
  --output "$REPORT_DRAFT"
zepto_publish_no_clobber "$REPORT_DRAFT" "$OUTPUT"

echo "Result: $OUTPUT"
echo "Raw compact result: $RESULT_JSON"
