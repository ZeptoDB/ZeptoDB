# ZeptoDB: Security & Access Control Layer (Layer 5)

**Version:** 2.0
**Last updated:** 2026-03-22
**Status:** ✅ Fully Implemented (all contract-blocker features complete)
**Related code:** `include/zeptodb/auth/`, `src/auth/`, `tests/unit/test_auth.cpp`

---

## 1. Overview

ZeptoDB's security layer enforces authentication, authorization, and audit logging
across all HTTP API endpoints. It is designed to meet the requirements of financial
institutions operating under EMIR, MiFID II, and SOC2 frameworks.

### Security pillars

| Pillar | Implementation | Status |
|--------|---------------|--------|
| **Transport Encryption (TLS)** | httplib::SSLServer, OpenSSL 3.2 | ✅ |
| **Authentication** | API Key (Bearer) + JWT/OIDC | ✅ |
| **Authorization (RBAC)** | 5-role model + symbol-level ACL | ✅ |
| **Rate Limiting** | Token bucket, per-identity + per-IP | ✅ |
| **Admin REST API** | Key/query/audit/version management | ✅ |
| **Query Timeout & Kill** | CancellationToken + QueryTracker | ✅ |
| **Secrets Management** | Vault > File > Env provider chain | ✅ |
| **Audit Logging** | spdlog file + in-memory ring buffer | ✅ |
| **Key Management** | SHA256-hashed store, rotation API | ✅ |

### Architecture

```
Client (HTTPS)
    │
    ▼
┌────────────────────────────────────────────────────────────────┐
│  HttpServer (port 8443 / HTTPS)                                │
│  set_pre_routing_handler ───► AuthManager::check()             │
│       │                           │                            │
│       │               ┌───────────┴───────────┐               │
│       │               ▼                       ▼               │
│       │         JwtValidator           ApiKeyStore             │
│       │         (HS256/RS256)          (SHA256 hash)           │
│       │               │                       │               │
│       │         JwtClaims             ApiKeyEntry              │
│       │               └───────────┬───────────┘               │
│       │                           ▼                           │
│       │                      AuthContext                       │
│       │                   (role, symbols)                      │
│       │                           │                           │
│       │                    RateLimiter::check()                │
│       │                  (token bucket per identity/IP)        │
│       │                           │                           │
│       ▼                           ▼                           │
│  Route Handler ◄──── RbacEngine::has_permission()             │
│       │                                                        │
│  SQL → run_query_with_tracking()                               │
│       │                                                        │
│  QueryTracker::register()  ──► CancellationToken              │
│  std::async + wait_for(timeout_ms)                             │
│  QueryExecutor::execute(sql, token)                            │
│       │                                                        │
│  AuditLogger (spdlog → audit.log + AuditBuffer ring)          │
│  GET /admin/audit → AuditBuffer::last(n) → JSON               │
└────────────────────────────────────────────────────────────────┘

Secrets bootstrap:
  SecretsProviderFactory::create_composite()
    → VaultSecretsProvider (if VAULT_ADDR/VAULT_TOKEN set)
    → FileSecretsProvider  (/run/secrets/ Docker/K8s)
    → EnvSecretsProvider   (always available, final fallback)
```

---

## 2. Transport Layer Security (TLS/SSL)

### 2.1 Configuration

TLS is configured via `TlsConfig` passed to `HttpServer`:

```cpp
zeptodb::auth::TlsConfig tls;
tls.enabled    = true;
tls.cert_path  = "/etc/zeptodb/tls/server.crt";  // PEM
tls.key_path   = "/etc/zeptodb/tls/server.key";  // PEM
tls.https_port = 8443;
tls.also_serve_http = false;  // disable unencrypted HTTP in production

auto server = zeptodb::server::HttpServer(executor, 8123, tls, auth_manager);
```

Compile-time activation: when OpenSSL is detected, CMake sets `APEX_TLS_ENABLED=1`
and links `OpenSSL::SSL OpenSSL::Crypto`. Without this flag, the server silently
falls back to HTTP (with a startup warning).

### 2.2 Certificate Management

**Self-signed (development / internal):**
```bash
openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt \
  -days 365 -nodes -subj "/CN=zeptodb.internal"
```

