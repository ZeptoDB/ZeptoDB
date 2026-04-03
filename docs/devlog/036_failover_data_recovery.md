# 033 — Failover Data Recovery (Auto Re-Replication)

**Date**: 2026-03-27
**Status**: Completed
**P8-Critical**: Failover Data Recovery

---

## Background

The existing FailoverManager immediately removes a failed node from the router, but:
- There was no procedure for recovering unreplicated data
- Re-replication had to be done manually in callbacks
- If callbacks were not registered, data was silently lost

## Changes

### failover_manager.h

- Added `FailoverConfig`: `auto_re_replicate` (default true), `async_re_replicate` (default true)
- Added `re_replication_attempted`, `re_replication_succeeded` fields to `FailoverEvent`
- Embedded `PartitionMigrator migrator_` — self-contained re-replication without external dependencies
- `register_node()` — registers node RPC address with migrator
- `re_replication_count()` — queries the number of successful re-replications
- Async re-replication thread management (`async_threads_`)

### failover_manager.cpp

**trigger_failover()**:
1. Remove failed node from router/coordinator (existing)
2. Calculate re-replication targets (existing)
3. **New**: If nodes are registered with migrator, automatically execute re-replication
4. Execute callback after re-replication completes (in thread if async)
5. If nodes are not registered, skip re-replication and execute callback immediately (graceful fallback)

**run_re_replication()**:
- Calls `migrator_.migrate_symbol()` for each ReReplicationTarget
- Logs success/failure + updates statistics

### partition_migrator.h
- Added `has_node()` — checks whether a node is registered

## Failover Flow

```
Node 2 DEAD detected (HealthMonitor)
  → FailoverManager::trigger_failover(2)
    → router_.remove_node(2), coordinator_.remove_node(2)
    → re-replication targets: [{new_primary=1, new_replica=3}]
    → migrator has nodes? YES
      → async thread: migrate_symbol(1 → 3)
        → SELECT * FROM trades on node 1
        → replicate_wal() to node 3
        → callback fired with results
    → migrator has nodes? NO
      → callback fired immediately (graceful fallback)
```

## Backward Compatibility

- Existing tests: `FailoverManager(router, coordinator)` constructor compatible (FailoverConfig defaults)
- If `register_node()` is not called, behavior is identical to before (only callbacks execute)
- All 796 tests pass
