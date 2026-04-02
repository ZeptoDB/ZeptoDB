# Devlog 041: Vault-backed API Key Store

**Date:** 2026-04-01
**Status:** ✅ Complete

## Summary

Added Vault-backed API Key Store — `ApiKeyStore` can now sync API key entries
to/from HashiCorp Vault KV v2 as a write-through backend.

## Design

The local file (`keys.conf`) remains the primary store. Vault acts as a sync target:

- **Write-through:** On `create_key()`, `revoke()`, `update_key()` → entry is
  written to Vault (best-effort, does not fail the operation).
- **Sync-on-load:** On startup, entries in Vault but not in the local file are
  merged into the local store. This enables multi-node key sharing.

### Vault path layout

```
{mount}/data/zeptodb/keys/{key_id}   → JSON with key entry fields
{mount}/data/zeptodb/keys/_index     → comma-separated list of all key IDs
```

## Files Changed

| File | Change |
|------|--------|
| `include/zeptodb/auth/vault_key_backend.h` | New: `VaultKeyBackend` class |
| `src/auth/vault_key_backend.cpp` | New: Vault HTTP + JSON serialization |
| `include/zeptodb/auth/api_key_store.h` | Added Vault-aware constructor, sync methods |
| `src/auth/api_key_store.cpp` | Write-through sync on create/revoke/update |
| `include/zeptodb/auth/auth_manager.h` | Added `vault_keys_enabled` + `vault_keys` config |
| `src/auth/auth_manager.cpp` | Creates `VaultKeyBackend` when configured |
| `CMakeLists.txt` | Added `vault_key_backend.cpp` to `zepto_auth` |
| `tests/unit/test_auth.cpp` | 8 new tests for `VaultKeyBackendTest` |

## Configuration

```cpp
AuthManager::Config cfg;
cfg.vault_keys_enabled = true;
cfg.vault_keys.addr    = "https://vault.internal:8200";
cfg.vault_keys.token   = getenv("VAULT_TOKEN");
cfg.vault_keys.mount   = "secret";
cfg.vault_keys.prefix  = "zeptodb/keys";
```

Or via environment variables: `VAULT_ADDR` + `VAULT_TOKEN` (auto-detected).

## Tests

8 new tests covering:
- Availability checks (no config, partial config, full config)
- Graceful degradation when Vault is unavailable
- `ApiKeyStore` with unavailable Vault backend (file-only fallback)
