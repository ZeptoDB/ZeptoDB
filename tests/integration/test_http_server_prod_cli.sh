#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SERVER_BIN="${SERVER_BIN:-$REPO_ROOT/build/zepto_http_server}"

if [[ ! -x "$SERVER_BIN" ]]; then
    echo "FAIL: zepto_http_server not built at $SERVER_BIN"
    exit 1
fi

TMP_DIR="$(mktemp -d -t zepto_http_prod_cli_XXXXXX)"
PORT=$((26000 + ($$ % 2000)))
SERVER_PID=""
OIDC_PID=""

cleanup() {
    if [[ -n "$SERVER_PID" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [[ -n "$OIDC_PID" ]]; then
        kill "$OIDC_PID" 2>/dev/null || true
        wait "$OIDC_PID" 2>/dev/null || true
    fi
    rm -r -- "$TMP_DIR"
}
trap cleanup EXIT

expect_failure() {
    local expected="$1"
    shift
    if "$SERVER_BIN" "$@" >"$TMP_DIR/failure.log" 2>&1; then
        echo "FAIL: command unexpectedly succeeded: $*"
        exit 1
    fi
    if ! grep -Fq -- "$expected" "$TMP_DIR/failure.log"; then
        echo "FAIL: expected '$expected' in failure output"
        cat "$TMP_DIR/failure.log"
        exit 1
    fi
}

wait_ready() {
    local port="$1"
    for _ in $(seq 1 50); do
        if curl -fsS "http://127.0.0.1:$port/ping" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

wait_ready_tls() {
    local port="$1"
    for _ in $(seq 1 50); do
        if curl -kfsS "https://127.0.0.1:$port/ping" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

expect_failure "unknown or incomplete option" --definitely-unknown
expect_failure "no credential source" --ticks 0
expect_failure "refusing a plaintext non-loopback" \
    --no-auth --bind 0.0.0.0 --ticks 0

HELP_OUTPUT="$("$SERVER_BIN" --help)"
for option in --oidc-issuer --oidc-client-id --oidc-client-secret \
    --oidc-client-secret-env --oidc-client-secret-file \
    --oidc-redirect-uri --oidc-audience; do
    if ! grep -Fq -- "$option" <<<"$HELP_OUTPUT"; then
        echo "FAIL: --help omitted $option"
        exit 1
    fi
done

expect_failure "OIDC requires --oidc-issuer" \
    --oidc-issuer https://idp.example.test
expect_failure "--oidc-issuer must use HTTPS" \
    --oidc-issuer http://idp.example.test --oidc-client-id client \
    --oidc-client-secret secret --oidc-redirect-uri http://localhost/callback
expect_failure "choose exactly one OIDC client-secret source" \
    --oidc-issuer https://idp.example.test --oidc-client-id client \
    --oidc-client-secret secret --oidc-client-secret-env SECRET \
    --oidc-redirect-uri http://localhost/callback
expect_failure "environment variable is unset or empty" \
    --oidc-issuer https://idp.example.test --oidc-client-id client \
    --oidc-client-secret-env ZEPTO_TEST_UNSET_OIDC_SECRET \
    --oidc-redirect-uri http://localhost/callback
expect_failure "--oidc-redirect-uri must use HTTPS" \
    --oidc-issuer https://idp.example.test --oidc-client-id client \
    --oidc-client-secret secret \
    --oidc-redirect-uri http://idp.example.test/callback
expect_failure "--oidc-redirect-uri must use HTTPS" \
    --oidc-issuer https://idp.example.test --oidc-client-id client \
    --oidc-client-secret secret \
    --oidc-redirect-uri http://localhost.attacker.test/callback
expect_failure "--oidc-redirect-uri must use HTTPS" \
    --oidc-issuer https://idp.example.test --oidc-client-id client \
    --oidc-client-secret secret \
    --oidc-redirect-uri http://127.0.0.1.attacker.test/callback
expect_failure "HTTPS OIDC redirect behind a TLS proxy requires --secure-cookie" \
    --oidc-issuer https://idp.example.test --oidc-client-id client \
    --oidc-client-secret secret \
    --oidc-redirect-uri https://zepto.example.test/auth/callback

mkdir -p "$TMP_DIR/corrupt-hdb"
expect_failure "--acknowledge-incomplete-durability" \
    --no-auth --storage-mode tiered --hdb-dir "$TMP_DIR/corrupt-hdb"
printf '%s' '{"next_table_id":2,"tables":[' \
    >"$TMP_DIR/corrupt-hdb/_schema.json"
expect_failure "invalid schema catalog" \
    --no-auth --storage-mode tiered --hdb-dir "$TMP_DIR/corrupt-hdb" \
    --acknowledge-incomplete-durability

mkdir -p "$TMP_DIR/corrupt-recovery/_rdb_snapshots"
printf '%s\n' 'not-a-generation' \
    >"$TMP_DIR/corrupt-recovery/_rdb_snapshots/CURRENT"
expect_failure "pipeline recovery/startup failed" \
    --no-auth --storage-mode tiered --hdb-dir "$TMP_DIR/corrupt-recovery" \
    --acknowledge-incomplete-durability

# Run a local TLS discovery endpoint to prove that OIDC CLI values reach
# AuthManager, server-side sessions are enabled, and a mounted secret works.
OIDC_PORT=$((PORT + 200))
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -subj '/CN=127.0.0.1' \
    -addext 'subjectAltName=IP:127.0.0.1' \
    -keyout "$TMP_DIR/oidc.key" -out "$TMP_DIR/oidc.crt" \
    >/dev/null 2>&1
printf '%s\n' 'mounted-oidc-secret' >"$TMP_DIR/oidc.secret"
python3 - "$OIDC_PORT" "$TMP_DIR/oidc.crt" "$TMP_DIR/oidc.key" \
    >"$TMP_DIR/oidc-provider.log" 2>&1 <<'PY' &
import http.server
import json
import ssl
import sys

port = int(sys.argv[1])
issuer = f"https://127.0.0.1:{port}"

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/.well-known/openid-configuration":
            payload = {
                "issuer": issuer,
                "jwks_uri": issuer + "/jwks",
                "authorization_endpoint": issuer + "/authorize",
                "token_endpoint": issuer + "/token",
            }
        elif self.path == "/jwks":
            payload = {"keys": []}
        else:
            self.send_error(404)
            return
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass

server = http.server.HTTPServer(("127.0.0.1", port), Handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(sys.argv[2], sys.argv[3])
server.socket = context.wrap_socket(server.socket, server_side=True)
server.serve_forever()
PY
OIDC_PID=$!
for _ in $(seq 1 50); do
    if curl -kfsS \
        "https://127.0.0.1:$OIDC_PORT/.well-known/openid-configuration" \
        >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

SSL_CERT_FILE="$TMP_DIR/oidc.crt" \
"$SERVER_BIN" --port "$PORT" --ticks 0 \
    --oidc-issuer "https://127.0.0.1:$OIDC_PORT" \
    --oidc-client-id zepto-test \
    --oidc-client-secret-file "$TMP_DIR/oidc.secret" \
    --oidc-redirect-uri "http://127.0.0.1:$PORT/auth/callback" \
    --oidc-audience zepto-test \
    >"$TMP_DIR/oidc-server.log" 2>&1 &
SERVER_PID=$!
if ! wait_ready "$PORT"; then
    echo "FAIL: OIDC-configured server did not start"
    cat "$TMP_DIR/oidc-server.log"
    exit 1
fi
if grep -Fq 'mounted-oidc-secret' "$TMP_DIR/oidc-server.log"; then
    echo "FAIL: OIDC client secret was written to server logs"
    exit 1
fi
kill "$SERVER_PID"
wait "$SERVER_PID" || true
SERVER_PID=""
kill "$OIDC_PID"
wait "$OIDC_PID" || true
OIDC_PID=""

KEY_FILE="$TMP_DIR/api_keys.txt"
"$SERVER_BIN" --port "$PORT" --ticks 0 \
    --api-keys-file "$KEY_FILE" --bootstrap-dev-keys \
    >"$TMP_DIR/bootstrap.log" 2>&1 &
SERVER_PID=$!
if ! wait_ready "$PORT"; then
    echo "FAIL: bootstrap server did not start"
    cat "$TMP_DIR/bootstrap.log"
    exit 1
fi
kill "$SERVER_PID"
wait "$SERVER_PID" || true
SERVER_PID=""
grep -Fq '# zeptodb-keys-v1' "$KEY_FILE"
grep -Fq 'Dev API Keys (shown once)' "$TMP_DIR/bootstrap.log"

ZEPTO_API_KEYS_FILE="$KEY_FILE" \
    "$SERVER_BIN" --port "$PORT" --ticks 0 --no-bootstrap-dev-keys \
    >"$TMP_DIR/production.log" 2>&1 &
SERVER_PID=$!
if ! wait_ready "$PORT"; then
    echo "FAIL: environment-key server did not start"
    cat "$TMP_DIR/production.log"
    exit 1
fi
if grep -Fq 'Dev API Keys' "$TMP_DIR/production.log"; then
    echo "FAIL: production startup printed development API keys"
    exit 1
fi

expect_failure "failed to bind HTTP listener" \
    --port "$PORT" --no-auth --ticks 0

kill "$SERVER_PID"
wait "$SERVER_PID" || true
SERVER_PID=""

# The pyarrow fallback exports bundled OpenSSL symbols. Exercise real
# certificate loading so ELF dependency-order regressions fail here instead of
# crashing a production process at startup.
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -subj '/CN=127.0.0.1' \
    -keyout "$TMP_DIR/server.key" -out "$TMP_DIR/server.crt" \
    >/dev/null 2>&1
TLS_PORT=$((PORT + 1))
"$SERVER_BIN" --port "$TLS_PORT" --ticks 0 --no-auth \
    --tls-cert "$TMP_DIR/server.crt" --tls-key "$TMP_DIR/server.key" \
    >"$TMP_DIR/tls.log" 2>&1 &
SERVER_PID=$!
if ! wait_ready_tls "$TLS_PORT"; then
    echo "FAIL: direct TLS server did not start"
    cat "$TMP_DIR/tls.log"
    exit 1
fi
kill "$SERVER_PID"
wait "$SERVER_PID" || true
SERVER_PID=""

echo "PASS: test_http_server_prod_cli"
