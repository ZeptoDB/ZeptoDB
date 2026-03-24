# Devlog 026: Storage Tiering + Materialized View + Multi-Tenancy

**Date:** 2026-03-24
**Phase:** Business-critical features

## Summary

Implemented 3 high sales-impact features in succession:
1. Storage Tiering — Automatic Hot→Warm→Cold→Drop policy
2. Materialized View — Incremental aggregation on ingest
3. Multi-Tenancy — Per-tenant resource quotas + table isolation

## 1. Storage Tiering

### SQL Interface
```sql
ALTER TABLE trades SET STORAGE POLICY
  HOT 1 HOURS WARM 24 HOURS COLD 30 DAYS DROP 365 DAYS
```

### Implementation
- `FlushConfig::TieringPolicy` — warm/cold/drop_after_ns thresholds
- `FlushManager::do_tiering()` — Automatic age-based tier transition in flush_loop
- `FlushManager::set_tiering_policy()` — Runtime policy change (called from SQL)
- ALTER TABLE parser extension — `SET STORAGE POLICY` syntax

### Tier Behavior
| Tier | Condition | Behavior |
|------|-----------|----------|
| HOT | age < warm | Kept in memory (μs queries) |
| WARM | age ≥ warm | Seal + LZ4 flush to SSD, memory reclaimed |
| COLD | age ≥ cold | Parquet + S3 upload, local deletion |
| DROP | age ≥ drop | Partition fully deleted |

## 2. Materialized View

### SQL Interface
```sql
CREATE MATERIALIZED VIEW ohlcv_5min AS
  SELECT symbol, xbar(timestamp, 300000000000) AS bar,
         first(price) AS open, max(price) AS high,
         min(price) AS low, last(price) AS close,
         sum(volume) AS vol
  FROM trades
  GROUP BY symbol, xbar(timestamp, 300000000000)

SELECT * FROM ohlcv_5min WHERE symbol = 1
DROP MATERIALIZED VIEW ohlcv_5min
```

### Implementation
- `MaterializedViewManager` — View registration/deletion, incremental aggregation
- `MVBucket` — Aggregation state per (group_key, time_key)
- `store_tick()` hook — Calls `on_tick()` on every tick
- Returns pre-computed results when SELECT is issued with MV name in executor
- Supported aggregations: SUM, COUNT, MIN, MAX, FIRST, LAST + xbar time bucket

### Performance Characteristics
- O(registered_views) overhead on INSERT (hash lookup + aggregation update per view)
- O(buckets) on SELECT — no raw ticks scan

## 3. Multi-Tenancy

### Implementation
- `TenantManager` — Tenant CRUD, quota enforcement, usage tracking
- `TenantConfig` — max_concurrent_queries, table_namespace, priority
- `TenantUsage` — active_queries, total_queries, rejected_queries (atomic)
- `AuthContext.tenant_id` — Tenant binding to API key/JWT
- `HttpServer.set_tenant_manager()` — Inject tenant manager into server

### Quota Enforcement Flow
```
Request → AuthManager (authentication) → Extract tenant_id
        → TenantManager.acquire_query_slot()
        → 429 Too Many Requests if quota exceeded
        → Query execution
        → TenantManager.release_query_slot()
```

## 4. Distributed DML Routing (Additional Modification)

Added DML routing to QueryCoordinator.execute_sql():
- INSERT → Route to node based on symbol
- UPDATE/DELETE → Route to corresponding node if symbol filter exists, otherwise broadcast + aggregate
- DDL → Broadcast to all nodes

## Test Results
- All existing 766/766 tests passed (including 19 distributed tests)
- Manual integration tests passed for each feature
