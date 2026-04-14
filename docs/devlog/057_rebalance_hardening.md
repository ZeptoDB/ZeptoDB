# Devlog 057: Live Rebalancing Hardening

Date: 2026-04-12

## Summary

Three safety fixes for the live rebalancing feature (devlog 055):

1. **`peer_rpc_clients_` thread safety** — data race in `ClusterNode::remote_ingest()`
2. **Move timeout enforcement** — per-move timeout in `PartitionMigrator`
3. **Query routing safety** — read-from-both grace period after migration

## Fix 1: peer_rpc_clients_ Thread Safety

**Problem:** `peer_rpc_clients_` (unordered_map) was accessed without synchronization in `remote_ingest()`. With dual-write doubling call frequency, concurrent threads could race on map reads/writes.

**Solution:** Added `std::shared_mutex peer_rpc_mutex_`:
- `shared_lock` for reads in `remote_ingest()` hot path
- `unique_lock` for writes in `join_cluster()`, `leave_cluster()`, and lazy client creation
- Race-safe lazy creation: if two threads try to create the same client, the second uses the first's result

## Fix 2: Move Timeout Enforcement

**Problem:** `PartitionMigrator::execute_move()` called `migrate_symbol()` synchronously with no timeout. If the source node was slow or unreachable, the worker thread would block indefinitely.

**Solution:**
- Added `move_timeout_sec` to `RebalanceConfig` (default 300s, 0 = disabled)
- `PartitionMigrator::execute_move()` wraps `migrate_symbol()` in `std::async` + `wait_for`
- On timeout: move marked FAILED, dual-write ended, descriptive error logged
- `RebalanceManager::start_plan()` wires config timeout into migrator

## Fix 3: Query Routing Safety

**Problem:** After `end_migration()`, queries route to the new owner which may not have all historical data replicated yet. Brief window of incomplete query results.

**Solution:** Added `recently_migrated_` map to `PartitionRouter`:
- `end_migration()` records `{from, to}` + timestamp in `recently_migrated_`
- `recently_migrated(symbol)` returns the pair during grace period (default 30s)
- Auto-expires after grace period (lazy cleanup on next query)
- `set_migration_grace_period()` allows configuration
- Query layer can call `recently_migrated()` to read from both old and new owner

## Tests Added (8)

| Test | Fix | What it verifies |
|------|-----|-----------------|
| `PeerRpcConcurrentAccess` | #1 | 4 threads concurrent `ingest_tick()` — no crash |
| `MoveTimeoutEnforced` | #2 | 5s timeout, local moves succeed within timeout |
| `MoveTimeoutZeroDisablesTimeout` | #2 | `set_move_timeout(0)` disables timeout |
| `RecentlyMigratedGracePeriod` | #3 | After `end_migration()`, `recently_migrated()` returns value |
| `RecentlyMigratedExpires` | #3 | After 1s grace period, entry expires |
| `RecentlyMigratedNoMigration` | #3 | No migration → both return nullopt |
| `RecentlyMigratedMultipleSymbols` | #3 | Multiple symbols tracked independently |
| `EndMigrationWithoutBeginIsNoOp` | #3 | `end_migration()` on non-migrating symbol is safe |

## Files Changed

| File | Change |
|------|--------|
| `include/zeptodb/cluster/cluster_node.h` | `peer_rpc_mutex_` + lock guards in `remote_ingest()`/`join_cluster()`/`leave_cluster()` |
| `include/zeptodb/cluster/partition_migrator.h` | `move_timeout_sec_` member, `set_move_timeout()`, `<future>` include |
| `include/zeptodb/cluster/partition_router.h` | `recently_migrated_` map, `recently_migrated()`, `set_migration_grace_period()`, `<chrono>` include |
| `include/zeptodb/cluster/rebalance_manager.h` | `move_timeout_sec` in `RebalanceConfig` |
| `src/cluster/partition_migrator.cpp` | `std::async` + `wait_for` timeout in `execute_move()` |
| `src/cluster/rebalance_manager.cpp` | Wire `move_timeout_sec` into migrator in `start_plan()` |
| `tests/unit/test_rebalance.cpp` | 8 new tests |
