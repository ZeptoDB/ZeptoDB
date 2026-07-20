#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SERVER_BIN="${SERVER_BIN:-$REPO_ROOT/build/zepto_flight_server}"

if [[ ! -x "$SERVER_BIN" ]]; then
    echo "FAIL: zepto_flight_server not built at $SERVER_BIN"
    exit 1
fi

TMP_DIR="$(mktemp -d -t zepto_flight_tls_XXXXXX)"
PORT=$((29000 + ($$ % 1000)))
HTTP_PORT=$((PORT + 1000))
SERVER_PID=""

cleanup() {
    if [[ -n "$SERVER_PID" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -r -- "$TMP_DIR"
}
trap cleanup EXIT

set +e
timeout 5 "$SERVER_BIN" --flight-port "$PORT" --http-port "$HTTP_PORT" \
    --ticks 0 >"$TMP_DIR/missing-credentials.log" 2>&1
MISSING_CREDENTIAL_STATUS=$?
set -e
if [[ "$MISSING_CREDENTIAL_STATUS" -eq 0 ||
      "$MISSING_CREDENTIAL_STATUS" -eq 124 ]] ||
   ! grep -q "no credential source" "$TMP_DIR/missing-credentials.log"; then
    echo "FAIL: auth-enabled Flight started without an explicit credential source"
    cat "$TMP_DIR/missing-credentials.log"
    exit 1
fi

printf '%s\n' '# zeptodb-keys-v1' >"$TMP_DIR/empty.keys"
set +e
timeout 5 "$SERVER_BIN" --flight-port "$PORT" --http-port "$HTTP_PORT" \
    --ticks 0 --api-keys-file "$TMP_DIR/empty.keys" \
    >"$TMP_DIR/empty-credentials.log" 2>&1
EMPTY_CREDENTIAL_STATUS=$?
set -e
if [[ "$EMPTY_CREDENTIAL_STATUS" -eq 0 ||
      "$EMPTY_CREDENTIAL_STATUS" -eq 124 ]] ||
   ! grep -q "no active credentials" "$TMP_DIR/empty-credentials.log"; then
    echo "FAIL: auth-enabled Flight started with no active credentials"
    cat "$TMP_DIR/empty-credentials.log"
    exit 1
fi

# The Flight plaintext override must not implicitly expose the bundled HTTP
# listener. This validation runs before either listener is created.
set +e
timeout 5 "$SERVER_BIN" --flight-host 0.0.0.0 \
    --flight-port "$PORT" --http-port "$HTTP_PORT" --ticks 0 --no-auth \
    --allow-insecure-flight >"$TMP_DIR/plaintext-http.log" 2>&1
PLAINTEXT_STATUS=$?
set -e
if [[ "$PLAINTEXT_STATUS" -eq 0 || "$PLAINTEXT_STATUS" -eq 124 ]]; then
    echo "FAIL: Flight insecure override also allowed bundled plaintext HTTP"
    cat "$TMP_DIR/plaintext-http.log"
    exit 1
fi
if ! grep -q -- "--allow-plaintext-http" "$TMP_DIR/plaintext-http.log"; then
    echo "FAIL: bundled HTTP plaintext rejection did not name its own override"
    cat "$TMP_DIR/plaintext-http.log"
    exit 1
fi

openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -subj '/CN=localhost' \
    -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
    -keyout "$TMP_DIR/server.key" -out "$TMP_DIR/server.crt" \
    >/dev/null 2>&1

BOOTSTRAP_PORT=$((PORT + 1))
BOOTSTRAP_HTTP_PORT=$((HTTP_PORT + 1))
"$SERVER_BIN" --flight-port "$BOOTSTRAP_PORT" \
    --http-port "$BOOTSTRAP_HTTP_PORT" --ticks 0 \
    --tls-cert "$TMP_DIR/server.crt" --tls-key "$TMP_DIR/server.key" \
    --api-keys-file "$TMP_DIR/bootstrap.keys" --bootstrap-dev-keys \
    >"$TMP_DIR/bootstrap.log" 2>&1 &
SERVER_PID=$!
BOOTSTRAP_READY=false
for _ in $(seq 1 50); do
    if curl --cacert "$TMP_DIR/server.crt" -fsS \
        "https://localhost:$BOOTSTRAP_HTTP_PORT/ping" >/dev/null 2>&1; then
        BOOTSTRAP_READY=true
        break
    fi
    sleep 0.1
done
if [[ "$BOOTSTRAP_READY" != true ]]; then
    echo "FAIL: explicit Flight development-key bootstrap did not start"
    cat "$TMP_DIR/bootstrap.log"
    exit 1
fi
kill "$SERVER_PID"
wait "$SERVER_PID" || true
SERVER_PID=""
grep -q '# zeptodb-keys-v1' "$TMP_DIR/bootstrap.keys"
grep -q 'Dev API Keys (shown once' "$TMP_DIR/bootstrap.log"

"$SERVER_BIN" --flight-port "$PORT" --http-port "$HTTP_PORT" --ticks 0 \
    --no-auth --tls-cert "$TMP_DIR/server.crt" --tls-key "$TMP_DIR/server.key" \
    >"$TMP_DIR/server.log" 2>&1 &
SERVER_PID=$!

READY=false
for _ in $(seq 1 50); do
    if curl --cacert "$TMP_DIR/server.crt" -fsS \
        "https://localhost:$HTTP_PORT/ping" >/dev/null 2>&1; then
        READY=true
        break
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

if [[ "$READY" != true ]]; then
    echo "FAIL: bundled HTTPS listener did not start"
    cat "$TMP_DIR/server.log"
    exit 1
fi

python3 - "$TMP_DIR/server.crt" "$PORT" <<'PY'
import sys

import pyarrow.flight as flight

certificate_path, port = sys.argv[1], int(sys.argv[2])
with open(certificate_path, "rb") as certificate_file:
    roots = certificate_file.read()

client = flight.connect(
    f"grpc+tls://127.0.0.1:{port}",
    tls_root_certs=roots,
    override_hostname="localhost",
)
actions = list(client.list_actions())
if not actions:
    raise SystemExit("Flight TLS handshake succeeded but ListActions was empty")

# Production/default DoPut is fail-closed until stream-level atomic commit is
# implemented. Opening or closing the stream may surface the server status,
# depending on the Arrow client version.
schema = __import__("pyarrow").schema([
    ("symbol", __import__("pyarrow").int64()),
    ("price", __import__("pyarrow").int64()),
    ("volume", __import__("pyarrow").int64()),
    ("timestamp", __import__("pyarrow").int64()),
])
try:
    writer, _ = client.do_put(
        flight.FlightDescriptor.for_command("trades"), schema)
    writer.close()
except Exception as error:
    if "atomic stream commit" not in str(error):
        raise
else:
    raise SystemExit("Flight DoPut unexpectedly enabled by default")
PY

echo "PASS: test_flight_server_tls"
