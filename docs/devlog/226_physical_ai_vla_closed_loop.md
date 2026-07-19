# 226: Physical AI VLA Closed-Loop EKS Harness

Date: 2026-07-17
Status: Complete

## Context

Experiment 026 showed a large offline compute reduction but skipped every
evaluation VLA call and did not execute a simulator. Experiment 027 adds real
closed-loop LIBERO task success and a guarded router with mandatory fallback.

## Changes

- Added a research-only paired direct/routed LIBERO-10 rollout harness.
- Added real simulator success extraction, paired regression accounting, and
  bounded confidence-streak and consecutive-skip guards.
- Reused the pinned Experiment 026 memory split, SigLIP encoder, SmolVLA model,
  and exact ZeptoDB Agent Memory path.
- Added focused tests for stable-confidence routing, task mismatch, forced
  fallback, empty paired results, vector success info, and compact output.
- Added a temporary EKS runner with pinned dependencies, MuJoCo EGL, compact
  result recovery, and idempotent namespace, NodePool, NodeClaim, node, and EC2
  cleanup.
- Added a fail-closed preflight that verifies the expected AWS account and
  `zepto-bench` API endpoint before any cluster mutation or cleanup trap.
- Added non-interactive LIBERO path initialization and CMake 4 compatibility
  for the upstream EGL probe packages.
- Mapped LIBERO suite tasks to dataset memory partitions by exact language
  instruction because the suite and dataset use different numeric orderings.
- Seeded CPU and CUDA policy sampling at each paired episode start so router
  quality is not confounded by unrelated SmolVLA sampling sequences.
- Made cleanup success depend on final Kubernetes and EC2 state rather than an
  intermediate Karpenter finalizer timeout.

## Verification

- Focused Experiment 027 Python tests: 9 passed.
- Python compile, shell syntax, and diff checks: passed.
- Pinned dependency installation and LIBERO config/init-state loading passed in
  the same PyTorch container image used by EKS.
- EKS L40S closed-loop run completed all ten paired LIBERO-10 tasks.
- Direct and routed simulator success both reached 5/10 with zero paired
  regressions.
- Routed confidence p50 was 0.737856 against the fixed 0.890841 threshold, so
  the router made 0 skips and fell back on 100% of decisions.
- Mean decision latency regressed 3.4% and online GPU time regressed 2.6%;
  ZeptoDB exact-search p95 was 2.671 ms.
- Temporary `c7i.xlarge` and `g6e.xlarge` instances terminated, experiment
  Kubernetes resources were absent, and shared bench NodePool CPU limits were
  zero.
- The x86_64/aarch64 C++ build matrix was not rerun because this change only
  adds Python research tooling and an NVIDIA L40S EKS harness; no C++, SIMD,
  storage layout, or architecture-sensitive runtime path changed.

## Boundary

This is research-only evidence. One initial state per task cannot establish
confidence intervals, temporal or distribution-shift robustness, collision
safety, or production readiness. The acceptance failure also shows that the
Experiment 026 offline threshold must not be promoted to an online routing
default.
