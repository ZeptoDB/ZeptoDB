# 149: Arrow-Enabled EKS Bench Images

Date: 2026-06-01
Status: Complete

## Context

`POST /insert/arrow` had local C++ coverage, but the EKS bench images were
compiled with `-DZEPTO_USE_FLIGHT=OFF`. That meant cross-architecture EKS could
only verify the graceful `406` path, not successful Arrow IPC ingest on x86_64
and arm64.

## Changes

- `CMakeLists.txt` now detects Arrow Flight independently of `ZEPTO_USE_S3`.
  S3-disabled builds can still enable `ZEPTO_USE_FLIGHT=ON`.
- The Arrow Flight detection path now also accepts `arrow-flight` via
  pkg-config when a CMake `ArrowFlight` config is not available.
- `deploy/docker/Dockerfile.bench` and
  `deploy/docker/Dockerfile.bench.arm64` now install Apache Arrow APT packages
  in both builder and runtime stages and build with `-DZEPTO_USE_FLIGHT=ON`.
- `deploy/scripts/run_arch_comparison_fast.sh` gained:
  - `--arrow-smoke` to generate a small Arrow IPC RecordBatchStream payload,
    copy it into each load generator pod, and POST it to `/insert/arrow`.
  - `--skip-benchmark` to stop after remote smoke so endpoint verification can
    run independently of Stage 5 benchmark scenarios.
  - A replica-safe SQL smoke check that requires a positive count instead of
    assuming all inserts land on the same service-backed pod.

## Verification

- Local CMake configure:
  - `ZEPTO_USE_S3=OFF`, `ZEPTO_USE_FLIGHT=OFF` -> `Arrow Flight: OFF`
  - `ZEPTO_USE_S3=OFF`, `ZEPTO_USE_FLIGHT=ON` -> `Arrow Flight: ON`
- EKS image build/push:
  - `bench-x86` pushed to ECR with Arrow Flight enabled.
  - `bench-arm64` pushed to ECR with Arrow Flight enabled.
- EKS smoke:
  - `./deploy/scripts/run_arch_comparison_fast.sh --skip-local --skip-build --arrow-smoke --skip-benchmark`
  - Stage 3 Helm deploy reached OK for x86_64 and arm64.
  - Stage 4 SQL smoke reached OK after transient kubelet TLS retries.
  - Stage 4 Arrow smoke reached OK on both x86_64 and arm64 with
    `/insert/arrow` returning `{"inserted":3,...}`.
- The bench cluster was slept after verification; x86_64 and arm64 NodePool
  CPU limits were confirmed at `0`, namespaces were removed, and bench nodes
  were confirmed at `0`.

## Follow-ups

- Resolved in devlog 150: `bench_rebalance` now fails fast on trigger errors,
  and the fast EKS harness passes Stage 5 smoke on both architectures. Full
  live add/remove impact numbers still require a scale-out topology path.
