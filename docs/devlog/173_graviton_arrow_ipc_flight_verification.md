# 173: Graviton Arrow IPC Flight Verification

Date: 2026-06-10
Status: Complete

## Context

The cross-architecture verification run reported `Arrow IPC not compiled in`
for the standalone Graviton host unit tests, while the EKS Docker Arrow smoke
path succeeded. The mismatch came from the Graviton host reusing an existing
CMake cache with `ZEPTO_USE_FLIGHT=OFF`.

## Changes

- Updated `deploy/scripts/run_arch_comparison_fast.sh` so Stage 0 local and
  Graviton smoke builds reconfigure when `ZEPTO_USE_FLIGHT=ON` is missing from
  `CMakeCache.txt`.
- Verified a fresh Graviton `build-flight` configuration with Arrow Flight
  enabled through the bundled `pyarrow` libraries.

## Verification

- `bash -n deploy/scripts/run_arch_comparison_fast.sh`
- Graviton `build-flight`: `ZEPTO_BUILD_BENCH=ON`, `ZEPTO_USE_FLIGHT=ON`,
  `ZEPTO_USE_S3=ON`
- Graviton Arrow IPC unit:
  `./tests/zepto_tests --gtest_filter="HttpArrowIpcTest.*:HttpArrowIpcEncoder.*:HttpArrowIpcIngest.*:HttpArrowIpcStub.*"`
  passed 14/14 with no Arrow IPC build-time skip.
- Graviton full unit:
  `./tests/zepto_tests --gtest_brief=1` ran 1453 tests, passed 1452, skipped
  one live S3 test because `ZEPTO_S3_TEST_BUCKET` was unset.
- Graviton live S3:
  `ZEPTO_S3_TEST_BUCKET=abp-demo-jinmp ./tests/zepto_tests --gtest_filter="S3Sink.*"`
  passed 2/2.
- Graviton standalone HTTP Arrow smoke returned `{"inserted":3,"failed":0}`.
- Graviton standalone multi-node rebalance smoke reported
  `FINAL RESULT: 1/1 scenarios passed`.

## Follow-ups

- None.
