# ZeptoDB Enterprise Security Operations Guide

Comprehensive reference for authentication, authorization, rate limiting, audit, multi-tenancy, and TLS configuration.

> SSO/OIDC setup: see [SSO Integration Guide](SSO_INTEGRATION_GUIDE.md)

---

## Table of Contents

- [Authentication Methods](#authentication-methods)
- [API Key Management](#api-key-management)
- [RBAC Roles & Permissions](#rbac-roles--permissions)
- [Rate Limiting](#rate-limiting)
- [Audit Logging](#audit-logging)
- [Multi-Tenancy](#multi-tenancy)
- [TLS / mTLS](#tls--mtls)
- [Query Timeout & Kill](#query-timeout--kill)
- [Secrets Management](#secrets-management)
- [Arrow Flight Security](#arrow-flight-security)
- [Compliance Notes](#compliance-notes)

---

## Authentication Methods

ZeptoDB supports three authentication methods, evaluated in this order:

```
1. Session cookie (zepto_sid)  — Web UI SSO sessions
2. SSO Identity Provider       — multi-IdP, issuer-routed JWT
3. JWT Bearer token            — single-IdP JWT (HS256/RS256)
4. API Key Bearer token        — zepto_<hex> format
```

All methods use the `Authorization: Bearer <token>` header (except session cookies).

### Public Paths (no auth required)

```
/ping    /health    /ready
/api/license
/auth/login    /auth/callback    /auth/logout    /auth/session
```

### Disabling Auth (development only)

```yaml
auth:
  enabled: false    # WARNING: all requests get admin access
```

---

## API Key Management

### Production Startup

Authentication-enabled HTTP startup is fail-closed. Supply a persisted key
store with `--api-keys-file PATH` (or `ZEPTO_API_KEYS_FILE`), or configure a
validated JWT method. A missing, unreadable, empty, or invalid key store does
not cause implicit credential creation. `--bootstrap-dev-keys` is the only
mode that creates and prints local development credentials and must not be
used in production.

```bash
./zepto_http_server \
  --bind 127.0.0.1 \
  --api-keys-file /run/secrets/zeptodb/api_keys.txt \
  --audit-log-file /var/log/zeptodb/audit.log
```

Rate limiting and audit recording are enabled by default. The
`--disable-rate-limit` and `--disable-audit` switches are development-only
opt-outs. A key store mounted read-only is suitable for static credentials,
but key create/update/revoke operations cannot be durably written to it; use a
writable protected store or Vault-backed key sync before relying on the admin
key-management API for rotation. File-backed mutations write a private
same-directory temporary file, sync it, atomically rename it, and sync the
parent directory before reporting success.

### Key Format

- Full key: `zepto_<64 hex chars>` (256-bit entropy)
- Stored as: SHA-256 hash (plaintext never persisted)
- Shown once at creation — cannot be retrieved later

### Create a Key

```bash
curl -X POST http://localhost:8123/admin/keys \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "name": "trading-desk-1",
    "role": "writer",
    "symbols": ["AAPL", "GOOGL"],
    "tables": ["trades", "quotes"],
    "tenant_id": "desk-a",
    "expires_at_ns": 1735689600000000000
  }'
```

Response (key shown once):
```json
{"key": "zepto_a1b2c3d4...", "id": "ak_7f3k", "role": "writer"}
```

### Update a Key

```bash
curl -X PATCH http://localhost:8123/admin/keys/ak_7f3k \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -H "Content-Type: application/json" \
  -d '{"symbols": ["AAPL", "GOOGL", "MSFT"], "enabled": true}'
```

Updatable fields: `symbols`, `tables`, `enabled`, `tenant_id`, `expires_at_ns`

### Revoke a Key

```bash
curl -X DELETE http://localhost:8123/admin/keys/ak_7f3k \
  -H "Authorization: Bearer $ADMIN_KEY"
```

### List Keys

```bash
curl http://localhost:8123/admin/keys -H "Authorization: Bearer $ADMIN_KEY"
```

### Key Expiry

Set `expires_at_ns` (epoch nanoseconds) at creation or via PATCH. Expired keys are rejected at validation time. Set to `0` for no expiry.

### Vault-Backed Key Sync

When Vault is configured, API keys are write-through synced:

```
Create key → write to local file + write to Vault
Revoke key → update local file + delete from Vault
Startup    → merge Vault keys not present locally
```

This enables multi-node key sharing without a shared filesystem.

```yaml
auth:
  api_keys_file: /etc/zeptodb/keys.txt
  vault_keys:
    enabled: true
    addr: https://vault.internal:8200
    token: ${VAULT_TOKEN}
    mount: secret
    prefix: zeptodb/keys
```

---

## RBAC Roles & Permissions

| Role | READ | WRITE | ADMIN | METRICS | Use Case |
|------|------|-------|-------|---------|----------|
| `admin` | ✅ | ✅ | ✅ | ✅ | DBAs, platform team |
| `writer` | ✅ | ✅ | ❌ | ✅ | Ingest services, trading desks |
| `reader` | ✅ | ❌ | ❌ | ❌ | Analysts, dashboards |
| `analyst` | ❌ | ❌ | ❌ | ❌ | Reserved until row-level symbol filtering ships |
| `metrics` | ❌ | ❌ | ❌ | ✅ | Prometheus scraper |

The `analyst` role intentionally has no permissions. This is a fail-closed
compatibility surface while persisted identities may still carry an
`allowed_symbols` list but the SQL executor does not yet enforce row-level
symbol filtering.

### Permission Mapping

| Permission | Endpoints |
|-----------|-----------|
| READ | `POST /` or read-only `GET /?query=` (`SELECT`, `SHOW`, `DESCRIBE`) |
| WRITE | `POST /` (`INSERT`, `UPDATE`, `DELETE`) |
| ADMIN | SQL DDL over `POST /` plus `/admin/*` (keys, queries, audit, tenants, nodes, settings) |
| METRICS | `GET /metrics`, `GET /stats`, `GET /api/ai/stats` |

SQL permissions are derived from the parsed statement, not from a string
prefix. Authenticated malformed or unsupported statements fail closed. The
server removes caller-provided internal `X-Zepto-*` authorization headers
before installing the authenticated role and ACL context.

`GET /?query=` never executes DML or DDL, even for writer/admin identities or
when authentication is disabled. State-changing SQL must use `POST /`. This
keeps `SameSite=Lax` browser sessions from being mutated by a cross-site
top-level GET navigation; custom browser integrations should still apply
normal Origin and CSRF controls to any state-changing HTTP endpoints they add.

### Symbol-Level ACL

API keys, JWT tokens, and SSO identities can carry a symbol whitelist:

```json
// API key creation
{"name": "external-analyst", "role": "analyst", "symbols": ["AAPL", "GOOGL"]}

// JWT claim
{"sub": "analyst@partner.com", "zepto_symbols": "AAPL,GOOGL"}
```

Any data request made with a non-empty symbol whitelist currently returns 403,
and `analyst` identities cannot read data. Do not grant access on the
assumption that ZeptoDB filters rows by symbol. Row-level symbol filtering
across SQL, ingest, Agent Memory, and Flight remains a product-promotion
blocker.

### Table-Level ACL

```json
{"name": "desk-a-reader", "role": "reader", "tables": ["trades", "quotes"]}
```

The allowlist is enforced on every table referenced by a JOIN, CTE, subquery,
set operation, or materialized-view definition on both SQL HTTP entry points.

---

## Rate Limiting

Token bucket algorithm, applied per-identity and per-IP.

### How It Works

```
Each identity gets a bucket:
  - Capacity: burst_capacity tokens (default: 200)
  - Refill rate: requests_per_minute / 60 tokens per second
  - Each request consumes 1 token
  - When tokens < 1: request returns 403 "Rate limit exceeded"
```

### Configuration

```yaml
auth:
  rate_limit:
    enabled: true
    requests_per_minute: 1000    # per identity (API key id or JWT sub)
    burst_capacity: 200          # max burst
    per_ip_rpm: 10000            # per source IP (0 = disabled)
    ip_burst: 500
```

### HTTP Behavior

When rate limited, the server returns:

```
HTTP 403 Forbidden
{"error": "Rate limit exceeded"}
```

The source-IP bucket is charged before JWT/JWKS or API-key verification so an
invalid-credential flood is bounded before expensive authentication work.
After a credential is validated, the per-identity bucket is charged as well.

### Per-Identity vs Per-IP

- Per-identity: tracks by API key ID or JWT `sub` claim
- Per-IP: tracks by `remote_addr` (defense against credential stuffing)
- Both checks must pass for a request to proceed

### Monitoring

Rate limiter bucket counts are visible in Prometheus metrics:

```
zepto_rate_limit_identity_buckets
zepto_rate_limit_ip_buckets
```

---

## Audit Logging

Every authenticated request is logged for compliance (SOC2, EMIR, MiFID II).

### Audit Log Format

```
[2026-04-02T01:30:00.000] [AUDIT] subject=ak_7f3k role=writer action="POST /" detail="apikey-auth" from=10.0.1.50
```

### Configuration

```yaml
auth:
  audit:
    enabled: true
    log_file: /var/log/zeptodb/audit.log    # empty = stderr
    buffer_enabled: true                     # in-memory ring for /admin/audit
```

### Querying Audit Events

```bash
# Last 100 events from in-memory buffer
curl "http://localhost:8123/admin/audit?n=100" \
  -H "Authorization: Bearer $ADMIN_KEY"
```

Response:
```json
[
  {
    "timestamp_ns": 1712019000000000000,
    "subject": "ak_7f3k",
    "role": "writer",
    "action": "POST /",
    "detail": "apikey-auth",
    "remote_addr": "10.0.1.50"
  }
]
```

### Retention

- In-memory buffer: 10,000 events (ring buffer, oldest evicted)
- File log: unlimited (manage with logrotate)
- Compliance target: 7-year retention via log shipping to S3/SIEM

### What Gets Audited

| Event | Logged |
|-------|--------|
| Successful auth (API key, JWT, SSO) | ✅ |
| Failed auth (invalid key, expired JWT) | ✅ (via HTTP access log) |
| Admin operations (key create/revoke, query kill) | ✅ |
| Rate limit rejections | ✅ (via HTTP access log) |
| Public path access (/ping, /health) | ❌ (no auth context) |

---

## Multi-Tenancy

Isolate resources per tenant: query concurrency, table namespace, usage tracking.

### Create a Tenant

```bash
curl -X POST http://localhost:8123/admin/tenants \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "tenant_id": "desk-a",
    "name": "Trading Desk A",
    "max_concurrent_queries": 10,
    "table_namespace": "desk_a"
  }'
```

### Tenant Quotas

| Quota | Default | Description |
|-------|---------|-------------|
| `max_concurrent_queries` | 10 | Queries beyond this are rejected (429) |
| `max_memory_bytes` | 0 | Reserved; not currently enforced |
| `max_queries_per_minute` | 0 | Reserved; use the AuthManager identity/IP limiter |
| `max_ingestion_rate` | 0 | Reserved; not currently enforced |
| `table_namespace` | "" (unrestricted) | Table prefix restriction |

### Table Namespace Isolation

When `table_namespace` is set (e.g., `desk_a`), the tenant can only access tables prefixed with `desk_a.`:

```sql
-- Tenant desk-a can access:
SELECT * FROM desk_a.trades     -- ✅
SELECT * FROM desk_a.quotes     -- ✅

-- Cannot access:
SELECT * FROM trades            -- ❌ 403
SELECT * FROM desk_b.trades     -- ❌ 403
```

### Binding Keys to Tenants

```bash
curl -X POST http://localhost:8123/admin/keys \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -d '{"name": "desk-a-svc", "role": "writer", "tenant_id": "desk-a"}'
```

### Usage Tracking

```bash
curl http://localhost:8123/admin/tenants -H "Authorization: Bearer $ADMIN_KEY"
```

Response includes per-tenant usage:
```json
[{
  "tenant_id": "desk-a",
  "name": "Trading Desk A",
  "max_concurrent_queries": 10,
  "active_queries": 3,
  "total_queries": 15420,
  "rejected_queries": 12
}]
```

---

## TLS / mTLS

### Server TLS (HTTPS)

```yaml
tls:
  enabled: true
  cert_path: /etc/zeptodb/server.crt
  key_path: /etc/zeptodb/server.key
  https_port: 8443
  also_serve_http: false    # disable plaintext HTTP in production
```

```bash
# Direct TLS with Bearer authentication
./zepto_http_server \
  --bind 0.0.0.0 \
  --tls-cert /etc/zeptodb/server.crt \
  --tls-key /etc/zeptodb/server.key \
  --api-keys-file /run/secrets/zeptodb/api_keys.txt

# Test
curl --cacert ca.crt https://zepto.internal:8443/ping
```

The listener defaults to `127.0.0.1`. Plain HTTP on a non-loopback address
fails closed. When a trusted ingress terminates TLS, the internal listener must
explicitly use `--bind 0.0.0.0 --allow-plaintext-http --secure-cookie`; network
policy must restrict that plaintext hop. Direct TLS automatically marks session
cookies Secure. `--tls-cert` and `--tls-key` are a required pair.

### Mutual TLS (mTLS)

For client certificate verification (zero-trust networks):

```yaml
tls:
  enabled: true
  cert_path: /etc/zeptodb/server.crt
  key_path: /etc/zeptodb/server.key
  ca_cert_path: /etc/zeptodb/ca.crt    # CA that signed client certs
  https_port: 8443
```

With `ca_cert_path` set, the server requires clients to present a certificate signed by that CA.

The CLI equivalent is `--tls-ca /etc/zeptodb/ca.crt`. This is a client CA,
not merely a server trust bundle: supplying it makes client certificates
mandatory.

```bash
# Client with cert
curl --cert client.crt --key client.key --cacert ca.crt \
  https://zepto.internal:8443/ping
```

mTLS is independent of Bearer token auth — both can be required simultaneously for defense-in-depth.

### Certificate Rotation

Replace cert/key files and restart the server. ZeptoDB does not hot-reload TLS
material, so cert-manager alone is insufficient; arrange a controlled workload
rollout when the mounted Secret changes.

### Inter-Node RPC Security

Inter-node TCP RPC uses a mutual, replay-resistant challenge protocol:

1. The client sends a fresh 32-byte nonce.
2. The server returns a fresh 32-byte nonce and a domain-separated
   HMAC-SHA256 proof over both nonces.
3. The client validates the server, then returns its own domain-separated
   HMAC-SHA256 proof over both nonces.
4. The server responds with `AUTH_OK` only after constant-time proof
   validation.

Configure exactly one process-wide secret source on every node. A secret must
contain at least 32 bytes; the file form is recommended so the secret does not
appear in a command line or ordinary environment dump.

```bash
export ZEPTO_CLUSTER_SECRET_FILE=/run/secrets/zeptodb/cluster-rpc
# Alternative: export ZEPTO_CLUSTER_SECRET='at-least-32-bytes-from-a-secret-store'
```

`zepto_data_node` and `zepto_ingest_node` always fail startup when the secret
is absent or invalid. `zepto_http_server` applies the same rule when `--ha` or
`--add-node` is used, and also rejects runtime `POST /admin/nodes` additions
without a valid secret. `--allow-insecure-cluster` is an explicit isolated
development override. Configured authentication also fails closed when the
binary was built without OpenSSL. Legacy one-message `AUTH_HANDSHAKE` payloads
are rejected.

The protocol authenticates peers but does **not** encrypt RPC payloads. Run RPC
ports only on a private network protected by security groups/network policy and
use an encrypted overlay or service mesh when traffic can cross an untrusted
network. Native RPC TLS/mTLS remains planned; the mTLS path fields in
`RpcSecurityConfig` are not connected to TCP sockets. Secret changes are not
hot-reloaded: rotate the secret through a coordinated restart of all cluster
processes so pooled connections are rebuilt.

---

## Query Timeout & Kill

### Automatic Timeout

```yaml
server:
  query_timeout_ms: 30000    # auto-cancel after 30s (0 = disabled)
```

Timed-out queries return:
```json
{"error": "Query cancelled (timeout 30000ms)"}
```

Cancellation is cooperative. The HTTP worker waits for local execution to
observe the token and finish, and distributed RPC work does not yet receive
that token. Treat this as an operator cancellation mechanism, not a hard
wall-clock or memory-isolation boundary.

### Manual Kill

List active queries:
```bash
curl http://localhost:8123/admin/queries -H "Authorization: Bearer $ADMIN_KEY"
```

```json
[{
  "query_id": "q_a1b2c3",
  "subject": "ak_7f3k",
  "sql": "SELECT * FROM trades WHERE ...",
  "started_at_ns": 1712019000000000000,
  "duration_ms": 5200
}]
```

Kill a query:
```bash
curl -X DELETE http://localhost:8123/admin/queries/q_a1b2c3 \
  -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{"cancelled": true}
```

### Cancellation Mechanism

Queries check a `CancellationToken` at partition boundaries during scan. Cancelled queries release resources immediately and return an error to the client.

---

## Secrets Management

### Provider Priority Chain

```
1. HashiCorp Vault KV v2     (if VAULT_ADDR set)
2. AWS Secrets Manager        (if AWS credentials available)
3. File secrets               (/run/secrets/ — Docker/K8s mounts)
4. Environment variables      (fallback)
```

### HashiCorp Vault

```yaml
secrets:
  vault:
    enabled: true
    address: https://vault.internal:8200
    mount_path: secret/data/zeptodb
    token_path: /var/run/secrets/vault/token    # or VAULT_TOKEN env
```

Store secrets:
```bash
vault kv put secret/zeptodb JWT_SECRET=my-secret OIDC_CLIENT_SECRET=xxx
```

Reference in config:
```yaml
auth:
  jwt:
    hs256_secret: ${JWT_SECRET}
  oidc:
    client_secret: ${OIDC_CLIENT_SECRET}
```

### AWS Secrets Manager

```yaml
secrets:
  aws:
    enabled: true
    region: us-east-1
    prefix: zeptodb/
```

Requires IAM role with `secretsmanager:GetSecretValue` permission.

### Kubernetes Secrets

Mount as files:
```yaml
volumes:
  - name: zepto-secrets
    secret:
      secretName: zeptodb-secrets
volumeMounts:
  - name: zepto-secrets
    mountPath: /run/secrets
    readOnly: true
```

ZeptoDB auto-detects `/run/secrets/` and reads files as key-value pairs (filename = key, content = value).

### Environment Variables

```bash
export JWT_SECRET=my-secret
export OIDC_CLIENT_SECRET=xxx
./zepto_http_server --config config.yaml
```

`${VAR}` references in YAML are resolved through the full provider chain.

---

## Arrow Flight Security

Arrow Flight (gRPC, port 8815) validates per-RPC `authorization` metadata
through the shared `AuthManager`. `GetFlightInfo`, `DoGet`, and `ListFlights`
require `READ`; `DoPut` requires `WRITE` and is disabled by default because its
current compatibility implementation is non-atomic. Query tickets are intentionally
limited to `SELECT`, `SHOW TABLES`, and `DESCRIBE`, so DDL/DML is rejected even
for administrators. Table allowlists and tenant namespaces cover every
physical table reached through JOINs, sequential CTEs, subqueries, and set
operations. `SHOW TABLES` and `ListFlights` are filtered to visible tables.

The CLI shares one `TenantManager` between HTTP and Flight. Flight read RPCs
enforce tenant concurrency slots and a 30-second cooperative timeout by
default. Request-owned bounded ASTs bypass shared SQL caches, preventing cache
priming, collision, and concurrent policy replacement. The
`--allow-non-atomic-put` flag is an experimental test-only override; a later
row/batch error can leave earlier rows committed.

`ping`, `healthcheck`, and `ListActions` remain public for health probes.
Missing/invalid credentials map to Flight `Unauthenticated`; authenticated
role, rate, table, and tenant denials map to Flight `Unauthorized`. Identity is
never accepted from caller-supplied `X-Zepto-*` metadata.

```python
# Python client with API key
import pyarrow.flight as fl

client = fl.connect("grpc://127.0.0.1:8815")
options = fl.FlightCallOptions(headers=[
    (b"authorization", b"Bearer zepto_abc123"),
])
reader = client.do_get(fl.Ticket("SELECT * FROM trades"), options)
```

The listener defaults to `127.0.0.1`. A non-loopback bind fails closed unless
TLS is configured; the explicit `--allow-insecure-flight` override is for
development only. It does not authorize plaintext for the bundled HTTP
listener, which has a separate `--allow-plaintext-http` development override.
Production startup:

```bash
./zepto_flight_server \
  --flight-host 0.0.0.0 \
  --flight-port 8815 \
  --tls-cert /etc/zeptodb/tls/server.crt \
  --tls-key /etc/zeptodb/tls/server.key \
  --api-keys-file /etc/zeptodb/api_keys.txt
```

```python
# Python client with TLS
client = fl.connect(
    "grpc+tls://zepto.internal:8815",
    tls_root_certs=open("ca.crt", "rb").read()
)
```

The certificate and private key must be provided together as readable,
non-empty PEM files. Tenant-scoped identities fail closed if the Flight
embedding has no matching `TenantManager` configuration. See
[`FLIGHT_REFERENCE.md`](FLIGHT_REFERENCE.md) for the complete RPC contract.

---

## Compliance Notes

### SOC2

| Control | Implementation |
|---------|---------------|
| Access control | RBAC (5 roles) + table/tenant ACL; symbol scopes fail closed |
| Authentication | API Key (SHA-256) + JWT/OIDC + SSO |
| Audit trail | Authenticated events are emitted; external immutable retention and control validation are operator responsibilities |
| Encryption in transit | HTTP TLS or a TLS-terminating ingress; RPC additionally needs an encrypted overlay |
| Secrets management | Vault/AWS SM/K8s — no plaintext secrets |

### EMIR / MiFID II

| Requirement | Implementation |
|-------------|---------------|
| Trade data retention | Operator-defined HDB/audit export and retention policy; not automatic certification evidence |
| Access logging | Structured JSON access log + audit buffer |
| Data integrity | Partial snapshot/recovery support; crash-durable SQL WAL proof remains a blocker |
| Segregation of duties | RBAC roles separate read/write/admin |

### GDPR

| Requirement | Implementation |
|-------------|---------------|
| Data minimization | TTL + storage tiering (auto-drop after N days) |
| Right to erasure | `DELETE FROM table WHERE ...` + HDB compaction |
| Access logging | Audit trail tracks who accessed what |

---

*See also: [SSO Integration Guide](SSO_INTEGRATION_GUIDE.md) · [HTTP Reference](HTTP_REFERENCE.md) · [Config Reference](CONFIG_REFERENCE.md) · [Production Deployment](../deployment/PRODUCTION_DEPLOYMENT.md)*
