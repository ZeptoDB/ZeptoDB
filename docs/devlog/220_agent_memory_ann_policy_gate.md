# 220: Agent Memory ANN Production Policy Gate

Date: 2026-07-11
Status: Complete

## Context

Sparse projection, optional HNSW, and IVF are already comparable in
`bench_agent_memory`, but the remaining production blocker was choosing a
default ANN policy from realistic embedding dumps and tenant-filter-heavy
workloads instead of synthetic global recall alone.

## Changes

- Added `--tenant-count` and `--query-tenant-index` to `bench_agent_memory` so
  real embedding dumps can be partitioned across many tenants while recall and
  latency are measured against one tenant-visible slice.
- Generalized clustered fixtures so cluster neighborhoods remain aligned when
  records are distributed across more than two tenants.
- Added recall average/minimum, ANN build-time, and optional ANN-memory
  threshold flags to the decision table.
- Replaced the old latency-only `ANN?` label with an explicit policy status:
  `scan_ok`, `needs_ann`, `eligible`, `inactive`, or a named rejection reason.
- Updated Agent Memory design, backlog, and completion docs to keep exact scan
  as the default until real production dumps pass the policy gate.

## Verification

- `cmake --build build --target bench_agent_memory -j$(nproc)`
- `./build/bench_agent_memory --fixture real --embedding-file tests/fixtures/agent_memory_real_embeddings.txt --records 8 --iters 1 --compare-ann --recall-queries 2 --tenant-count 4 --query-tenant-index 2 --skip-snapshot`
- Full x86_64 CTest:
  `ninja -C build -j$(nproc) zepto_tests && cd build && ctest -j$(nproc) -E "Benchmark\\.|K8s" --output-on-failure --timeout 180`
  - 1742/1742 passed; live S3 opt-in skipped.
- Full aarch64 Graviton stage:
  `./tools/run-full-matrix.sh --stages=8 --force-resync`
  - 1742/1742 passed; live S3 opt-in skipped.

## Follow-ups

- Run larger customer or production-like embedding dumps through the tenant-heavy
  policy gate before changing `--agent-memory-ann` defaults.
- Keep persisted ANN sidecars optional unless production dump results show ANN
  rebuild time, not exact-scan latency, is the operational bottleneck.