**Production (Let's Encrypt / corporate CA):**
```bash
certbot certonly --standalone -d zeptodb.yourdomain.com
# certificates at /etc/letsencrypt/live/zeptodb.yourdomain.com/
```

**mTLS (inter-node cluster authentication):**
Each cluster node presents a client certificate signed by the APEX internal CA.
This prevents unauthorized nodes from joining the cluster.

```bash
# Generate CA
openssl genrsa -out ca.key 4096
openssl req -new -x509 -key ca.key -out ca.crt -days 3650 -subj "/CN=zeptodb-ca"

# Per-node certificate
openssl genrsa -out node-01.key 2048
openssl req -new -key node-01.key -out node-01.csr -subj "/CN=node-01"
openssl x509 -req -in node-01.csr -CA ca.crt -CAkey ca.key \
  -CAcreateserial -out node-01.crt -days 365
```

### 2.3 Certificate Rotation Policy (Enterprise)

| Scope | Rotation Frequency | Automation |
|-------|--------------------|------------|
| Server TLS cert | 90 days (Let's Encrypt) or 365 days (corporate CA) | certbot renew + service reload |
| mTLS node certs | 365 days | Ansible playbook |
| CA root cert | 10 years | Manual, dual-control sign-off |

**Rotation procedure (zero downtime):**
1. Generate new certificate (keep old running)
2. Deploy new cert to `/etc/zeptodb/tls/server.crt.new`
3. `systemctl reload zeptodb` (SIGHUP reloads TLS context)
4. Verify TLS handshake on new cert
5. Remove old cert backup

---

## 3. Authentication

### 3.1 API Keys

API keys are the primary authentication mechanism for service-to-service access
(trading systems, feed handlers, monitoring agents).

**Key format:**
```
zepto_<64-character lowercase hex>    (256-bit entropy via RAND_bytes)

Example: zepto_3f7a2b9d1e4c8f0a5b6d7e2c9a1b3f8e4d7c2a9b5e1f3d8a6b4c7e2f9a1b3d
```

**Security properties:**
- Only the SHA256 hash is stored — raw key never persisted
- Even with full key store read access, original keys cannot be recovered
- Each key has a unique short ID (`ak_<8-hex>`) for management without exposing the secret

**Key lifecycle:**
```
                ┌──────────────┐
                │ create_key() │  ← plaintext key returned ONCE
                └──────┬───────┘
                       │ SHA256(key) stored in key file
                       ▼
                ┌──────────────┐
                │   ACTIVE     │  ← validate() succeeds
                └──────┬───────┘
                       │ revoke(key_id)
                       ▼
                ┌──────────────┐
                │   REVOKED    │  ← validate() returns nullopt, entry preserved
                └──────────────┘
```

**Storage format** (`/etc/zeptodb/keys.conf`):
```
# zeptodb-keys-v1
# id|name|key_hash|role|symbols|enabled|created_at_ns
ak_3f7a2b9d|trading-desk-1|sha256:<64-hex>|reader|AAPL,GOOG|1|1742000000000000000
ak_1e4c8f0a|data-pipeline|sha256:<64-hex>|writer||1|1742000000000000000
ak_5b6d7e2c|prometheus|sha256:<64-hex>|metrics||1|1742000000000000000
```

**Rotation procedure:**
```bash
# 1. Create new key with same permissions
NEW_KEY=$(zepto-admin key create --name "trading-desk-1-v2" --role reader --symbols "AAPL,GOOG")

# 2. Deploy NEW_KEY to the client application

# 3. After client confirms new key works, revoke old key
zepto-admin key revoke ak_3f7a2b9d

# 4. Verify old key no longer works
curl -H "Authorization: Bearer <old_key>" https://zeptodb:8443/ping
# → 401 Unauthorized
```

### 3.2 JWT / SSO (OIDC)

JWT authentication enables Single Sign-On integration with enterprise identity providers
such as Okta, Azure Active Directory, Google Workspace, and Auth0.

**Supported algorithms:**

| Algorithm | Use case | Key material |
|-----------|----------|-------------|
| HS256 | Internal / simple setups | Shared secret (min 256-bit) |
| RS256 | Enterprise IdP (Okta, Azure, Google) | RSA public key (2048-bit min) |

**JWT payload claims:**

| Claim | Type | Required | Description |
|-------|------|----------|-------------|
| `sub` | string | ✅ | Subject (user ID, service account) |
| `exp` | integer | ✅ | Expiry (Unix seconds, validated) |
| `iss` | string | When configured | Issuer URL (e.g. `https://corp.okta.com`) |
| `aud` | string/array | When configured | Audience (e.g. `zeptodb`) |
| `zepto_role` | string | Recommended | Role assignment: admin/writer/reader/analyst/metrics |
| `zepto_symbols` | string | Optional | Comma-separated symbol whitelist: `AAPL,GOOG,MSFT` |
| `email` | string | Optional | Used for audit log display name |

**OIDC integration example (Okta):**
```cpp
JwtValidator::Config jwt_cfg;
jwt_cfg.rs256_public_key_pem = load_file("/etc/zeptodb/okta_public.pem");
jwt_cfg.expected_issuer      = "https://corp.okta.com/oauth2/default";
jwt_cfg.expected_audience    = "zeptodb";
jwt_cfg.role_claim           = "zepto_role";    // custom claim in Okta app
jwt_cfg.symbols_claim        = "zepto_symbols"; // custom claim in Okta app
```

**IdP configuration (Okta example):**
1. Create an API Services application in Okta
2. Add custom claims: `zepto_role` (string) and `zepto_symbols` (string)
3. Configure claim values per group: `Trading-Desk` → `reader`, `Quant-Research` → `analyst`
4. Export the RS256 public key from Okta JWKS endpoint
5. Configure ZeptoDB with the exported PEM

**Token rotation:**
- JWTs are short-lived (typically 1 hour). The IdP handles rotation transparently.
- ZeptoDB verifies `exp` on every request — no server-side token revocation needed.
- For immediate revocation: rotate the signing key in the IdP (all existing tokens become invalid).

---

## 4. Authorization (RBAC)

### 4.1 Role Model

ZeptoDB uses five predefined roles covering all access patterns for financial operations:

| Role | Permissions | Target User |
|------|-------------|-------------|
| `admin` | READ + WRITE + ADMIN + METRICS | DBA, system administrator |
| `writer` | READ + WRITE + METRICS | Trading systems, feed handlers, data pipelines |
| `reader` | READ | Quant researchers, BI dashboards |
| `analyst` | READ (symbol-restricted) | External analysts, team-scoped access |
| `metrics` | METRICS only | Prometheus, monitoring agents |

### 4.2 Permission Bitmask

```
Permission   Bit   Allows
─────────────────────────────────────────────────────────
READ         0     SELECT queries on all endpoints
WRITE        1     INSERT via HTTP API, ingest endpoints
ADMIN        2     DDL operations, user management APIs
METRICS      3     /metrics, /stats, /health, /ready endpoints
```

**Endpoint → required permission mapping:**

| Endpoint | Method | Required Permission |
|----------|--------|-------------------|
| `POST /` | SQL query | READ |
| `GET /?query=...` | SQL query | READ |
| `GET /metrics` | Prometheus | METRICS |
| `GET /stats` | Statistics | METRICS |
| `GET /ping` | Health | (public, no auth) |
| `GET /health` | Liveness | (public, no auth) |
| `GET /ready` | Readiness | (public, no auth) |
| `POST /admin/keys` | Key management | ADMIN |

### 4.3 Symbol-Level Access Control (Multi-Tenant)

For organizations with multiple trading desks or external analysts, ZeptoDB supports
per-identity symbol whitelisting.

**API Key with symbol restriction:**
```cpp
// Trading Desk A: can only query AAPL, GOOG
std::string key = auth->create_api_key(
    "desk-a-reader",
    Role::ANALYST,
    {"AAPL", "GOOG"}
);
```

**JWT with symbol restriction:**
```json
{
  "sub": "analyst@hedge-fund-client.com",
  "zepto_role": "analyst",
  "zepto_symbols": "AAPL,GOOG,MSFT",
  "exp": 9999999999
}
```

**Enforcement in query execution:**
The SQL executor checks `AuthContext::can_access_symbol(sym)` during partition selection.
Symbol-restricted identities only see results for their allowed symbols — other symbols
are filtered at the partition scan level (no timing side-channel).

**Empty symbol list = unrestricted** (admin/writer/reader roles default to all symbols).

### 4.4 Principle of Least Privilege

Recommended key assignment by service type:

| Service | Recommended Role | Symbol List |
|---------|-----------------|-------------|
| Market data feed handler | `writer` | All (empty) |
| Trading strategy process | `reader` | Strategy-specific symbols |
| Quant research notebook | `reader` | Research universe |
| External client analytics | `analyst` | Contracted symbols only |
| Prometheus scraper | `metrics` | N/A |
| DBA / operations | `admin` | All (empty) |
| Backup / ETL jobs | `writer` | All (empty) |

---

## 5. Audit Logging

### 5.1 Log Format

Every authenticated request is recorded to the audit log:

```
[2026-03-22T14:35:22.431] [AUDIT] subject=ak_3f7a2b9d role=reader action="POST /" detail="apikey-auth" from=10.0.1.45
[2026-03-22T14:35:23.117] [AUDIT] subject=user@corp.com role=analyst action="POST /" detail="jwt-auth" from=10.0.2.33
[2026-03-22T14:35:24.002] [AUDIT] subject=anonymous role=metrics action="GET /metrics" detail="public" from=10.0.0.1
```

**Fields:**
- `subject` — API key short ID or JWT `sub` claim
- `role` — resolved role at time of request
- `action` — HTTP method + path
- `detail` — authentication method used
- `from` — client IP address

### 5.2 Log Configuration

```cpp
AuthManager::Config cfg;
cfg.audit_enabled  = true;
cfg.audit_log_file = "/var/log/zeptodb/audit.log";  // empty = spdlog default
```

### 5.3 Log Retention Policy

| Log type | Retention | Storage |
|----------|-----------|---------|
| Audit log (auth events) | 7 years | S3 with Object Lock |
| Access log (all HTTP) | 90 days | Local + S3 |
| Error log | 30 days | Local |

**Rationale:** EMIR requires trade activity records for 5 years; MiFID II requires
investment firm records for 5–7 years. Audit logs establish who queried what data,
supporting regulatory inspection requests.

### 5.4 Regulatory Compliance Mapping

| Regulation | Requirement | ZeptoDB Implementation |
|-----------|-------------|----------------------|
| **EMIR** | Transaction record retention (5 years) | Audit log + WAL archival to S3 |
| **MiFID II Article 25** | Investment firm record keeping (7 years) | 7-year audit log retention |
| **MiFID II Article 17** | Algorithmic trading audit trail | Per-request identity logging |
| **Basel IV** | Risk data aggregation auditability | Audit log + RBAC enforcement |
| **SOC2 Type II** | Access control evidence | RBAC roles + API key lifecycle |
| **ISO 27001 A.9** | User access management | Role-based + symbol ACL |

---

## 6. Enterprise Governance Policies

### 6.1 Access Review Cadence

| Review type | Frequency | Owner | Action |
|------------|-----------|-------|--------|
| API key inventory | Monthly | Security team | Revoke unused keys (last_used > 30 days) |
| Role assignment review | Quarterly | Department heads | Confirm RBAC roles are still appropriate |
| Admin account audit | Monthly | CISO | Verify admin key count ≤ 3 |
| Symbol ACL review | On change | Team lead | Confirm analyst symbol lists |
| JWT claim mapping | On IdP changes | IAM team | Re-verify `zepto_role` claim values |

**Inactive key cleanup script:**
```bash
# Revoke all keys not used in 30 days
zepto-admin key list --format json | \
  jq -r '.[] | select(.last_used_days > 30) | .id' | \
  xargs -I{} zepto-admin key revoke {}
```

### 6.2 Separation of Duties

| Function | Allowed roles | Prohibited combination |
|----------|--------------|----------------------|
| Create API keys | admin only | reader/writer/analyst cannot self-issue keys |
| Revoke API keys | admin only | Key owners cannot revoke their own keys |
| View all keys | admin only | Other roles cannot list keys (key hash not exposed) |
| Execute queries | reader, writer, analyst, admin | metrics role cannot execute SQL |
| Ingest data | writer, admin | reader/analyst cannot write data |

### 6.3 Security Hardening Checklist

**Network:**
- [ ] Disable unencrypted HTTP (`also_serve_http = false`) in production
- [ ] Firewall: allow port 8443 from trusted CIDR only
- [ ] mTLS for cluster inter-node communication
- [ ] VPC-only deployment (no public internet exposure)

**Key management:**
- [ ] Store `keys.conf` with permissions `600` (root or zeptodb service user only)
- [ ] Rotate all API keys at 90-day intervals
- [ ] Never log raw API keys — log short ID only
- [ ] Admin key count ≤ 3 (principle of least privilege)
- [ ] JWT HS256 secret: minimum 256-bit entropy, stored in HSM or secrets manager

**Operational:**
- [ ] Audit log shipped to immutable storage (S3 Object Lock) in real-time
- [ ] Alert on failed authentication spikes (> 100 failures in 5 minutes)
- [ ] Alert on new admin key creation (always unusual)
- [ ] Enable Prometheus `zepto_auth_failures_total` metric (todo: add metric)

### 6.4 Incident Response Procedures

**Suspected API key compromise:**
1. Immediately revoke the suspected key: `zepto-admin key revoke <key_id>`
2. Review audit log for all requests made by that key: `grep "subject=<key_id>" audit.log`
3. Issue a new key with same permissions to the legitimate owner
4. Notify security team and document the incident

**Suspected JWT secret compromise (HS256):**
1. Rotate the HS256 secret in AuthManager config immediately
2. Restart ZeptoDB to apply the new secret (all existing tokens become invalid)
3. Notify all SSO users to re-authenticate
4. Review audit log for anomalous access patterns in the past 1 hour

**Suspected RS256 private key compromise:**
1. Immediately rotate the signing key in the IdP (Okta/Azure AD)
2. Download the new JWKS/PEM public key
3. Update `rs256_public_key_pem` in AuthManager config
4. Reload ZeptoDB config
5. Existing tokens signed by old key are now invalid

**Unauthorized access attempt:**
1. Identify source IP from audit log
2. Block IP at firewall level
3. Review for lateral movement (other services accessed from same IP)
4. If inside VPC: escalate to incident response team

### 6.5 Multi-Tenant Isolation Model

For managed service operators running ZeptoDB shared infrastructure:

```
Tenant A (Hedge Fund Alpha)
  ├── API key: ak_a1b2 — role=reader — symbols=[AAPL, GOOG, MSFT]
  └── API key: ak_c3d4 — role=writer — symbols=[AAPL, GOOG, MSFT]

Tenant B (Crypto Desk)
  ├── API key: ak_e5f6 — role=reader — symbols=[BTC, ETH, SOL]
  └── API key: ak_g7h8 — role=analyst — symbols=[BTC]

Internal (Ops)
  └── API key: ak_i9j0 — role=admin — symbols=[] (unrestricted)
```

**Isolation guarantees:**
- Symbol ACL enforced at partition scan level — not application logic
- Tenant A cannot observe Tenant B's symbols (even if they know the partition key structure)
- `admin` role is only issued to internal ops accounts, never tenants

---

## 7. Configuration Reference

### 7.1 Full AuthManager::Config

```cpp
AuthManager::Config cfg;

// Master switch (set false ONLY for isolated development)
cfg.enabled = true;

// --- API Key ---
cfg.api_keys_file = "/etc/zeptodb/keys.conf";

// --- JWT/SSO ---
cfg.jwt_enabled = true;
cfg.jwt.hs256_secret           = getenv("APEX_JWT_SECRET");   // or ""
cfg.jwt.rs256_public_key_pem   = load_file("/etc/zeptodb/idp_public.pem"); // or ""
cfg.jwt.expected_issuer        = "https://corp.okta.com/oauth2/default";
cfg.jwt.expected_audience      = "zeptodb";
cfg.jwt.verify_expiry          = true;
cfg.jwt.role_claim             = "zepto_role";
cfg.jwt.symbols_claim          = "zepto_symbols";

// --- Audit ---
cfg.audit_enabled  = true;
cfg.audit_log_file = "/var/log/zeptodb/audit.log";

// --- Public paths (no auth required) ---
cfg.public_paths = {"/ping", "/health", "/ready"};
```

### 7.2 TlsConfig

```cpp
TlsConfig tls;
tls.enabled         = true;
tls.cert_path       = "/etc/zeptodb/tls/server.crt";
tls.key_path        = "/etc/zeptodb/tls/server.key";
tls.https_port      = 8443;
tls.also_serve_http = false;  // true only during migration window
```

### 7.3 Environment Variable Secrets

Never hardcode secrets in config files. Use environment variables:

```bash
export APEX_JWT_SECRET="$(openssl rand -hex 32)"
export APEX_TLS_CERT_PATH="/etc/zeptodb/tls/server.crt"
export APEX_TLS_KEY_PATH="/etc/zeptodb/tls/server.key"
```

For Kubernetes deployments, store secrets in `Secret` objects:
```yaml
apiVersion: v1
kind: Secret
metadata:
  name: zeptodb-secrets
data:
  jwt-secret: <base64-encoded>
  tls.crt: <base64-encoded>
  tls.key: <base64-encoded>
```

---

## 8. Implementation Details

### 8.1 File Structure

```
include/zeptodb/auth/
  rbac.h            — Role enum, Permission bitmask, role_to_string() (header-only)
  api_key_store.h   — ApiKeyEntry, ApiKeyStore class
  jwt_validator.h   — JwtClaims, JwtValidator class (HS256 + RS256)
  auth_manager.h    — AuthContext, AuthDecision, AuthManager, TlsConfig

src/auth/
  api_key_store.cpp — SHA256(OpenSSL), RAND_bytes, file I/O
  jwt_validator.cpp — JWT parsing, HMAC verify (OpenSSL), EVP_DigestVerify
  auth_manager.cpp  — Middleware logic, JWT priority, spdlog audit
```

### 8.2 Request Authentication Flow

```
1. HttpServer receives request
2. set_pre_routing_handler fires before any route handler
3. AuthManager::check(method, path, Authorization header, remote_addr)
   a. is_public_path(path)? → OK (anonymous/metrics role)
   b. No Authorization header? → 401
   c. Extract Bearer token
   d. Token starts with "ey"? → Try JwtValidator first
   e. JwtValidator fails or JWT not configured? → Try ApiKeyStore
   f. Both fail? → 401
4. Decision.status == OK → continue to route handler
5. Decision.status != OK → return 401/403 with JSON error
6. Audit log written for all authenticated requests
```

### 8.3 Performance Impact

Auth overhead is measured in microseconds and does not affect the critical ingest path
(auth only applies to HTTP API, not the internal tick plant pipeline).

| Operation | Latency |
|-----------|---------|
| API key validation (SHA256 lookup) | ~1μs |
| JWT HS256 verification | ~2μs |
| JWT RS256 verification | ~50μs |
| Public path bypass | ~100ns |

RS256 is expensive (RSA modular exponentiation). For latency-sensitive paths, use HS256
or API keys. RS256 is acceptable for interactive research queries but not recommended
for HFT trading system authentication.

---

## 9. Design Rationale

### Why SHA256-hashed key storage?
If the key store file is leaked (disk backup, accidental exposure), an attacker cannot
derive any valid credentials. This is the same design used by GitHub PATs and Stripe API keys.

### Why not bcrypt for API keys?
bcrypt is designed for password storage (high work factor, slow). API key lookup is
per-request — bcrypt (~100ms) would be prohibitive. SHA256 is appropriate because
API keys have high entropy (256-bit random), making brute-force infeasible regardless
of hash speed.

### Why JWT priority over API key?
JWTs carry signed identity claims from an enterprise IdP (SSO). They represent an
authenticated human user. API keys represent service-to-service auth. The auth priority
order (JWT > API key) ensures that when a human SSO token is presented, it is used
for proper identity attribution in audit logs.

### Why header-only RBAC?
The permission model is a pure function of the role — no I/O, no allocations. Keeping
it header-only allows the compiler to inline and optimize all `has_permission()` calls.
For the hot path (query execution), this results in zero overhead compared to
passing permissions by pointer or virtual dispatch.

---

## 10. Rate Limiting

Token bucket algorithm — per identity and per IP address.

### Configuration

```cpp
AuthManager::Config cfg;
cfg.rate_limit_enabled             = true;
cfg.rate_limit.requests_per_minute = 1000;  // per API key / JWT sub
cfg.rate_limit.burst_capacity      = 200;   // max burst
cfg.rate_limit.per_ip_rpm          = 10000; // per source IP (DDoS protection)
cfg.rate_limit.ip_burst            = 500;
```

### Behavior

- Each identity has its own token bucket (independent limits).
- Tokens refill at `rpm / 60 / 1e9` per nanosecond.
- When tokens < 1.0, request is rejected with `403 Forbidden` and reason `"Rate limit exceeded"`.
- Rate limit check occurs **after** successful authentication in `AuthManager::check()`.
- `RateLimiter::cleanup(max_idle_sec)` removes stale buckets (call periodically).

### Response

```
HTTP/1.1 403 Forbidden
{"error":"Rate limit exceeded"}
```

---

## 11. Admin REST API

All admin endpoints require `ADMIN` role. Auth is checked inline in each handler.

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/admin/keys` | Create API key (returns plaintext once) |
| `GET` | `/admin/keys` | List all API keys with metadata |
| `DELETE` | `/admin/keys/:id` | Revoke API key by id |
| `GET` | `/admin/queries` | List currently running queries |
| `DELETE` | `/admin/queries/:id` | Cancel a running query |
| `GET` | `/admin/audit` | Recent audit events (`?n=100`) |
| `GET` | `/admin/version` | Server version info |

### Create API key

```
POST /admin/keys
Authorization: Bearer <admin_key>
Content-Type: application/json

{"name":"algo-service","role":"writer"}

→ {"key":"zepto_<64-hex>"}  (shown once, store securely)
```

### List active queries

```
GET /admin/queries
→ [{"id":"q_a1b2c3...","subject":"svc-key-id","sql":"SELECT * FROM ...","started_at_ns":...}]
```

### Cancel a query

```
DELETE /admin/queries/q_a1b2c3...
→ {"cancelled":true}
```

### Audit log

```
GET /admin/audit?n=50
→ [{"ts":1711120800000000000,"subject":"svc-key-id","role":"writer","action":"POST /",
    "detail":"apikey-auth","from":"10.0.0.1"}, ...]
```

---

## 12. Query Timeout & Cancellation

### Architecture

```
HttpServer::run_query_with_tracking(sql, subject)
    │
    ├─ QueryTracker::register_query(subject, sql, token)  → query_id "q_<hex>"
    │
    ├─ if timeout_ms > 0:
    │     std::async → QueryExecutor::execute(sql, token.get())
    │     future.wait_for(timeout_ms)
    │     if timeout: token->cancel(); future.wait()
    │
    ├─ QueryExecutor::execute(sql, token)
    │     sets thread_local CancellationToken*
    │     checks is_cancelled() at each partition boundary
    │     if cancelled: returns QueryResultSet{error="Query cancelled"}
    │
    └─ QueryTracker::complete(query_id)
```

### Configuration

```cpp
HttpServer server(executor, 8123, tls_cfg, auth_mgr);
server.set_query_timeout_ms(30000);  // 30 second timeout
```

### Cancel via admin API

```
DELETE /admin/queries/q_a1b2c3...
```
This sets the `CancellationToken` — the running query aborts at the next partition boundary
and returns HTTP 408 with `{"error":"Query cancelled"}`.

---

## 13. Secrets Management

Provider chain (priority order):

1. **VaultSecretsProvider** — HashiCorp Vault KV v2 (`VAULT_ADDR` + `VAULT_TOKEN` env vars)
2. **FileSecretsProvider** — filesystem secrets at `/run/secrets/` (Docker/K8s mounted secrets)
3. **EnvSecretsProvider** — environment variables (final fallback, always available)

### Usage

```cpp
// Auto-detect based on env vars
auto secrets = SecretsProviderFactory::create_composite();
std::string jwt_secret = secrets->get("JWT_SECRET", "");
std::string db_pass    = secrets->get("DB_PASSWORD", "");

// Which backends are active?
for (const auto& name : secrets->active_backends())
    spdlog::info("secrets backend: {}", name);
```

### Vault integration

```
VAULT_ADDR=https://vault.internal:8200
VAULT_TOKEN=s.xxx

# Key lookup path: /v1/{mount}/data/{key}
# Response parsed: {"data":{"data":{"value":"<secret>"}}}
```

### K8s / Docker integration

Mount Kubernetes Secret or Docker secret to `/run/secrets/`:
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

### AWS Secrets Manager

Full SigV4 signing is not yet implemented. For AWS deployments:
- Use the **ExternalSecrets Operator** which writes AWS SM values to K8s Secrets → FileSecretsProvider reads them.
- Or use the AWS CLI subprocess approach in K8s IRSA environments.

---

*Last updated: 2026-03-22 | Related: architecture_design.md, PRODUCTION_OPERATIONS.md*
