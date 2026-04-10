# Devlog 054: Live Rebalancing Test Plan

Date: 2026-04-08

## Status

Planning — test scenarios defined, T1/T2 implemented.

## Background

ZeptoDB has partition migration infrastructure:
- `PartitionRouter`: consistent hash ring with `begin_migration()` / `end_migration()` dual-write flags
- `PartitionMigrator`: state machine (PENDING → DUAL_WRITE → COPYING → COMMITTED/FAILED), checkpoint, retry
- `MigrationCheckpoint`: JSON persistence for crash recovery

Existing tests cover static migration (no concurrent ingestion). Live rebalancing — migrating partitions while data is actively being ingested — has no E2E test coverage.

## Critical Gap Found

`PartitionRouter::migration_target()` is defined but **never called in the ingestion path**. `ClusterNode::ingest_tick()` calls `route()` which only checks the hash ring and pin table. During migration, ticks for a migrating symbol should be dual-written to both source and destination, but this is not wired in.

This means the dual-write mechanism is currently **dead code** in production ingestion flow.

## Test Scenarios

### Tier 1: Data Correctness (must-have)

| # | Scenario | Verifies |
|---|----------|----------|
| T1 | Ingestion during single symbol migration | No data gap or duplication after migration completes |
| T2 | Dual-write verification during migration window | Ticks arriving between `begin_migration()` and `end_migration()` exist on both nodes |

### Tier 2: Failure Resilience

| # | Scenario | Verifies |
|---|----------|----------|
| T3 | Source node crash during COPYING state | Rollback cleans partial data on dest, checkpoint allows resume |
| T4 | Dest node crash during COPYING state | Rollback attempt fails gracefully, retry on new dest |
| T5 | Checkpoint resume after process restart | JSON load → resume_plan() completes remaining moves |

### Tier 3: Concurrency & Performance

| # | Scenario | Verifies |
|---|----------|----------|
| T6 | Multi-symbol concurrent migration during ingestion | No cross-symbol interference, all data correct |
| T7 | Query during migration returns consistent results | SELECT on migrating symbol returns complete data |
| T8 | High-throughput ingestion + migration latency | Dual-write overhead < 10% latency increase |
| T9 | Scale-out: plan_add() + execute_plan() during ingestion | Full flow from new node join to traffic serving |

## Implementation

- T1, T2: `tests/unit/test_coordinator.cpp` — implemented in this devlog
- T3–T9: future work

## Files Changed

- `tests/unit/test_coordinator.cpp` — T1, T2 test cases
- `docs/devlog/054_live_rebalancing_test_plan.md` — this file
