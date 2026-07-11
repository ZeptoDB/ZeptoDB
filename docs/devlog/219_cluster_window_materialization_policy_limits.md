# 219: Cluster Window Materialization Policy Limits

Date: 2026-07-11
Status: Complete

## Context

Experiment 011/012 validated coordinator-local full-data materialization for
the Action-Outcome replay shape, including `ROW_NUMBER` and `LAG`. The path
still needed production guardrails before it could be promoted for the bounded
operational/control-table scope: explicit policy control, row and memory
limits, latency handling, telemetry, and fail-closed semantics for large
tables.

## Changes

- Added `QueryCoordinator::WindowMaterializationConfig` as the feature policy
  for distributed queries that require all base rows on the coordinator.
- Kept `BoundedCoordinatorLocal` as the default for declared
  operational/control-table window and full-data queries, and added `Disabled`
  to reject matching candidates explicitly.
- Added configurable row, estimated materialized-byte, and optional latency
  caps. Cap overages return clear SQL errors instead of falling back to partial
  scatter semantics.
- Fixed cluster window detection to check both `window_spec` and
  `window_func`, so `ROW_NUMBER`, `LAG`, and related functions reliably enter
  the bounded full-data path.
- Extended `/stats` and Prometheus with candidate, accepted, row-cap,
  byte-cap, latency-cap, error, materialized row/byte, last row, last byte, and
  last latency telemetry.

## Verification

- `git diff --check`
- `ninja -C build -j$(nproc) zepto_tests`
- `./build/tests/zepto_tests --gtest_filter='DistributedInsert.ClusterWindowMaterializesGenericTableValues:DistributedInsert.WindowMaterialization*' --gtest_brief=1`
- Full x86_64 CTest:
  `ninja -C build -j$(nproc) zepto_tests && cd build && ctest -j$(nproc) -E "Benchmark\\.|K8s" --output-on-failure --timeout 180`
  - 1742/1742 passed; live S3 opt-in skipped.
- Full aarch64 Graviton stage:
  `./tools/run-full-matrix.sh --stages=8 --force-resync`
  - 1742/1742 passed; live S3 opt-in skipped.

## Follow-ups

- Add a future optimizer/cost rule before claiming broader arbitrary MPP
  window planning.
- Verify GitHub Actions after push before merging this promotion to `main`.
