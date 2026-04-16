#!/usr/bin/env bash
# ZeptoDB K8s Operator — Bash Reconciler
# Watches ZeptoDBCluster CRDs and reconciles via Helm.
set -euo pipefail

CHART_PATH="${CHART_PATH:-/opt/operator/chart}"
NAMESPACE="${WATCH_NAMESPACE:-default}"
POLL_INTERVAL="${POLL_INTERVAL:-10}"

log() { echo "[$(date -u +%FT%TZ)] $*"; }

update_status() {
  local name="$1" phase="$2" ready="$3" edition="$4" msg="$5"
  kubectl patch zeptodbcluster/"$name" -n "$NAMESPACE" --type=merge --subresource=status \
    -p "{\"status\":{\"phase\":\"$phase\",\"readyReplicas\":$ready,\"edition\":\"$edition\",\"message\":\"$msg\"}}" \
    2>/dev/null || true
}

reconcile() {
  local name="$1"
  local cr
  cr=$(kubectl get zeptodbcluster/"$name" -n "$NAMESPACE" -o json 2>/dev/null) || return 0

  local replicas version storage secret_name secret_key
  replicas=$(echo "$cr" | jq -r '.spec.replicas // 1')
  version=$(echo "$cr" | jq -r '.spec.version // "0.0.3"')
  storage=$(echo "$cr" | jq -r '.spec.storage // "500Gi"')
  secret_name=$(echo "$cr" | jq -r '.spec.license.secretName // ""')
  secret_key=$(echo "$cr" | jq -r '.spec.license.secretKey // "license-key"')

  local edition="community"
  local cluster_enabled="false"
  local extra_args=()

  # --- License gate: multi-node requires license secret ---
  if [ "$replicas" -gt 1 ]; then
    cluster_enabled="true"
    if [ -z "$secret_name" ]; then
      log "FAIL $name: multi-node without license secret"
      update_status "$name" "Failed" 0 "community" "Multi-node cluster requires Enterprise license"
      return 0
    fi
    if ! kubectl get secret "$secret_name" -n "$NAMESPACE" >/dev/null 2>&1; then
      log "FAIL $name: license secret '$secret_name' not found"
      update_status "$name" "Failed" 0 "community" "License secret '$secret_name' not found"
      return 0
    fi
    edition="enterprise"
    extra_args+=(
      --set "extraEnv[0].name=ZEPTODB_LICENSE_KEY"
      --set "extraEnv[0].valueFrom.secretKeyRef.name=$secret_name"
      --set "extraEnv[0].valueFrom.secretKeyRef.key=$secret_key"
    )
  fi

  update_status "$name" "Pending" 0 "$edition" "Reconciling"

  log "HELM $name: replicas=$replicas version=$version storage=$storage edition=$edition"
  if helm upgrade --install "zdb-$name" "$CHART_PATH" \
    -n "$NAMESPACE" \
    --set replicaCount="$replicas" \
    --set image.tag="$version" \
    --set persistence.size="$storage" \
    --set cluster.enabled="$cluster_enabled" \
    "${extra_args[@]+"${extra_args[@]}"}"; then

    # Count ready pods
    local ready
    ready=$(kubectl get pods -n "$NAMESPACE" -l "app.kubernetes.io/instance=zdb-$name" \
      -o jsonpath='{range .items[*]}{.status.conditions[?(@.type=="Ready")].status}{"\n"}{end}' 2>/dev/null \
      | grep -c True || echo 0)

    update_status "$name" "Running" "$ready" "$edition" ""
    log "OK $name: phase=Running ready=$ready/$replicas"
  else
    update_status "$name" "Failed" 0 "$edition" "Helm upgrade failed"
    log "FAIL $name: helm upgrade failed"
  fi
}

handle_delete() {
  local name="$1"
  log "DELETE $name"
  helm uninstall "zdb-$name" -n "$NAMESPACE" 2>/dev/null || true
}

# --- Main reconcile loop ---
log "ZeptoDB Operator started (namespace=$NAMESPACE, poll=${POLL_INTERVAL}s)"

RUNNING=true
trap 'RUNNING=false; log "SIGTERM received, shutting down"' SIGTERM SIGINT

# Track known resources for delete detection
declare -A KNOWN=()

while $RUNNING; do
  # Get current set of CRs — skip cycle on failure to avoid false deletions
  names_output=$(kubectl get zeptodbcluster -n "$NAMESPACE" -o jsonpath='{.items[*].metadata.name}' 2>&1) || {
    log "WARN: kubectl get failed, skipping cycle"
    sleep "$POLL_INTERVAL"
    continue
  }

  declare -A CURRENT=()
  for name in $names_output; do
    CURRENT["$name"]=1
    reconcile "$name"
  done

  # Detect deletions
  for name in "${!KNOWN[@]}"; do
    if [ -z "${CURRENT[$name]:-}" ]; then
      handle_delete "$name"
    fi
  done

  # Update known set
  KNOWN=()
  for name in "${!CURRENT[@]}"; do
    KNOWN["$name"]=1
  done
  unset CURRENT

  sleep "$POLL_INTERVAL"
done

log "Operator stopped"
