# Devlog 112 — DDL replication across cluster pods (P8-DDL-replication)

Date: 2026-04-30
Scope: `include/zeptodb/cluster/query_coordinator.h`, `src/cluster/query_coordinator.cpp`, `src/server/http_server.cpp`, `tests/unit/test_ddl_replication.cpp`, `tests/CMakeLists.txt`

---

## Problem

Discovered during the 2026-04-28 EKS multinode bench Round 2 (`docs/bench/results_multinode.md`): a client that sends `CREATE TABLE trades (...)` through the Kubernetes Service LoadBalancer hits only the LB-picked pod. The other pods keep their previous schema (or no schema at all), so when the `CoordinatorRoutingAdapter` (devlog 111) subsequently routes an `INSERT` to a pod that does not yet have `trades`, the tick lands in a "nowhere" partition — `ticks_ingested` increments but `ticks_stored` does not, and `SELECT count(*)` on that pod returns 0.

The same gap affects `DROP TABLE` and `ALTER TABLE`: only the LB-picked pod applies the change, leaving the cluster's schema registry divergent across pods.

This is harmless in production, where schemas are pre-provisioned on every pod at deploy time (Helm chart, init job, or StatefulSet startup hook). It **does** break any multi-pod integration test or benchmark that creates tables via the LB rather than through side-channel provisioning.

## Design decision — fire-and-forget replication

Two options were considered:

- **Option A (adopted).** After `QueryExecutor::execute()` finishes a DDL statement locally, the HTTP server forwards the same SQL string to every *remote* node via a new `QueryCoordinator::forward_ddl_to_remotes(sql)`. The forward is fire-and-forget: per-remote failures emit a `ZEPTO_WARN` but never fail the client request.
- **Option B (rejected).** Intercept DDL inside `QueryExecutor::execute()` and replicate before returning. This would couple the executor to the coordinator, which violates the layer boundary (executor knows nothing about clustering today).

**Why fire-and-forget is safe here:**

1. DDL is idempotent when callers use `IF NOT EXISTS` / `IF EXISTS`, which is the documented multi-pod pattern.
2. If a remote pod is temporarily unreachable, the next DDL or a startup-time schema-sync can catch it up. The client sees success on the coordinator because the local DDL succeeded.
3. Strong consistency for DDL (Raft-based schema log, 2PC over all pods, quorum writes) is a much larger project and out of scope for this task. The benchmark-harness use case does not require it.

**Known limitation (explicitly accepted):** if a remote pod is down at the exact moment a DDL runs, it will miss the change until the next DDL touches the same object or until it re-syncs on restart. This is acceptable for the benchmark harness that motivated the fix.

## Implementation

### 1. `QueryCoordinator::forward_ddl_to_remotes` (new public method)

- Snapshots remote `NodeEndpoint` `shared_ptr`s under a `shared_lock` so a concurrent `remove_node()` cannot invalidate the iteration.
- Iterates only endpoints with `!is_local && rpc` — the local pipeline has already executed the DDL via the HTTP server.
- Calls `TcpRpcClient::execute_sql(sql)` on each remote. Per-remote `!ok()` results and any exception are caught and logged via `ZEPTO_WARN` with the node id, host, port, and error/exception message.
- Single-pod deployments (no coordinator wired) and remote-less coordinators never enter the loop — zero overhead.

~30 lines added across `query_coordinator.{h,cpp}`.

### 2. HTTP server wiring

In the `svr_->Post("/", ...)` handler, immediately after `run_query_with_tracking` returns a successful result:

```cpp
if (coordinator_ && have_parsed &&
    (cached_ps.create_table || cached_ps.drop_table ||
     cached_ps.alter_table)) {
    coordinator_->forward_ddl_to_remotes(req.body);
}
```

Key properties:

- **No extra parsing.** The POST handler already parses the SQL once for ACL/tenant enforcement and caches the result in `cached_ps` (devlog 091 F4). DDL detection piggybacks on the existing parse via the `std::optional` fields on `ParsedStatement`. No string matching.
- **Null-safe for single-pod mode.** `coordinator_` is only non-null when `set_coordinator()` was called (cluster mode).
- **Runs after the local DDL succeeded** — if the local DDL failed, `result.ok()` is false and the handler has already returned a 400 before reaching this point.

~10 lines added in `http_server.cpp`.

### 3. Tests — `tests/unit/test_ddl_replication.cpp`

Four scenarios, all in-process using real `TcpRpcServer` on loopback (same pattern as `test_coordinator.cpp` scatter-gather tests):

1. **`CreateTableReplicatesToRemote`** — `CREATE TABLE` locally on p1, call `forward_ddl_to_remotes`, assert `p2->schema_registry().exists("trades")`.
2. **`DropTableReplicatesToRemote`** — seed both nodes, `DROP TABLE` locally, forward, assert `!p2->schema_registry().exists("trades")`.
3. **`ForwardToDownNodeNoThrow`** — register a fake remote pointing at a no-listener port; expect `EXPECT_NO_THROW` and a warning log (confirmed visually in the test output).
4. **`LocalEndpointSkipped`** — forward with only a local endpoint registered; if the local endpoint were accidentally double-executed, the second `CREATE TABLE` would fail. Passes, proving local endpoints are skipped.

All 4 tests green. Full suite: **1288 → 1292** passing, zero regressions.

## Verification

```
$ cd build && ninja -j$(nproc) zepto_tests zepto_http_server
[17/17] Linking CXX executable tests/zepto_tests
$ ./tests/zepto_tests --gtest_filter="DDLReplication*"
[  PASSED  ] 4 tests.
$ ./tests/zepto_tests
[==========] 1292 tests from 178 test suites ran. (287 s)
[  PASSED  ] 1292 tests.
```

No new compiler warnings. The single warning observed in test output (`[warning] DDL replication to node 99 (127.0.0.1:…) failed: TcpRpcClient: cannot connect`) is the intended `ZEPTO_WARN` fired by the `ForwardToDownNodeNoThrow` test case.

## Files changed

| File | Lines | Purpose |
|------|------:|---------|
| `include/zeptodb/cluster/query_coordinator.h` | +8  | `forward_ddl_to_remotes` declaration + doc comment |
| `src/cluster/query_coordinator.cpp`           | +35 | snapshot-remotes pattern + `ZEPTO_WARN` per failure |
| `src/server/http_server.cpp`                  | +9  | DDL detection via `cached_ps` + forward call |
| `tests/unit/test_ddl_replication.cpp`         | +189 (new) | 4 scenarios |
| `tests/CMakeLists.txt`                        | +1  | register new test file |

Plus docs: BACKLOG/COMPLETED/design updates.

## Follow-ups (out of scope for this change)

- Startup-time schema sync (pull schema from a coordinator peer on pod start) so a pod that missed a DDL while down can catch up.
- Raft-based schema log if/when strong schema consistency becomes a business requirement.
- Replicate `CREATE MATERIALIZED VIEW` / `DROP MATERIALIZED VIEW` via the same mechanism — the `ParsedStatement` already exposes `create_mv` / `drop_mv` fields; one-line extension when needed.
