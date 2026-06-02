# 150: EKS Rebalance Bench Hardening

Date: 2026-06-02
Status: Complete

## Context

The Arrow-enabled EKS pass exposed two remaining benchmark harness issues:
`bench_rebalance` could wait for the full rebalance timeout after the admin
trigger had already failed, and the fast cross-architecture script could mark a
successful run as failed while Karpenter nodes were still draining. The EKS
bench pods also needed the Enterprise bench license injected so the admin
rebalance endpoints could pass license gating.

## Changes

- `tests/bench/bench_rebalance.cpp`
  - Added the `smoke` scenario for baseline ingest/query validation without a
    live rebalance trigger.
  - Added `--rebalance-timeout-sec` and propagated trigger failures
    immediately instead of waiting for the full timeout.
  - Preserved HTTP status/body for failed `/admin/rebalance/start` calls so
    license or topology failures are visible in logs.
- `tools/zepto_http_server.cpp`
  - Loads `ZEPTODB_LICENSE_KEY` / license-file state at startup through the
    shared license validator.
- `deploy/scripts/run_arch_comparison_fast.sh`
  - Injects `keys/bench.license` into each EKS namespace as
    `ZEPTODB_LICENSE_KEY`.
  - Adds `--rebalance-timeout` and `--bench-timeout`.
  - Uses the `smoke` scenario for local/remote harness sanity checks.
  - Generates accurate smoke-only summaries for both x86_64 and arm64 and no
    longer appends teardown output into `summary.md` on missing `basic`
    scenario metrics.
  - Deletes bench NodeClaims during teardown and waits for bench nodes to reach
    zero before returning success.
- `deploy/docker/Dockerfile.bench` and
  `deploy/docker/Dockerfile.bench.arm64`
  - Fixed the CMake test-disable flag to `-DZEPTO_BUILD_TESTS=OFF`.

## Verification

- Local:
  - `cmake --build build --target bench_rebalance zepto_http_server zepto_data_node zepto_tests -j$(nproc)`
  - `./build/tests/zepto_tests --gtest_filter='*Rebalance*:*License*:*HttpArrow*:*ArrowIpc*'`
    passed 100/100 tests.
  - Local `bench_rebalance --scenario smoke` passed against a loopback
    three-node setup.
  - Local no-license rebalance trigger now fails quickly with
    `status=402 ... enterprise_required` instead of hanging.
- EKS images:
  - `bench-x86` pushed with digest
    `sha256:13c16ef65701eca31acc42cf89634691579cd642ae53580ad84a3f0d1481596c`.
  - `bench-arm64` pushed with digest
    `sha256:46bb01ad138ea97423616d57ea02377a263c00f50eedc8da92d255b9926eb51e`.
- EKS cross-architecture smoke:
  - `./deploy/scripts/run_arch_comparison_fast.sh --skip-local --skip-build --arrow-smoke --scenario smoke --baseline 3 --ticks 1000 --symbols 10 --rebalance-timeout 20 --bench-timeout 180`
  - Stage 4 SQL smoke passed on x86_64 and arm64.
  - Stage 4 Arrow smoke passed on x86_64 and arm64.
  - Stage 5 `bench_rebalance --scenario smoke` passed on x86_64 and arm64.
  - Stage 6 summary reported `smoke | PASS | PASS`.
  - Stage 7 teardown exited 0; bench NodePool CPU/status CPU were confirmed at
    `0`, and bench nodes were confirmed at `0`.

## Follow-ups

- Full `add_remove_cycle` / live rebalance impact benchmarking still needs an
  EKS topology or admin path that can actually add/register a new node, or a
  scenario rewrite that benchmarks moves over existing registered nodes. The
  current harness now reports unsupported topology failures quickly instead of
  hanging.
