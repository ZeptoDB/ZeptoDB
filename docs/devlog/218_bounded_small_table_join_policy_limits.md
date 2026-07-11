# 218: Bounded Small-Table JOIN Policy Limits

Date: 2026-07-11
Status: Complete

## Context

Experiment 011/012 validated coordinator-local materialization for small
operational/control-table hash JOINs, but the path still needed an explicit
product boundary before it could be treated as production behavior for that
scope. The open blockers were policy control, memory and latency limits,
telemetry, and rejection tests.

## Changes

- Added `QueryCoordinator::SmallTableJoinConfig` as the feature policy for the
  bounded coordinator-local JOIN path.
- Kept `BoundedBroadcast` as the default for simple declared-table hash JOINs
  over small operational/control tables, and added `Disabled` to reject matching
  candidates explicitly instead of falling through to unsafe scatter semantics.
- Replaced the fixed row cap with configurable per-side row limits and added
  estimated materialized-byte and optional latency caps.
- Extended `/stats` and Prometheus with byte-cap and latency-cap rejection
  counters, materialized-byte totals, and last-attempt byte/latency gauges.
- Hardened the distributed scheduler RPC regression to use the shared
  kernel-assigned free-port helper under parallel CTest.
- Updated API/design/governance docs to promote only the bounded small-table
  JOIN scope, while keeping arbitrary distributed JOIN planning and optimizer
  selection out of scope.

## Verification

- `git diff --check`
- `ninja -C build -j$(nproc) zepto_tests`
- `./build/tests/zepto_tests --gtest_filter='DistributedInsert.SmallTableBroadcastJoin*:DistributedInsert.OperationalTablePlacementPolicyPinsSymbollessTable:DistributedInsert.CatalogPlacementOptionAppliesAfterCoordinatorRestart' --gtest_brief=1`
- Full x86_64 CTest:
  `ninja -C build -j$(nproc) zepto_tests && cd build && ctest -j$(nproc) -E "Benchmark\\.|K8s" --output-on-failure --timeout 180`
  - 1742/1742 passed; live S3 opt-in skipped.
- Full aarch64 Graviton stage:
  `./tools/run-full-matrix.sh --stages=8 --force-resync`
  - 1742/1742 passed; live S3 opt-in skipped.

## Follow-ups

- Cluster-mode full-data window materialization policy limits are now covered
  by devlog 219.
- Add a future optimizer/cost rule before claiming larger arbitrary cross-node
  JOIN planning.
