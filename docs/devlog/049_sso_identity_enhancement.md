# Devlog 044 — SSO / Identity Enhancement

Date: 2026-04-02

## What

Implemented 4 of 5 SSO/Identity backlog items (SAML excluded per scope).

## Features

### 1. OIDC Discovery (S)
- `OidcDiscovery::fetch(issuer_url)` — fetches `/.well-known/openid-configuration`
- Auto-populates `jwks_uri`, `authorization_endpoint`, `token_endpoint`, `userinfo_endpoint`
- `AuthManager` auto-registers OIDC IdP when `--oidc-issuer` is set
- Also auto-configures JWT validator + JWKS provider if not already set

### 2. Server-Side Sessions (M)
- `SessionStore` — cookie-based session management
- Configurable TTL (default 1h), sliding window refresh, max capacity
- `Set-Cookie: zepto_sid=<id>; Path=/; HttpOnly; SameSite=Lax`
- `AuthManager::check_session(cookie_header)` resolves cookie → AuthContext

### 3. Web UI SSO Login Flow (M)
- `GET /auth/login` — redirects to IdP authorization endpoint
- `GET /auth/callback` — OAuth2 code exchange → session cookie → redirect to /query
- `POST /auth/session` — create session from Bearer token (API key or JWT)
- `POST /auth/logout` — destroy session + clear cookie
- `GET /auth/me` — return current identity (cookie or Bearer)
- Web UI: "Sign in with SSO" button (no longer disabled), session-aware auth provider
- All API calls now include `credentials: "include"` for cookie transport

### 4. JWT Refresh Token (M)
- `OAuth2TokenExchange::refresh()` — exchanges refresh_token for new access_token
- `POST /auth/refresh` — server-side refresh using stored refresh_token
- Session store tracks refresh_token per session
- Web UI: `useAuth().refresh()` hook for client-side auto-renewal

## New Files

| File | Purpose |
|------|---------|
| `include/zeptodb/auth/oidc_discovery.h` | OIDC metadata fetcher |
| `src/auth/oidc_discovery.cpp` | Implementation |
| `include/zeptodb/auth/session_store.h` | Server-side session store |
| `src/auth/session_store.cpp` | Implementation |
| `include/zeptodb/auth/oauth2_token_exchange.h` | OAuth2 code/refresh exchange |
| `src/auth/oauth2_token_exchange.cpp` | Implementation |

## Modified Files

| File | Change |
|------|--------|
| `include/zeptodb/auth/auth_manager.h` | OIDC config, session store, OAuth2 imports, new accessors |
| `src/auth/auth_manager.cpp` | OIDC auto-discovery, session init, check_session() |
| `src/server/http_server.cpp` | 6 new endpoints (/auth/*), session cookie includes |
| `CMakeLists.txt` | 3 new source files in zepto_auth |
| `web/src/lib/auth.tsx` | Session-aware auth provider, loginSSO(), refresh() |
| `web/src/lib/api.ts` | credentials: "include" on all fetch calls |
| `web/src/app/login/page.tsx` | SSO button enabled |
| `web/src/components/Sidebar.tsx` | Async logout |
| `tests/unit/test_auth.cpp` | 19 new tests |
| `web/src/__tests__/auth.test.tsx` | Updated for session-aware auth |
| `web/src/__tests__/api.test.ts` | Updated for credentials: "include" |
| `web/src/__tests__/tables.test.ts` | Updated for credentials: "include" |
| `web/src/__tests__/cluster.test.ts` | Updated for credentials: "include" |

## Tests

- C++: 19 new tests (OidcDiscovery: 2, SessionStore: 10, OAuth2TokenExchange: 4, AuthManager session: 3)
- All 876 C++ tests pass
- Web: 53/54 pass (1 pre-existing theme color mismatch)

## HTTP Endpoints Added

| Method | Path | Auth | Purpose |
|--------|------|------|---------|
| GET | `/auth/login` | Public | Redirect to IdP |
| GET | `/auth/callback` | Public | OAuth2 code exchange |
| POST | `/auth/session` | Bearer | Create session from token |
| POST | `/auth/logout` | Public | Destroy session |
| POST | `/auth/refresh` | Cookie | Refresh OAuth2 token |
| GET | `/auth/me` | Cookie/Bearer | Current identity |
