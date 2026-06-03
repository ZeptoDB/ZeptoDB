# 158: EKS Full Rebalance Integrity Repair

Date: 2026-06-03
Status: Complete

## Context

The full cross-architecture EKS rebalance scenario failed in the `basic`
integrity phase: only a subset of the 50 symbols were visible after routed
HTTP INSERT traffic. Live pod inspection showed that symbols owned by the
front-door node were stored correctly, while forwarded single-tick RPC writes
to remote owners returned success without creating owner-side partitions.

The same investigation also exposed a harness bug: a `kubectl exec` failure
could leave a result file without a scenario PASS while Stage 5 still advanced
to comparison.

## Changes

- `tools/zepto_http_server.cpp`
  - Peer single-tick `TICK_INGEST` RPC now marks the table as containing data
    and calls `drain_sync()` before returning success, matching local HTTP
    INSERT visibility semantics.
- `src/server/http_server.cpp` and `src/cluster/query_coordinator.cpp`
  - Cluster-mode HTTP `SELECT` now routes through `QueryCoordinator`, and the
    coordinator resolves table-aware routing from stable table ids.
- `include/zeptodb/storage/schema_registry.h`
  - Table ids are stable and name-derived across pods, with collision probing
    and lookup by id.
- `deploy/scripts/run_arch_comparison_fast.sh`
  - Benchmark result files must contain an explicit scenario PASS.
  - Stage 5 waits for the x86_64 and arm64 benchmark jobs by PID.
  - Summary comparison fails if any scenario is `FAIL` or unknown.
- Tests
  - Added a single-tick RPC visibility regression test.
  - Expanded distributed insert, HTTP cluster, and table-scoped partitioning
    coverage around stable table ids and coordinator-routed SELECT.

## Verification

- Local build:
  - `cmake --build build --target zepto_http_server zepto_tests -j$(nproc)`
- Local focused tests:
  - `./build/tests/zepto_tests --gtest_filter='HttpCluster.SingleTickRpcDrainsBeforeSuccess:HttpCluster.ClusterMode_ReturnsClusterAndMultipleNodes:DistributedInsert.RoutesToRemoteOwner:CoordinatorRoutingAdapter.*:DDLReplication.*:RebalanceTest.PartialMovePreservesTradesTableId:RebalanceTest.PartialMoveDataIntegrity:TableScopedPartitioning.*:SqlExecutorTest.CreateTable_StableTableIdAcrossRegistries'`
  - Result: 28/28 PASS.
- EKS full cross-arch run:
  - `./deploy/scripts/run_arch_comparison_fast.sh --skip-local --skip-build --arrow-smoke`
  - Result directory: `/tmp/arch_fast_20260603_175848`
  - Stage 4 SQL smoke and Arrow smoke passed on x86_64 and arm64.
  - Stage 5/6 scenarios all passed on x86_64 and arm64:
    `basic`, `add_remove_cycle`, `pause_resume`, `heavy_query`,
    `back_to_back`, and `status_polling`.
  - Integrity checks reported `Symbols verified: 50/50` on both
    architectures for scenarios with integrity phases.
  - Teardown reset both bench NodePools to CPU limit `0`; no bench-labeled
    nodes or ZeptoDB pods remained.
- Hygiene:
  - `bash -n deploy/scripts/run_arch_comparison_fast.sh`
  - `git diff --check`

## Follow-ups

- The HTTP rebalance benchmark remains latency-bound because it sends
  per-row HTTP INSERTs through the service and often pays an RPC hop for
  non-local owners. The existing "Bench: symbol-aware / batched HTTP client"
  backlog item remains open for throughput-quality measurements.
