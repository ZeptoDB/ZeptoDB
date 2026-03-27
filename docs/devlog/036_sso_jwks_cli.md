# Devlog 036: SSO/JWT CLI Flags, JWKS Auto-Fetch, Runtime Reload

**Date:** 2026-03-27

## Summary

Removed all three SSO limitations:
1. CLI flags to enable JWT/SSO without code changes
2. JWKS auto-fetch with background key rotation
3. Runtime JWKS reload endpoint

## Changes

### 1. CLI Flags (`tools/zepto_http_server.cpp`)

New flags:
- `--jwt-issuer <url>` — expected issuer (iss claim)
- `--jwt-audience <aud>` — expected audience (aud claim)
- `--jwt-secret <secret>` — HS256 shared secret
- `--jwt-public-key <path>` — RS256 PEM public key file
- `--jwks-url <url>` — JWKS endpoint for auto-fetch

Validation: HS256 and RS256 cannot be combined. Any JWT flag auto-enables `jwt_enabled`.

### 2. JWKS Provider (`include/zeptodb/auth/jwks_provider.h`, `src/auth/jwks_provider.cpp`)

New class `JwksProvider`:
- HTTP(S) fetch from JWKS endpoint (uses httplib Client/SSLClient)
- Parses `{"keys":[...]}` JSON, filters `kty=RSA, use=sig`
- JWK→PEM conversion: base64url decode n/e → `OSSL_PARAM_BLD` → `EVP_PKEY_fromdata` → PEM
- `kid`-based key map for rotation support
- Background refresh thread (default 3600s interval)
- `refresh()` for synchronous force-fetch

Integration with `JwtValidator`:
- New `KeyResolver` callback: `std::function<std::string(const std::string& kid)>`
- RS256 verification falls back to key resolver when no static PEM is configured
- JWT header `kid` field extracted and passed to resolver

### 3. Runtime Reload (`POST /admin/auth/reload`)

- Admin-only endpoint
- Calls `AuthManager::refresh_jwks()` → `JwksProvider::refresh()`
- Returns `{"refreshed":true,"keys_loaded":N}` on success
- Returns 502 if no JWKS URL configured or fetch fails

### Files Modified

- `include/zeptodb/auth/jwks_provider.h` (new)
- `src/auth/jwks_provider.cpp` (new)
- `include/zeptodb/auth/jwt_validator.h` — KeyResolver callback
- `src/auth/jwt_validator.cpp` — key resolver fallback in RS256 path
- `include/zeptodb/auth/auth_manager.h` — jwks_url config, JwksProvider member, refresh_jwks()
- `src/auth/auth_manager.cpp` — JWKS initialization, key resolver wiring
- `src/server/http_server.cpp` — POST /admin/auth/reload endpoint
- `tools/zepto_http_server.cpp` — CLI flags, JWT config wiring
- `CMakeLists.txt` — jwks_provider.cpp added to zepto_auth
- `tests/unit/test_auth.cpp` — 3 new tests

### Tests (3 new, 819 total passing)

- `JwkToPemProducesValidKey` — round-trip: generate RSA → extract n,e → JWK→PEM → verify signature
- `KeyResolverIntegration` — JwtValidator uses resolver when no static PEM
- `KeyResolverEmptyPemFails` — resolver returning empty string rejects token
