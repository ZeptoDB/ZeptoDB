# 140: EKS Cross-Arch Verification Hardening

Date: 2026-05-29
Status: Complete

## Context

The EKS full-matrix path exposed several infrastructure and test-harness issues
while validating amd64 and arm64 Kubernetes compatibility, HA, performance, and
native engine benchmarks.

## Changes

- `tests/k8s/run_eks_bench.sh`
  - Provisions both amd64 and arm64 trigger deployments before waiting for
    three or more ready nodes per architecture.
  - Captures amd64 and arm64 result files even when a child test chain fails,
    so cleanup and diagnostics still run deterministically.
- `tests/k8s/test_k8s_compat.py`
  - Accepts either hard or soft pod anti-affinity for hostname spreading.
    The chart default is hard anti-affinity.
- `tests/k8s/test_k8s_ha_perf.py`
  - Waits for service endpoints to catch up after pod readiness during node
    drain recovery.
- `tests/bench/run_arch_bench.sh`
  - Accepts both bare host and `user@host` forms for `GRAVITON_HOST`.
  - Builds benchmark binaries when `--skip-build` is requested but local or
    remote binaries are missing.
  - Makes remote build failures visible instead of hiding them behind `tail`.
  - Stores filtered benchmark summary logs to avoid filling `/tmp`.
- `include/zeptodb/storage/s3_sink.h`
  - Gates AWS SDK code on the CMake-provided `ZEPTO_S3_ENABLED` definition, not
    `__has_include` alone, so `ZEPTO_USE_S3=OFF` builds do not link AWS symbols.

## Verification

- `bash -n tests/k8s/run_eks_bench.sh`
- `bash -n tests/bench/run_arch_bench.sh`
- `python3.13 -m py_compile tests/k8s/test_k8s_compat.py tests/k8s/test_k8s_ha_perf.py`
- `helm template test deploy/helm/zeptodb -f tests/k8s/test-values.yaml`
  confirmed hard pod anti-affinity is rendered.
- `cmake --build build --target bench_hdb -j2`
- `./tools/run-full-matrix.sh --stages=7 --keep-going`
  - Log directory: `/tmp/zepto_full_matrix_20260529_230032`
  - K8s result: amd64 compat 27/27, amd64 HA/perf 11/11, arm64 compat 27/27,
    arm64 HA/perf 11/11.
  - Native engine benchmark result: exit 0.
  - Cluster cleanup: `zepto-bench-x86` and `zepto-bench-arm64` CPU limits
    returned to 0; no ZeptoDB pods remained.
- `./tests/k8s/run_eks_bench.sh --engine-only --keep`
  - Native benchmark result: exit 0.
  - Log directory: `/tmp/eks_bench_20260529_225701`.
- `/tmp` remained at 1% usage after filtered benchmark logging; final log
  footprints were 48 KiB (`arch_bench`), 32 KiB (`eks_bench`), and 12 KiB
  (`full_matrix`) for the final EKS run.

## Follow-ups

- The native benchmark parser is now stable, but some benchmark binaries still
  emit very noisy low-level allocator diagnostics under stress. Keep the
  filtered log path unless full raw logs are explicitly requested.
