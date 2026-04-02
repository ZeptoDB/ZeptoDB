# ZeptoDB SSO Integration Guide

This guide covers connecting ZeptoDB to OIDC identity providers for Single Sign-On.

---

## Quick Start (OIDC Discovery)

The simplest setup — one flag auto-configures everything:

```bash
./zepto_http_server --port 8123 \
  --oidc-issuer https://dev-123456.okta.com/oauth2/default \
  --oidc-client-id 0oa1bcdef2ghijk3l4m5 \
  --oidc-client-secret YOUR_CLIENT_SECRET \
  --oidc-redirect-uri http://localhost:8123/auth/callback
```

ZeptoDB fetches `/.well-known/openid-configuration` and auto-configures:
- JWKS endpoint (key rotation)
- Authorization endpoint (SSO login redirect)
- Token endpoint (code exchange)

Users can then:
1. Open the Web UI → click "Sign in with SSO"
2. Authenticate at the IdP
3. Get redirected back with a session cookie

---

## Supported Providers

| Provider | OIDC Discovery | Group Claim | Tested |
|----------|---------------|-------------|--------|
| Okta | ✅ | `groups` | ✅ |
| Azure AD (Entra ID) | ✅ | `groups` (GUID) or `roles` | ✅ |
| Google Workspace | ✅ | N/A (use directory API) | ✅ |
| Keycloak | ✅ | `groups` or `realm_access.roles` | ✅ |
| Auth0 | ✅ | `https://zeptodb.io/roles` (custom) | ✅ |
| AWS Cognito | ✅ | `cognito:groups` | ✅ |

---

## Provider Setup

### Okta

1. In Okta Admin → Applications → Create App Integration
2. Select "OIDC - OpenID Connect" → "Web Application"
3. Configure:
   - Sign-in redirect URI: `http://localhost:8123/auth/callback`
   - Sign-out redirect URI: `http://localhost:8123/login`
   - Assignments: assign users/groups
4. Note the Client ID and Client Secret
5. Enable Groups claim:
   - Security → API → Authorization Servers → default → Claims
   - Add claim: name=`groups`, value type=Groups, filter=Matches regex `.*`

```bash
./zepto_http_server --port 8123 \
  --oidc-issuer https://dev-123456.okta.com/oauth2/default \
  --oidc-client-id 0oa1bcdef2ghijk3l4m5 \
  --oidc-client-secret YOUR_SECRET \
  --oidc-redirect-uri http://localhost:8123/auth/callback
```

Group mapping (in code or config):
```
ZeptoDB-Admins  → admin
Trading-Desk    → writer
Quant-Research  → reader
```

### Azure AD (Microsoft Entra ID)

1. Azure Portal → App registrations → New registration
2. Redirect URI: `http://localhost:8123/auth/callback` (Web)
3. Certificates & secrets → New client secret
4. Token configuration → Add groups claim → Security groups
5. Note: Azure sends group GUIDs, not names. Map GUIDs to roles.

```bash
./zepto_http_server --port 8123 \
  --oidc-issuer https://login.microsoftonline.com/TENANT_ID/v2.0 \
  --oidc-client-id APP_CLIENT_ID \
  --oidc-client-secret APP_SECRET \
  --oidc-redirect-uri http://localhost:8123/auth/callback \
  --oidc-audience api://APP_CLIENT_ID
```

Group mapping (Azure uses GUIDs):
```
a1b2c3d4-...  → admin    # "ZeptoDB Admins" group GUID
e5f6g7h8-...  → writer   # "Trading Desk" group GUID
```

### Google Workspace

1. Google Cloud Console → APIs & Services → Credentials → Create OAuth client ID
2. Application type: Web application
3. Authorized redirect URI: `http://localhost:8123/auth/callback`
4. Enable "Google Workspace" domain-wide delegation if needed

```bash
./zepto_http_server --port 8123 \
  --oidc-issuer https://accounts.google.com \
  --oidc-client-id CLIENT_ID.apps.googleusercontent.com \
  --oidc-client-secret GOCSPX_SECRET \
  --oidc-redirect-uri http://localhost:8123/auth/callback
```

Note: Google does not include groups in the ID token by default. Use `zepto_role` custom claim via Google Workspace Admin SDK, or assign roles via the ZeptoDB API key system alongside SSO.

### Keycloak

1. Keycloak Admin → Clients → Create client
2. Client type: OpenID Connect
3. Valid redirect URIs: `http://localhost:8123/auth/callback`
4. Client authentication: On (confidential)
5. Add group mapper: Client scopes → dedicated scope → Add mapper → Group Membership
   - Token claim name: `groups`
   - Full group path: Off

