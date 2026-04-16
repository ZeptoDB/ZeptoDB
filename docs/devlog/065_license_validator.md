# Devlog 065 — License Validator

**Date:** 2026-04-15
**Category:** Security / Monetization (P2.5)

## Summary

Implemented edition-based license validation using RS256-signed JWT keys.

## What was done

- **`include/zeptodb/auth/license_validator.h`** — `Edition` enum (Community/Enterprise), `Feature` bitmask (8 gated features), `LicenseClaims` struct, `LicenseValidator` class, `license()` global singleton
- **`src/auth/license_validator.cpp`** — JWT decode + RS256 verification (reuses `JwtValidator` infrastructure), env/file/direct key loading, expiry with 30-day grace period, 7-day warning log
- **`docs/design/license_system.md`** — Design doc covering tiers, JWT claims, loading priority, expiry policy, HTTP 402 gating

## Design Decisions

- **Reuse JwtValidator** — RS256 verification, base64url decode, and JSON helpers are all reused from the existing `jwt_validator.h`. No new crypto code.
- **Silent Community default** — No key = Community edition with no log noise. Invalid key logs a single warning.
- **Grace period** — 30-day default post-expiry grace keeps production running while license renewal is in progress. After grace: automatic downgrade to Community.
- **Offline validation** — No phone-home. Public key embedded at compile time.

## Key Loading Priority

1. `ZEPTODB_LICENSE_KEY` environment variable
2. `/etc/zeptodb/license.key` file
3. Direct string via API

## Files Changed

| File | Change |
|------|--------|
| `include/zeptodb/auth/license_validator.h` | New — header |
| `src/auth/license_validator.cpp` | New — implementation |
| `docs/design/license_system.md` | New — design doc |
| `CMakeLists.txt` | Added `license_validator.cpp` to `zepto_auth` |

> Updated 2026-04-15: Edition references updated to reflect 2-tier consolidation (see devlog 066).