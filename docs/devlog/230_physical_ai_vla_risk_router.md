# 230: Physical AI Risk-Partitioned Free-Space Router Harness

Date: 2026-07-18
Status: Complete

## Context

Experiment 030 found that confidence, safety, and cooldown overlapped on only
1.6% of observations. Directly weakening the safety gate would not provide
credible Physical AI evidence. Experiment 031 instead limits action reuse to a
cheaply identifiable free-space hold state and moves incompatible memories
into a non-executable suppression partition.

## Changes

- Added a research-only Experiment 031 harness with calibration, held-out
  shadow, and routed stages.
- Added hard task/gripper memory partitions for bounded hold candidates and a
  separate non-executable suppression partition.
- Added contact component telemetry that separates finger and arm/wrist
  contacts.
- Added a pre-retrieval free-space gate so contact, transition, ambiguous,
  outlier, and missing-partition states do not pay SigLIP or ZeptoDB cost.
- Added a negative-memory veto, independent confidence and candidate-safety
  checks, and mandatory VLA cooldown after direct reuse.
- Added immediate post-action contact checks and next-observation state-outlier
  checks.
- Added held-out shadow gates for reuse density, action error, and projected
  end-to-end latency before routed actions are permitted.
- Added focused Python tests and an EKS wrapper using the shared staged runner.

## Verification

- Focused Experiment 031 Python tests: 16 passed.
- Python compile, shell syntax, and diff checks: passed.
- `ruff` was unavailable in the local Python environment.
- EKS calibration shadow: 595 observations across tasks 0 and 5.
- Calibration result: 0/48 viable regions; stopped before held-out or routed
  execution.
- Precheck: 127 eligible, 390 finger contact, 58 missing executable memory,
  12 gripper movement, 6 state outlier, and 2 first observation.
- ZeptoDB combined search p95: 6.871 ms.
- Temporary namespace and NodePools deleted, temporary EC2 instances
  terminated, and shared bench NodePool CPU limits returned to zero.
- Cross-architecture C++ verification is not applicable because no C++, SIMD,
  storage layout, or product runtime code changed.

## Boundary

This is a research-only harness. The source demonstrations lack per-step
contact-force and post-action hazard labels, so `bounded_free_space_candidate`
means admission by bounded simulator proxies, not certified safe memory. The
harness does not change ZeptoDB runtime behavior or expose a product API.

## Follow-Ups

- Preserve the original Experiment 031 result as an immutable negative result.
- Experiment 032 completed the shadow-only instrumented replication and
  attributed the failure to a cooldown ceiling, configured negative veto, and
  asymmetric task/hold memory coverage rather than ZeptoDB search latency.
- Diagnose veto separability and rebuild phase-local contact-free trajectory
  memory before another threshold grid.
- Evaluate manipulation-phase VLA feature caching separately; do not permit
  cached features to bypass the independent action safety controller.