```bash
./zepto_http_server --port 8123 \
  --oidc-issuer https://keycloak.example.com/realms/zeptodb \
  --oidc-client-id zeptodb \
  --oidc-client-secret KEYCLOAK_SECRET \
  --oidc-redirect-uri http://localhost:8123/auth/callback
```

### Auth0

1. Auth0 Dashboard → Applications → Create Application → Regular Web Application
2. Allowed Callback URLs: `http://localhost:8123/auth/callback`
3. Create a Rule or Action to add roles to the token:

```javascript
// Auth0 Action: Add roles to ID token
exports.onExecutePostLogin = async (event, api) => {
  const roles = event.authorization?.roles || [];
  api.idToken.setCustomClaim('https://zeptodb.io/roles', roles);
};
```

```bash
./zepto_http_server --port 8123 \
  --oidc-issuer https://YOUR_DOMAIN.auth0.com/ \
  --oidc-client-id AUTH0_CLIENT_ID \
  --oidc-client-secret AUTH0_SECRET \
  --oidc-redirect-uri http://localhost:8123/auth/callback
```

### AWS Cognito

1. Cognito → User Pools → Create/select pool
2. App integration → Create app client (Confidential client)
3. Hosted UI: add callback URL `http://localhost:8123/auth/callback`
4. Create groups in Cognito (e.g., `zeptodb-admins`, `zeptodb-writers`)

```bash
./zepto_http_server --port 8123 \
  --oidc-issuer https://cognito-idp.us-east-1.amazonaws.com/us-east-1_POOLID \
  --oidc-client-id COGNITO_CLIENT_ID \
  --oidc-client-secret COGNITO_SECRET \
  --oidc-redirect-uri http://localhost:8123/auth/callback
```

Cognito group claim: `cognito:groups`

---

## Authentication Flows

### Flow 1: Web UI SSO (recommended for humans)

```
Browser → GET /auth/login
       → 302 Redirect to IdP
       → User authenticates at IdP
       → 302 Redirect to /auth/callback?code=...
       → Server exchanges code for tokens
       → Server creates session (Set-Cookie: zepto_sid=...)
       → 302 Redirect to /query
       → Subsequent requests use session cookie
```

### Flow 2: JWT Bearer Token (for APIs/scripts)

```
Client obtains JWT from IdP (e.g., client_credentials grant)
Client → POST / -H "Authorization: Bearer eyJ..."
       → Server validates JWT signature via JWKS
       → Server resolves identity (issuer routing → group mapping)
       → Query executes with resolved role/permissions
```

### Flow 3: API Key (for service accounts)

```
Admin creates key → POST /admin/keys {"name":"svc","role":"writer"}
Service → POST / -H "Authorization: Bearer zepto_..."
       → Server validates key hash
       → Query executes with key's role
```

---

## Group-to-Role Mapping

ZeptoDB maps IdP groups to its 5-role model:

| ZeptoDB Role | Permissions | Typical IdP Group |
|-------------|-------------|-------------------|
| `admin` | Full access (DDL, queries, user management) | `ZeptoDB-Admins` |
| `writer` | Read + write (SQL + ingest) | `Trading-Desk`, `Data-Engineers` |
| `reader` | SELECT queries only | `Quant-Research`, `Analysts` |
| `analyst` | SELECT, restricted to symbol whitelist | `External-Analysts` |
| `metrics` | /metrics, /health, /stats only | `Monitoring-Service` |

When a user belongs to multiple groups, the highest-privilege role wins.

---

## Server-Side Sessions

After SSO login, ZeptoDB issues an HttpOnly session cookie instead of exposing the raw JWT to the browser.

| Setting | Default | Description |
|---------|---------|-------------|
| `session.ttl_s` | 3600 | Session lifetime (seconds) |
| `session.refresh_window_s` | 300 | Extend session if active within this window |
| `session.max_sessions` | 10000 | Max concurrent sessions |
| `session.cookie_name` | `zepto_sid` | Cookie name |
| `session.cookie_secure` | false | Set true when TLS enabled |
| `session.cookie_httponly` | true | Prevent JavaScript access |

### Token Refresh

If the IdP issued a refresh token, ZeptoDB stores it in the session. The Web UI calls `POST /auth/refresh` before the session expires to get a new access token without re-authentication.

---

## HTTP Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/auth/login` | Public | Redirect to IdP authorization endpoint |
| `GET` | `/auth/callback` | Public | OAuth2 code exchange → session → redirect |
| `POST` | `/auth/session` | Bearer | Create session from existing token |
| `POST` | `/auth/logout` | Public | Destroy session, clear cookie |
| `POST` | `/auth/refresh` | Cookie | Refresh token using stored refresh_token |
| `GET` | `/auth/me` | Cookie/Bearer | Return current identity |
| `POST` | `/admin/auth/reload` | Admin | Force JWKS key refresh |

