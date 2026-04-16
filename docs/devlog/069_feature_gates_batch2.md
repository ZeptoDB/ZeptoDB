# Devlog 069: Feature Gates Batch 2 — SSO, Audit Export, Kafka, Migration

**Date:** 2026-04-15
**Status:** Complete

## Summary

Added Enterprise license feature gates for four feature areas:

1. **SSO/OIDC authentication** — `/auth/login`, `/auth/callback`, `/auth/session`, `/auth/refresh` return 402 on Community edition. `/auth/me` and `/auth/logout` remain ungated (informational/cleanup).

2. **Audit log export** — `GET /admin/audit` returns 402 on Community. The `AuditBuffer` itself still records events internally on all editions; only the HTTP export endpoint is gated.

3. **Kafka/Pulsar connectors** — `KafkaConsumer::start()` checks `Feature::KAFKA` and returns false with a warning log on Community.

4. **kdb+/ClickHouse migration** — `ClickHouseMigrator::run()` and `HDBLoader::scan()` check `Feature::MIGRATION` and return false with a warning log on Community.

## Implementation

All gates use the same pattern established in Batch 1 (devlog 068):
- HTTP routes: inline `license().hasFeature()` check → 402 + `build_402_json()`
- Engine components: `license().hasFeature()` check → `ZEPTO_WARN()` + return false

## Files Changed

| File | Change |
|------|--------|
| `src/server/http_server.cpp` | SSO gate (4 routes) + audit export gate (1 route) |
| `src/feeds/kafka_consumer.cpp` | License check in `start()` |
| `src/migration/clickhouse_migrator.cpp` | License check in `run()` |
| `src/migration/hdb_loader.cpp` | License check in `scan()` |
| `tests/unit/test_license.cpp` | 4 new tests: `FeatureGateSSO`, `FeatureGateAuditExport`, `FeatureGateKafka`, `FeatureGateMigration` |
| `docs/BACKLOG.md` | Marked 4 items complete |

## Tests

- `FeatureGateSSO` — verifies `hasFeature(SSO)` false on 0-bitmask, true with SSO bit
- `FeatureGateAuditExport` — same pattern for `AUDIT_EXPORT`
- `FeatureGateKafka` — same pattern for `KAFKA`
- `FeatureGateMigration` — same pattern for `MIGRATION`
