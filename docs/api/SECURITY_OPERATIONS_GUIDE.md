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
/auth/login    /auth/callback    /auth/logout
```

### Disabling Auth (development only)

```yaml
auth:
  enabled: false    # WARNING: all requests get admin access
```

---

## API Key Management

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
| `analyst` | ✅* | ❌ | ❌ | ❌ | External users (symbol-restricted) |
| `metrics` | ❌ | ❌ | ❌ | ✅ | Prometheus scraper |

*`analyst` can only SELECT from symbols in their `allowed_symbols` whitelist.

### Permission Mapping

| Permission | Endpoints |
|-----------|-----------|
| READ | `POST /` (SELECT), `GET /stats` |
| WRITE | `POST /` (INSERT, UPDATE, DELETE) |
| ADMIN | `/admin/*` (keys, queries, audit, tenants, nodes, settings) |
| METRICS | `GET /metrics`, `GET /health`, `GET /stats` |

### Symbol-Level ACL

Both API keys and JWT tokens can carry a symbol whitelist:

```json
// API key creation
{"name": "external-analyst", "role": "analyst", "symbols": ["AAPL", "GOOGL"]}

// JWT claim
{"sub": "analyst@partner.com", "zepto_symbols": "AAPL,GOOGL"}
```

Queries referencing symbols outside the whitelist return 403.

### Table-Level ACL

```json
{"name": "desk-a-reader", "role": "reader", "tables": ["trades", "quotes"]}
```

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

Rate limiting is applied after authentication — unauthenticated requests are rejected before rate check.

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
| `max_memory_bytes` | 0 (unlimited) | Per-tenant memory cap |
| `max_queries_per_minute` | 0 (global limit) | Per-tenant rate limit |
| `max_ingestion_rate` | 0 (unlimited) | Ticks/sec cap |
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
# Test
curl --cacert ca.crt https://zepto.internal:8443/ping
```

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

```bash
# Client with cert
curl --cert client.crt --key client.key --cacert ca.crt \
  https://zepto.internal:8443/ping
```

mTLS is independent of Bearer token auth — both can be required simultaneously for defense-in-depth.

### Certificate Rotation

Replace cert/key files and restart the server. For zero-downtime rotation in Kubernetes, use cert-manager with auto-reload.

### Inter-Node RPC Security

Cluster nodes authenticate via shared-secret HMAC:

```yaml
cluster:
  rpc_security:
    enabled: true
    shared_secret: ${RPC_SHARED_SECRET}    # use Vault/env
```

mTLS for inter-node RPC is planned (config structure prepared in `RpcSecurityConfig`).

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

Arrow Flight (gRPC, port 8815) inherits the same auth as the HTTP API:

```bash
# Python client with API key
import pyarrow.flight
client = pyarrow.flight.connect(
    "grpc://localhost:8815",
    generic_options=[("authorization", "Bearer zepto_abc123")]
)
```

TLS for Flight:
```bash
./zepto_flight_server --port 8815 \
  --tls-cert /etc/zeptodb/server.crt \
  --tls-key /etc/zeptodb/server.key
```

```python
# Python client with TLS
client = pyarrow.flight.connect(
    "grpc+tls://zepto.internal:8815",
    tls_root_certs=open("ca.crt", "rb").read()
)
```

---

## Compliance Notes

### SOC2

| Control | Implementation |
|---------|---------------|
| Access control | RBAC (5 roles) + symbol/table ACL |
| Authentication | API Key (SHA-256) + JWT/OIDC + SSO |
| Audit trail | Every auth event logged with timestamp, subject, action, IP |
| Encryption in transit | TLS 1.2+ (OpenSSL 3.2) |
| Secrets management | Vault/AWS SM/K8s — no plaintext secrets |

### EMIR / MiFID II

| Requirement | Implementation |
|-------------|---------------|
| Trade data retention | Audit log + HDB (Parquet on S3, 7-year retention) |
| Access logging | Structured JSON access log + audit buffer |
| Data integrity | WAL + snapshot recovery |
| Segregation of duties | RBAC roles separate read/write/admin |

### GDPR

| Requirement | Implementation |
|-------------|---------------|
| Data minimization | TTL + storage tiering (auto-drop after N days) |
| Right to erasure | `DELETE FROM table WHERE ...` + HDB compaction |
| Access logging | Audit trail tracks who accessed what |

---

*See also: [SSO Integration Guide](SSO_INTEGRATION_GUIDE.md) · [HTTP Reference](HTTP_REFERENCE.md) · [Config Reference](CONFIG_REFERENCE.md) · [Production Deployment](../deployment/PRODUCTION_DEPLOYMENT.md)*
