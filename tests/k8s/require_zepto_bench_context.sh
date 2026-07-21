#!/bin/bash
# Fail closed unless AWS and kubectl both target the dedicated ZeptoDB bench cluster.

zepto_require_bench_context() {
  local expected_account="060795905711"
  local expected_cluster="zepto-bench"
  local expected_region="ap-northeast-2"
  local actual_account expected_endpoint current_endpoint

  for command_name in aws kubectl; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
      echo "Required command is unavailable: $command_name" >&2
      return 1
    fi
  done

  actual_account="$(aws sts get-caller-identity --query Account --output text)"
  if [[ "$actual_account" != "$expected_account" ]]; then
    echo "Refusing to run outside AWS account $expected_account" >&2
    return 1
  fi

  expected_endpoint="$(
    aws eks describe-cluster \
      --name "$expected_cluster" \
      --region "$expected_region" \
      --query 'cluster.endpoint' \
      --output text
  )"
  current_endpoint="$(
    kubectl config view --minify --raw \
      -o jsonpath='{.clusters[0].cluster.server}'
  )"
  if [[ -z "$expected_endpoint" || "$expected_endpoint" == "None" \
      || "$current_endpoint" != "$expected_endpoint" ]]; then
    echo "Refusing to run outside EKS cluster $expected_cluster in $expected_region" >&2
    return 1
  fi
}

zepto_publish_no_clobber() {
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
except FileExistsError:
    raise SystemExit(f"Refusing to overwrite immutable experiment result: {target}")
finally:
    temporary.unlink(missing_ok=True)
PY
}
