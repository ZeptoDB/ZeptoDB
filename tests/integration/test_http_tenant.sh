#!/usr/bin/env bash
# ============================================================================
# devlog 091 F1: zepto_http_server --tenant id:namespace smoke test
# ----------------------------------------------------------------------------
# Launches zepto_http_server with a single tenant "deska_" and verifies:
#   1. A CREATE TABLE inside the namespace (deska_t) succeeds (baseline).
#   2. With X-Zepto-Tenant-Id: deska_, SELECT against a table outside the
#      namespace (deskb_t) returns 403.
#   3. With X-Zepto-Tenant-Id: deska_, SELECT against a table inside the
#      namespace (deska_t) returns 200.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIN="${SERVER_BIN:-$REPO_ROOT/build/zepto_http_server}"
PORT="${PORT:-28811}"

if [[ ! -x "$BIN" ]]; then
    echo "FAIL: zepto_http_server not built at $BIN"
    exit 1
fi

"$BIN" --port "$PORT" --no-auth --tenant deska_:deska_ >/tmp/zepto_tenant_test.log 2>&1 &
SVR=$!
trap 'kill $SVR 2>/dev/null || true; wait $SVR 2>/dev/null || true' EXIT

# Wait for server bind (up to 5s)
for i in $(seq 1 25); do
    if curl -fsS "http://127.0.0.1:$PORT/ping" >/dev/null 2>&1; then break; fi
    sleep 0.2
done

# Baseline: create tables inside + outside the namespace (no tenant header)
curl -sS -X POST "http://127.0.0.1:$PORT/" \
    -d 'CREATE TABLE deska_t (symbol INT64, price INT64, volume INT64, timestamp INT64)' >/dev/null
curl -sS -X POST "http://127.0.0.1:$PORT/" \
    -d 'CREATE TABLE deskb_t (symbol INT64, price INT64, volume INT64, timestamp INT64)' >/dev/null

# 1. Outside namespace → 403
RESULT=$(curl -sS -o /dev/null -w "%{http_code}" -X POST "http://127.0.0.1:$PORT/" \
    -H "X-Zepto-Tenant-Id: deska_" \
    -d 'SELECT * FROM deskb_t')
if [[ "$RESULT" != "403" ]]; then
    echo "FAIL: expected 403 for deskb_t outside namespace, got $RESULT"
    exit 1
fi

# 2. Inside namespace → 200
RESULT=$(curl -sS -o /dev/null -w "%{http_code}" -X POST "http://127.0.0.1:$PORT/" \
    -H "X-Zepto-Tenant-Id: deska_" \
    -d 'SELECT * FROM deska_t')
if [[ "$RESULT" != "200" ]]; then
    echo "FAIL: expected 200 for deska_t inside namespace, got $RESULT"
    exit 1
fi

echo "PASS: test_http_tenant"
