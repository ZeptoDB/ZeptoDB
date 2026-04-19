#!/usr/bin/env bash
# ============================================================================
# tools/run-aarch64-tests.sh — devlog 088
# ----------------------------------------------------------------------------
# Build the `test` Dockerfile stage for linux/arm64 via buildx, push to ECR,
# then run the unit / migration / feed test suites on a Graviton node of the
# EKS bench cluster.
#
# Usage:
#   ./tools/run-aarch64-tests.sh
#
# Environment:
#   BUILD_PLATFORM  default: linux/arm64
#   REPO            ECR repo (required for --push). Override with your account.
#   TAG             image tag (default: latest-arm64-test)
#   SKIP_PUSH       if set to 1, builds but does not push (local smoke check)
#
# Requires: docker buildx, AWS creds for the target ECR repo, kubectl for
# wake/sleep (handled by tools/eks-bench.sh).
# ============================================================================
set -euo pipefail

BUILD_PLATFORM=${BUILD_PLATFORM:-linux/arm64}
REPO=${REPO:-REPLACE_ME.dkr.ecr.ap-northeast-2.amazonaws.com/zeptodb}
TAG=${TAG:-latest-arm64-test}
SKIP_PUSH=${SKIP_PUSH:-0}

# devlog 089: guard against the REPLACE_ME placeholder leaking into a push.
if [[ "$REPO" == *REPLACE_ME* ]] && [[ "${SKIP_PUSH:-0}" != "1" ]]; then
    echo "ERROR: REPO still contains REPLACE_ME. Set REPO=<your-ecr-repo> or SKIP_PUSH=1."
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "==> Building aarch64 test image ($BUILD_PLATFORM, target=test)"
if [[ "$SKIP_PUSH" == "1" ]]; then
    docker buildx build \
        --platform "$BUILD_PLATFORM" \
        --target test \
        -f "$REPO_ROOT/deploy/docker/Dockerfile" \
        -t "$REPO:$TAG" \
        --load \
        "$REPO_ROOT"
    echo "==> Skipped push (SKIP_PUSH=1). Local image: $REPO:$TAG"
    exit 0
fi

docker buildx build \
    --platform "$BUILD_PLATFORM" \
    --target test \
    -f "$REPO_ROOT/deploy/docker/Dockerfile" \
    -t "$REPO:$TAG" \
    --push \
    "$REPO_ROOT"

echo "==> Waking EKS bench cluster (Graviton pool)"
"$REPO_ROOT/tools/eks-bench.sh" wake
trap '"$REPO_ROOT/tools/eks-bench.sh" sleep || true' EXIT

echo "==> Running full C++ test suite on aarch64 Graviton node"
kubectl run zepto-arm64-tests \
    --rm -i --restart=Never \
    --image="$REPO:$TAG" \
    --overrides='{"spec":{"nodeSelector":{"kubernetes.io/arch":"arm64"}}}' \
    -- /build/build/tests/zepto_tests

echo "==> aarch64 full-suite run complete"
