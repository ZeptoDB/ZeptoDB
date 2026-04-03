# Devlog 035: API Key Granular Control

**Date:** 2026-03-27

## Summary

Enhanced API key management with fine-grained control: symbol/table ACL at creation time,
tenant binding, key expiry, and in-place key editing via PATCH endpoint.

## Changes

### Backend (C++)

- `ApiKeyEntry`: added `expires_at_ns` field and `is_expired()` helper
- `ApiKeyStore::create_key()`: extended with `tenant_id` and `expires_at_ns` parameters
- `ApiKeyStore::update_key()`: new method to modify symbols, tables, enabled, tenant_id, expires_at_ns
- `ApiKeyStore::validate()`: now rejects expired keys
- File format: extended with `tenant_id` (field 8) and `expires_at_ns` (field 9), backward-compatible
- `AuthManager`: pass-through for new `create_api_key` params and new `update_api_key` method
- `POST /admin/keys`: parses `symbols`, `tables`, `tenant_id`, `expires_at_ns` from request body
- `GET /admin/keys`: response now includes `last_used_ns`, `expires_at_ns`, `tenant_id`, `allowed_symbols`
- `PATCH /admin/keys/:id`: new endpoint for updating key fields in-place

### Frontend (Web UI)

- `api.ts`: `createKey()` extended with symbols/tables/tenantId/expiresAtNs; new `updateKey()` function
- Admin page API Keys tab: table now shows Scope, Tenant, Expires, Last Used columns
- Create Key dialog: added fields for Allowed Symbols, Allowed Tables, Tenant ID, Expiry (days)
- Edit Key dialog: modify symbols, tables, tenant, expiry, enabled/disabled toggle

### Tests

- `ExpiryBlocksValidation`: expired key rejected
- `NonExpiredKeyValidates`: future-expiry key accepted, `expires_at_ns` preserved
- `TenantIdPreserved`: tenant_id round-trips through create/validate
- `UpdateKeyFields`: symbols and tenant updated via `update_key`
- `UpdateKeyDisable`: key disabled via `update_key`
- `TenantAndExpiryPersistence`: all new fields survive file persistence

## Files Modified

- `include/zeptodb/auth/api_key_store.h`
- `include/zeptodb/auth/auth_manager.h`
- `src/auth/api_key_store.cpp`
- `src/auth/auth_manager.cpp`
- `src/server/http_server.cpp`
- `web/src/lib/api.ts`
- `web/src/app/admin/page.tsx`
- `tests/unit/test_auth.cpp`
- `docs/api/HTTP_REFERENCE.md`
