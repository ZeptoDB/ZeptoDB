# 228: Physical AI VLA Trajectory Fork Harness

Date: 2026-07-17
Status: Complete

## Context

Experiment 028 found a 28.8% shadow skip region, but actual eligibility fell to
13.3% after historical actions were executed. Experiment 029 isolates whether
one retrieved action causes the subsequent observation and confidence shift.

## Changes

- Added named confidence/margin regions and independent consecutive-hit
  tracking.
- Added MuJoCo state snapshot capture at early, middle, and late eligible
  observations.
- Added paired state restoration into fresh control and historical-action
  environments with exact-state and exact-pixel checks.
- Added one-action counterfactual forks followed by four deterministically
  seeded paired VLA recovery steps.
- Added normalized state drift, front-pixel drift, confidence delta,
  eligibility retention, and action-error/state-drift correlation metrics.
- Added bounded cause-support criteria and compact Kubernetes termination
  output.
- Added focused tests for malformed regions, independent streak reset, empty
  and mismatched dimensions, zero-variance correlation, summary aggregation,
  causal criteria, and compact-output limits.

## Verification

- Related Physical AI Python tests: 59 passed.
- Python compile, shell syntax, and diff checks: passed.
- EKS NVIDIA L40S execution completed 14 paired forks across two tasks and
  three phases.
- Paired restoration was exact with zero maximum state and front-pixel error.
- Historical action MAE averaged 0.1415; post-action state drift p50 was
  0.0113, front-pixel MAE p50 was 0.00413, and action-error/state-drift
  correlation was 0.960.
- Control and historical branches both retained 94.6% eligibility. Mean
  branch-minus-control confidence was -0.0002, so the tested immediate
  confidence-collapse cause was rejected.
- Exact ZeptoDB search p95 was 5.486 ms.
- Temporary experiment Kubernetes resources were absent, both temporary EC2
  instances terminated, and shared bench NodePool CPU limits were zero.
- Cross-architecture C++ matrix is not applicable because no C++, SIMD,
  storage-layout, or product runtime code changed.

## Boundary

This is research-only negative evidence over two deterministic LIBERO tasks.
It rejects the immediate one-action confidence-collapse hypothesis for the
tested forks, but does not rule out repeated-action drift, policy RNG stream
misalignment, physical-robot effects, or long-horizon safety failures.

## Follow-Ups

- Seed policy sampling by absolute control step before another routed pilot so
  skips cannot desynchronize fallback randomness from the baseline.
- Compare one and two consecutive historical actions and sample more than the
  first eligible pocket per phase.