### Example: /auth/me

```bash
# With session cookie
curl -b "zepto_sid=abc123" http://localhost:8123/auth/me

# With Bearer token
curl -H "Authorization: Bearer eyJ..." http://localhost:8123/auth/me
```

```json
{"subject": "user@example.com", "role": "writer", "source": "sso:okta-prod"}
```

---

## Multi-IdP Configuration

For environments with multiple identity providers (e.g., internal Okta + external Azure AD):

```cpp
AuthManager::Config cfg;
cfg.sso_enabled = true;

// Internal employees via Okta
IdpConfig okta;
okta.id       = "okta-internal";
okta.issuer   = "https://corp.okta.com/oauth2/default";
okta.jwks_url = "https://corp.okta.com/oauth2/default/v1/keys";
okta.group_role_map = {{"Admins", Role::ADMIN}, {"Traders", Role::WRITER}};
cfg.sso_idps.push_back(okta);

// External partners via Azure AD
IdpConfig azure;
azure.id       = "azure-partners";
azure.issuer   = "https://login.microsoftonline.com/TENANT/v2.0";
azure.jwks_url = "https://login.microsoftonline.com/TENANT/discovery/v2.0/keys";
azure.default_role = Role::ANALYST;  // restricted by default
cfg.sso_idps.push_back(azure);
```

Token routing is automatic: the JWT `iss` claim determines which IdP config is used.

---

## Vault Integration for Secrets

SSO client secrets should not be stored in plaintext config files. Use the secrets provider chain:

### HashiCorp Vault

```bash
# Store OIDC client secret in Vault
vault kv put secret/zeptodb/oidc client_secret=YOUR_SECRET

# ZeptoDB reads from Vault at startup
./zepto_http_server \
  --vault-addr https://vault.example.com \
  --vault-token $VAULT_TOKEN \
  --vault-path secret/data/zeptodb/oidc \
  --oidc-issuer https://dev-123456.okta.com/oauth2/default \
  --oidc-client-id 0oa1bcdef2ghijk3l4m5
```

### Kubernetes Secrets

```yaml
apiVersion: v1
kind: Secret
metadata:
  name: zeptodb-oidc
type: Opaque
stringData:
  client-secret: YOUR_SECRET
---
# In ZeptoDB deployment
env:
  - name: ZEPTO_OIDC_CLIENT_SECRET
    valueFrom:
      secretKeyRef:
        name: zeptodb-oidc
        key: client-secret
```

### Environment Variables

```bash
export ZEPTO_OIDC_CLIENT_SECRET=YOUR_SECRET
./zepto_http_server --oidc-issuer ... --oidc-client-id ...
```

Priority chain: Vault → K8s Secret → Environment Variable → Config file

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| "OIDC not configured" on /auth/login | `--oidc-issuer` not set or discovery failed | Check issuer URL, verify `/.well-known/openid-configuration` is accessible |
| "Token exchange failed" on callback | Client secret wrong or redirect URI mismatch | Verify client_secret and redirect_uri match IdP config exactly |
| "SSO: no matching IdP" | JWT issuer doesn't match any registered IdP | Check `iss` claim in JWT matches `IdpConfig::issuer` |
| Groups not mapped to roles | Group claim name mismatch | Check IdP's group claim name (e.g., `groups` vs `cognito:groups`) |
| Session expires too quickly | Default TTL is 1 hour | Increase `session.ttl_s` |
| JWKS refresh fails | Network/firewall blocking JWKS URL | Ensure server can reach IdP's JWKS endpoint |
| "Invalid or expired JWT" | Clock skew between server and IdP | Sync NTP, or the token is genuinely expired |

### Debug: Inspect JWT Claims

```bash
# Decode JWT payload (no verification)
echo "eyJ..." | cut -d. -f2 | base64 -d 2>/dev/null | python3 -m json.tool
```

### Debug: Force JWKS Refresh

```bash
curl -X POST http://localhost:8123/admin/auth/reload \
  -H "Authorization: Bearer $ADMIN_KEY"
```

---

## Security Considerations

- Always use HTTPS in production (`--tls-cert`, `--tls-key`)
- Set `session.cookie_secure=true` when TLS is enabled
- Store client secrets in Vault or K8s Secrets, never in config files
- Use short session TTLs (1h) with refresh tokens for long-lived sessions
- Enable audit logging to track all SSO authentication events
- Restrict `redirect_uri` to exact match (no wildcards) in IdP config
