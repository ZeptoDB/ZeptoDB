# 225: Physical AI VLA Early Exit EKS Harness

Date: 2026-07-17
Status: Complete

## Context

Experiment 025 proved real vision retrieval but did not show that retrieval
reduces VLA inference work. Experiment 026 adds a real SmolVLA baseline and a
confidence-gated historical-action early exit.

## Changes

- Added a research-only two-camera LIBERO manifest and replay harness.
- Added a disjoint calibration/evaluation split and a quality-constrained
  threshold selector.
- Added direct-VLA and ZeptoDB-routed measurements for VLA calls, CUDA time,
  wall latency, offline action MAE, energy when supported, and peak GPU memory.
- Added focused tests for split boundaries, action aggregation, threshold
  calibration, invalid manifests, and compact-result limits.
- Added an EKS runner with pinned ML image and VLA revisions, exact Agent
  Memory search, termination-message result recovery, and owned cleanup.
- Added a fail-closed preflight that verifies the expected AWS account and
  `zepto-bench` API endpoint before any cluster mutation or cleanup trap.
- Removed shared-bench shutdown from the owned-NodePool cleanup path and made
  canonical report publication atomic and no-clobber.
- Added a low-rate three-worker dataset manifest resolver, shared 429 cooldown,
  and an atomic per-row checkpoint after the initial unauthenticated rows API
  attempt exhausted its retries before AWS provisioning.
- Added the Linux compiler and userspace kernel headers required by LeRobot's
  indirect `evdev` source build, plus termination-message capture for dependency
  setup failures.
- Made cleanup idempotent and added EC2 lookup by EKS NodePool tag when
  short-lived NodeClaims disappear before cleanup verification.

## Verification

- Focused Python tests: 21 passed, including 11 Experiment 026 tests.
- Python compile check: passed.
- Shell syntax check: passed.
- Initial manifest attempt: stopped before AWS provisioning after persistent
  HTTP 429 responses for dataset row 28549.
- Low-rate manifest retry: all 290 rows resolved without another 429.
- First GPU Job: dependency setup stopped before model loading because `evdev`
  could not build without Linux headers; both temporary EC2 instances were
  terminated and the shared bench NodePools returned to CPU limit 0.
- Second GPU Job: dependency setup and harness startup passed, then SigLIP
  initialization exposed a missing `sentencepiece` runtime; the runner recovered
  the structured error and terminated both temporary EC2 instances.
- Final EKS L40S run: pass. Direct VLA averaged 475.572 ms per decision and
  consumed 23,728.155 ms of CUDA time over 50 evaluation queries. The routed
  path skipped 50/50 VLA calls, averaged 10.205 ms, and consumed 365.314 ms of
  CUDA time, reducing mean latency by 97.9% and online GPU time by 98.5%.
- Offline normalized action MAE was 0.107831 for direct VLA and 0.099922 for
  the routed path. ZeptoDB exact-search p95 was 2.565 ms.
- NVML energy was 2,417.4 J for direct VLA and 60.7 J for the routed path.
- Cleanup verification: the temporary `c7i.xlarge` and `g6e.xlarge` instances
  reached `terminated`; namespace, NodePools, NodeClaims, and nodes were absent;
  shared bench NodePool CPU limits remained 0.

## Boundary

This is research-only offline replay. It does not modify ZeptoDB runtime
behavior and does not prove simulator success, control safety, or production
VLA routing readiness.

## Follow-ups

- Run closed-loop LIBERO episodes and measure task success, temporal stability,
  false early exits, and out-of-distribution fallback behavior.
- Require a non-trivial fallback slice and drifted evaluation set before
  promoting any runtime router.
