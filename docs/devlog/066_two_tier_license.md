# Devlog 066 — Two-Tier License Consolidation

**Date:** 2026-04-15
**Category:** Security / Monetization

## Summary

Consolidated the 3-tier license system (Community/Pro/Enterprise) to 2-tier (Community/Enterprise). All former Pro features are now part of Enterprise.

## What Changed

- Removed `PRO = 1` from `Edition` enum → `{ COMMUNITY = 0, ENTERPRISE = 1 }`
- `edition_from_string("pro")` now returns `Edition::ENTERPRISE` (backward compat)
- Removed `Edition::PRO` case from `edition_to_string()`
- Updated all tests referencing Pro to use Enterprise
- Merged "Community → Pro Gate" and "Pro → Enterprise Gate" backlog sections into single "Community → Enterprise Gate"

## Rationale

- Simpler sales motion: free vs paid, no mid-tier confusion
- Matches industry standard (kdb+, ClickHouse, TimescaleDB all use 2-tier)
- Website pricing page already showed 2-tier (Community/Enterprise)

## Backward Compatibility

Old JWT license keys with `"edition": "pro"` are automatically mapped to `Edition::ENTERPRISE`. No action required from existing licensees.

## Files Changed

| File | Change |
|------|--------|
| `include/zeptodb/auth/license_validator.h` | Removed `PRO` from `Edition` enum, updated header comment |
| `src/auth/license_validator.cpp` | `"pro"` → `ENTERPRISE` mapping, removed `PRO` case |
| `tests/unit/test_license.cpp` | 6 tests updated: Pro → Enterprise |
| `docs/design/license_system.md` | Removed Pro tier row, added backward compat note |
| `docs/BACKLOG.md` | Merged gate tables into single Community → Enterprise |
| `docs/COMPLETED.md` | Updated license validator description to 2-tier |
| `docs/devlog/065_license_validator.md` | Updated Edition enum reference |
