# Devlog 068 — Edition Foundation (P2.5 Batch 1)

**Date:** 2026-04-15
**Status:** Complete
**Design doc:** `docs/design/license_system.md`

## Summary

Implemented 5 foundation items for the ZeptoDB edition/license system: startup banner, trial key support, HTTP 402 standard, public license endpoint, and admin license management endpoints.

## Changes

### 1. Startup Banner (`startupBanner()`)

- Added `LicenseValidator::startupBanner()` returning multi-line banner
- Community: shows edition + upgrade URL hint
- Enterprise: shows company, node count, expiry (no upgrade hint)
- Files: `license_validator.h`, `license_validator.cpp`

### 2. Trial Key Support

- `generate_trial_key(company)` — creates unsigned JWT (`alg:none`) with `trial:true`, 30-day expiry, all features, single-node
- `isTrial()` — checks if current license is a trial
- `decode_and_verify()` — accepts unsigned JWTs when `alg=none` AND `trial=true`; still enforces expiry
- `statusLine()` — appends "Trial" suffix for trial licenses
- Added `trial_` private member to `LicenseValidator`
- Files: `license_validator.h`, `license_validator.cpp`

### 3. HTTP 402 Response Standard

- `build_402_json(feature_name)` — file-local helper in `http_server.cpp`
- `require_feature` lambda in `setup_admin_routes()` for feature gating
- Returns `{"error":"enterprise_required","message":"...","upgrade_url":"https://zeptodb.com/pricing"}`

### 4. GET /api/license (Public)

- Returns edition, features array, max_nodes, trial, expired status
- Community includes `upgrade_url`; Enterprise includes `company` and `expires`
- No authentication required

### 5. Admin License Endpoints

- `GET /admin/license` — full license details (+ company, tenant_id, grace_days, issued_at)
- `POST /admin/license` — upload and load a license key JWT
- `POST /admin/license/trial` — generate and load a 30-day trial key

## Tests Added

- `StartupBannerCommunity` — banner contains "Community" and upgrade URL
- `StartupBannerEnterprise` — banner contains "Enterprise", no upgrade hint
- `TrialKeyGeneration` — generate → load → verify edition, trial flag, max_nodes
- `TrialKeyExpiry` — expired trial beyond grace → load fails
- `StatusLineTrialSuffix` — trial key → statusLine contains "Trial"

## Files Modified

| File | Change |
|------|--------|
| `include/zeptodb/auth/license_validator.h` | Added `startupBanner()`, `generate_trial_key()`, `isTrial()`, `trial_` |
| `src/auth/license_validator.cpp` | Implemented banner, trial key gen, trial detection in load/verify |
| `src/server/http_server.cpp` | Added license include, 402 helper, `/api/license`, admin license endpoints |
| `tests/unit/test_license.cpp` | 5 new tests |
| `docs/design/license_system.md` | Added Trial Keys section |
| `docs/api/HTTP_REFERENCE.md` | Added 4 license endpoint docs |
| `docs/devlog/068_edition_foundation.md` | This file |
