#!/usr/bin/env bash
# ============================================================================
# devlog 086 (D4): zepto_http_server --hdb-dir / --storage-mode integration
# ----------------------------------------------------------------------------
# Verifies that launching with --hdb-dir <path> (or --storage-mode tiered)
# persists the SchemaRegistry catalog and final graceful-stop RDB generation
# across a server restart, then exposes the restored row to general SQL.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SERVER_BIN="${SERVER_BIN:-$REPO_ROOT/build/zepto_http_server}"

if [[ ! -x "$SERVER_BIN" ]]; then
    echo "FAIL: zepto_http_server not built at $SERVER_BIN"
    exit 1
fi

PORT="${PORT:-28999}"
HDB="$(mktemp -d -t zepto_hdb_it_XXXXXX)"
trap 'rm -rf "$HDB"' EXIT

"$SERVER_BIN" --port "$PORT" --no-auth --hdb-dir "$HDB" --storage-mode tiered \
    --acknowledge-incomplete-durability \
    >"$HDB/server.log" 2>&1 &
SVR=$!
trap 'kill $SVR 2>/dev/null || true; wait $SVR 2>/dev/null || true; rm -rf "$HDB"' EXIT

# Wait for server to bind (up to 5s).
for i in $(seq 1 25); do
    if curl -fsS "http://127.0.0.1:$PORT/ping" >/dev/null 2>&1; then break; fi
    sleep 0.2
done

curl -fsS -X POST "http://127.0.0.1:$PORT/" \
    -d 'CREATE TABLE hdb_test(symbol INT64, price INT64, volume INT64, timestamp INT64)' >/dev/null
curl -fsS -X POST "http://127.0.0.1:$PORT/" \
    -d 'INSERT INTO hdb_test VALUES (1,100,10,1000)' >/dev/null

kill "$SVR" 2>/dev/null || true
if ! wait "$SVR"; then
    echo 'FAIL: first server did not complete graceful shutdown successfully'
    cat "$HDB/server.log" || true
    exit 1
fi

if [[ ! -f "$HDB/_schema.json" ]]; then
    echo 'FAIL: _schema.json missing after server shutdown'
    echo '--- server.log ---'
    cat "$HDB/server.log" || true
    exit 1
fi
if ! grep -q hdb_test "$HDB/_schema.json"; then
    echo 'FAIL: schema_registry did not persist CREATE TABLE hdb_test'
    cat "$HDB/_schema.json"
    exit 1
fi

# Restart against the same HDB directory and prove that both the catalog and
# gracefully snapshotted hot row load. Abrupt termination remains a separate
# production-promotion gate.
"$SERVER_BIN" --port "$PORT" --no-auth --hdb-dir "$HDB" --storage-mode tiered \
    --acknowledge-incomplete-durability \
    >"$HDB/server-restart.log" 2>&1 &
SVR=$!

READY=false
for i in $(seq 1 25); do
    if curl -fsS "http://127.0.0.1:$PORT/ping" >/dev/null 2>&1; then
        READY=true
        break
    fi
    if ! kill -0 "$SVR" 2>/dev/null; then
        break
    fi
    sleep 0.2
done

if [[ "$READY" != true ]]; then
    echo 'FAIL: restarted server did not become ready'
    cat "$HDB/server-restart.log" || true
    exit 1
fi

TABLES="$(curl -fsS -X POST "http://127.0.0.1:$PORT/" -d 'SHOW TABLES')"
if ! grep -q hdb_test <<<"$TABLES"; then
    echo 'FAIL: restarted server did not load hdb_test from the schema catalog'
    echo "$TABLES"
    cat "$HDB/server-restart.log" || true
    exit 1
fi

COUNT="$(curl -fsS -X POST "http://127.0.0.1:$PORT/" \
    -d 'SELECT count(*) AS rows FROM hdb_test')"
if ! grep -Fq '"data":[[1]]' <<<"$COUNT"; then
    echo 'FAIL: graceful restart did not restore the inserted SQL row'
    echo "$COUNT"
    cat "$HDB/server-restart.log" || true
    exit 1
fi

kill "$SVR" 2>/dev/null || true
if ! wait "$SVR"; then
    echo 'FAIL: restarted server did not complete graceful shutdown successfully'
    cat "$HDB/server-restart.log" || true
    exit 1
fi

echo 'PASS: test_http_hdb'
