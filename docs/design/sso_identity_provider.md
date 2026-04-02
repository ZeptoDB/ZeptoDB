# ZeptoDB: SSO Identity Provider Integration

**Version:** 1.0
**Status:** ✅ Complete
**Related code:** `include/zeptodb/auth/sso_identity_provider.h`, `src/auth/sso_identity_provider.cpp`
**Integration guide:** [`docs/api/SSO_INTEGRATION_GUIDE.md`](../api/SSO_INTEGRATION_GUIDE.md)

---

## 1. Overview

Extends ZeptoDB's existing JWT/OIDC authentication with a unified SSO Identity Provider
abstraction. This enables:

- **Multi-IdP support** — connect multiple OIDC providers simultaneously (Okta, Azure AD, Google, Keycloak)
- **Identity mapping** — map IdP-specific claims to ZeptoDB roles/symbols/tenants
- **Group-based RBAC** — derive roles from IdP group memberships
- **Session identity cache** — avoid repeated JWKS lookups for the same subject within a TTL window

## 2. Architecture

```
JWT Bearer Token
       │
       ▼
  AuthManager::check()
       │
       ├─ SsoIdentityProvider::resolve(token)
       │       │
       │       ├─ Match issuer → IdpConfig
       │       ├─ JwtValidator::validate(token)  (existing)
       │       ├─ Apply claim mappings (group → role, attrs → symbols)
       │       └─ Return SsoIdentity
       │
       └─ Build AuthContext from SsoIdentity
```

## 3. Data Model

### IdpConfig — per-provider configuration

| Field | Type | Description |
|-------|------|-------------|
| `id` | string | Unique IdP identifier (e.g. "okta-prod") |
| `issuer` | string | Expected `iss` claim (e.g. "https://corp.okta.com/oauth2/default") |
| `jwks_url` | string | JWKS endpoint for RS256 key auto-fetch |
| `audience` | string | Expected `aud` claim |
| `group_claim` | string | JWT claim containing group list (default: "groups") |
| `group_role_map` | map<string,Role> | Group name → ZeptoDB role mapping |
| `default_role` | Role | Fallback role when no group matches (default: READER) |
| `tenant_claim` | string | JWT claim for tenant assignment (optional) |
| `symbols_claim` | string | JWT claim for symbol whitelist (default: "zepto_symbols") |

### SsoIdentity — resolved identity from IdP token

| Field | Type | Description |
|-------|------|-------------|
| `subject` | string | `sub` claim |
| `email` | string | `email` claim |
| `idp_id` | string | Which IdP resolved this identity |
| `groups` | vector<string> | Group memberships from token |
| `role` | Role | Resolved role (from group mapping or default) |
| `allowed_symbols` | vector<string> | Symbol whitelist |
| `tenant_id` | string | Tenant assignment |

## 4. Issuer Routing

When multiple IdPs are configured, the provider matches the JWT `iss` claim against
registered `IdpConfig::issuer` values. First match wins. If no issuer matches,
the token falls through to the existing JwtValidator/ApiKeyStore path.

## 5. Group-to-Role Mapping

Example configuration:

```cpp
IdpConfig okta;
okta.id       = "okta-prod";
okta.issuer   = "https://corp.okta.com/oauth2/default";
okta.jwks_url = "https://corp.okta.com/oauth2/default/v1/keys";
okta.audience = "zeptodb";
okta.group_claim = "groups";
okta.group_role_map = {
    {"ZeptoDB-Admins",  Role::ADMIN},
    {"Trading-Desk",    Role::WRITER},
    {"Quant-Research",  Role::READER},
    {"External-Analyst", Role::ANALYST},
};
okta.default_role = Role::READER;
```

Priority: first matching group in the map wins (ordered by role privilege: admin > writer > reader > analyst > metrics).

## 6. Identity Cache

Resolved identities are cached by `(idp_id, subject)` with a configurable TTL
(default: 300s). This avoids re-parsing and re-validating the same JWT on every
request within the token's active window.

Cache is bounded (default: 10,000 entries) with LRU eviction.

---

*Related: layer5_security_auth.md, devlog/042_sso_identity_provider.md*
