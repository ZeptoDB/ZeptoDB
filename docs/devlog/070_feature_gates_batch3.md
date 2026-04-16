# Devlog 070: Feature Gates Batch 3

**Date:** 2026-04-15
**Scope:** P2.5 Monetization — Enterprise feature gating for cluster, rebalancing, rolling upgrade, per-tenant rate limiting, and advanced RBAC

---

## Summary

Batch 3 adds license gates to 5 feature areas, completing the Community → Enterprise gate table for cluster and RBAC features. All gates follow the established pattern from Batch 2 (devlog 069): `require_feature` lambda for HTTP endpoints, `license().hasFeature()` for library code.

## Changes

### 1. Multi-node Cluster (Feature::CLUSTER)

- **`POST /admin/nodes`** — gated after `require_admin`
- **`DELETE /admin/nodes/:id`** — gated after `require_admin`
- **`ClusterNode::join_cluster()`** — throws `std::runtime_error` if CLUSTER not licensed
- Added `#include "zeptodb/auth/license_validator.h"` to `cluster_node.h`

### 2. Live Rebalancing (Feature::CLUSTER)

All 6 rebalance endpoints gated with CLUSTER (rebalancing is a cluster feature):
- `POST /admin/rebalance/start`
- `POST /admin/rebalance/pause`
- `POST /admin/rebalance/resume`
- `POST /admin/rebalance/cancel`
- `GET /admin/rebalance/status`
- `GET /admin/rebalance/history`

### 3. Rolling Upgrade (Feature::ROLLING_UPGRADE)

- **`POST /admin/upgrade/start`** — new placeholder endpoint, gated with ROLLING_UPGRADE
- Returns 501 (not yet implemented) when licensed

### 4. Per-tenant Rate Limiting (Feature::ADVANCED_RBAC)

Tenant admin endpoints gated (per-tenant isolation is an advanced RBAC feature):
- `GET /admin/tenants`
- `POST /admin/tenants`
- `DELETE /admin/tenants/:id`

Basic rate limiting (per-IP, per-identity) remains available on Community.

### 5. Advanced RBAC (Feature::ADVANCED_RBAC)

Basic RBAC (admin/writer/reader/metrics roles) works on all editions. Per-tenant isolation and the ANALYST role with symbol whitelisting are gated via the tenant endpoints above.

## Tests Added

3 new tests in `tests/unit/test_license.cpp`:
- `FeatureGateCluster` — 0-bitmask → false; CLUSTER bit → true
- `FeatureGateRollingUpgrade` — 0-bitmask → false; ROLLING_UPGRADE bit → true
- `FeatureGateAdvancedRBAC` — 0-bitmask → false; ADVANCED_RBAC bit → true

All 32 license tests pass.

## Files Modified

| File | Change |
|------|--------|
| `src/server/http_server.cpp` | Gate 11 endpoints + add 1 new endpoint |
| `include/zeptodb/cluster/cluster_node.h` | Gate `join_cluster()` + add include |
| `tests/unit/test_license.cpp` | 3 new tests |
| `docs/BACKLOG.md` | Mark 5 items complete |
| `docs/devlog/070_feature_gates_batch3.md` | This file |
