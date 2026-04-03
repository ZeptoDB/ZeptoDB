# Devlog 042: SSO Identity Provider — Multi-IdP Support

**Date:** 2026-04-02
**Status:** ✅ Complete

---

## Summary

Added `SsoIdentityProvider` — a unified abstraction for resolving JWT tokens from
multiple OIDC identity providers (Okta, Azure AD, Google Workspace, Keycloak) into
ZeptoDB identities with group-based role mapping.

## What Changed

### New Files
- `include/zeptodb/auth/sso_identity_provider.h` — Header with `IdpConfig`, `SsoIdentity`, `SsoIdentityProvider`
- `src/auth/sso_identity_provider.cpp` — Implementation
- `docs/design/sso_identity_provider.md` — Design doc

### Modified Files
- `include/zeptodb/auth/auth_manager.h` — Added SSO config fields, `check_sso()`, `sso_provider()` accessor
- `src/auth/auth_manager.cpp` — SSO initialization + auth flow integration (SSO → JWT → API key)
- `CMakeLists.txt` — Added `sso_identity_provider.cpp` to `zepto_auth` target
- `tests/unit/test_auth.cpp` — 13 new tests

## Architecture

```
Auth Flow (updated priority):
  SSO Identity Provider (issuer-routed, multi-IdP)
    → JWT Validator (single-IdP fallback)
      → API Key Store
```

### Key Features
- **Issuer-based routing** — JWT `iss` claim matched against registered IdPs
- **Group-to-role mapping** — IdP groups (e.g. "ZeptoDB-Admins") → ZeptoDB roles
- **Identity cache** — TTL-based cache (default 300s, 10K capacity) avoids repeated validation
- **Tenant claim extraction** — Map IdP claims to ZeptoDB tenant_id
- **Graceful fallback** — Unmatched tokens fall through to existing JWT/API key auth

### Configuration Example
```cpp
AuthManager::Config cfg;
cfg.sso_enabled = true;

IdpConfig okta;
okta.id       = "okta-prod";
okta.issuer   = "https://corp.okta.com/oauth2/default";
okta.jwks_url = "https://corp.okta.com/oauth2/default/v1/keys";
okta.group_claim = "groups";
okta.group_role_map = {
    {"ZeptoDB-Admins", Role::ADMIN},
    {"Trading-Desk",   Role::WRITER},
};
cfg.sso_idps.push_back(okta);
```

## Tests

13 new tests covering:
- IdP registration/removal
- Issuer routing (unknown issuer → nullopt)
- Malformed token handling
- JSON claim extraction (string, string array, missing keys)
- Cache operations and TTL
- Multiple IdP registration
- AuthManager integration (SSO fallback to JWT)

All 169 auth tests pass (156 existing + 13 new).
