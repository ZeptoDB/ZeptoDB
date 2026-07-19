# 231: Physical AI VLA Calibration Failure Attribution

Date: 2026-07-18
Status: Complete

## Context

Experiment 031 stopped after none of 48 confidence/margin regions passed its
calibration gate. Its Kubernetes termination payload preserved only aggregate
precheck counts and search p95, so the individual reuse, action-error, latency,
and gate-failure evidence cannot be reconstructed from the immutable result.

Experiment 032 is a new research-only, shadow-only diagnostic. Its success
criterion is complete failure attribution, not routing viability.

## Changes

- Added full-precision gate decisions and compact six-decimal telemetry for all
  48 threshold/margin points.
- Added five-bit overlapping failure masks, gate counts, signature counts, and
  explicit handling for zero-reuse points without non-standard `Infinity` or
  `NaN` JSON values.
- Added candidate, cooldown, safety, negative-veto, combined fixed-gate, and
  projected-latency ceilings over one ordered trace.
- Added first-failing route reasons plus task and hold-partition reuse and
  action-error breakdowns for the closest descriptive point.
- Added per-task precheck funnels, scoped memory availability, encoder and
  positive/suppression search timing, model provenance, and code/data hashes.
- Added a dedicated Experiment 032 entry point that forces experiment identity
  and diagnostic-only mode, even if extra CLI arguments attempt to override
  them.
- Added shared-runner identity validation and a dedicated EKS wrapper/result
  stem for Experiment 032.
- Preserved the original Experiment 031 result artifact unchanged.

## Result

- EKS run `4639958745` completed 595 shadow steps and all 48 diagnostic grid
  rows without entering held-out or routed execution.
- All five Experiment 031 replication anchors matched: step count, aggregate
  precheck funnel, memory partitions, semantic manifest, and VLA revision.
- Every grid point failed the 20% reuse and 15% projected-latency gates. Thirty
  points reused 2/595 actions; the other 18 reused none and therefore had no
  measured action-quality evidence.
- The precheck admitted 127/595 observations, but the mandatory cooldown capped
  reuse at 65/595 (10.9%). The configured negative veto matched 123/127
  candidates, leaving four before cooldown and two after it; the configured
  candidate-safety proxy rejected none.
- All candidates came from suite task 0 `open_hold`. Suite task 5 supplied no
  candidate and incurred 58 missing-executable-memory rejections. Experiments
  033-034 later corrected the source attribution: suite task 5 maps to manifest
  task 9, whose missing executable partition is `open_hold`, not task-5
  closed-hold.
- Combined ZeptoDB search p95 was 7.741 ms. Mean encoder plus search cost was
  17.762 ms versus a 440.576 ms VLA call, so density rather than search latency
  was the compute bottleneck.
- Diagnostic telemetry acceptance passed and every temporary namespace,
  NodePool, NodeClaim, and EC2 instance was removed. Shared bench NodePool CPU
  limits returned to zero.

## Verification

- Focused risk-router and attribution tests: 43 passed.
- Related Experiments 024-032 Python tests: 117 passed.
- Python compile, shell syntax, and diff checks: passed.
- Standard JSON and exact 3,900-byte write boundary: passed.
- Adversarial bounded compact payload: 3,436 bytes, leaving 464 bytes of
  termination-message headroom.
- `ruff` and `black` are unavailable in the local Python environment.
- EKS Experiment 032 diagnostic: passed; calibration viability: failed as a
  research finding.
- Cross-architecture C++ verification is not applicable because no C++, SIMD,
  storage layout, or ZeptoDB product runtime code changed.

## Boundary

This instrumentation does not change ZeptoDB product runtime behavior or
expose a product API. The 48 points share two simulator traces and are not
independent trials. No retrieved action is permitted to execute. The result
cannot certify safety or production readiness.

## Follow-Ups

- Run a pre-registered shadow-only veto-separability experiment that measures
  positive/suppression similarity gaps and candidate-to-VLA error for vetoed
  and allowed candidates without weakening the veto.
- Rebuild contact-free memory from phase-local trajectory windows, including
  suite-task-5/manifest-task-9 `open_hold`, and require a structural density/latency preflight
  before another threshold grid.
- Only after that preflight passes, run task-balanced multi-seed held-out
  shadow validation.
- Keep manipulation-phase VLA feature caching separate from direct action
  reuse.

## Retrospective Correction

Experiment 034 reproduced all 127 candidate row values and showed that the
immutable Experiment 032 source table used suite IDs as manifest indexes. The
48/48 reuse and latency failures, cooldown ceiling, and zero routed actions
remain valid. Only the source-partition label and its closed-hold interpretation
are invalidated; the actual gap is suite task 5 -> manifest task 9 `open_hold`.
