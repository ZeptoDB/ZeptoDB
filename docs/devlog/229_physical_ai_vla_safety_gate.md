# 229: Physical AI VLA Confidence-Safety Gate Harness

Date: 2026-07-17
Status: Complete

## Context

Experiment 029 showed that one historical action causes trajectory drift but
does not immediately reduce retrieval eligibility. Experiment 030 separates
memory confidence from action safety and removes skip-induced policy RNG
desynchronization before another routed pilot.

## Changes

- Added a VLA-free safety index for contact, gripper transition, candidate
  consensus, motion bounds, neighbor disagreement, and state outliers.
- Added a strict confidence-then-safety decision path rather than combining
  confidence and risk into one score.
- Added a memory-derived gripper hold/transition boundary for the LIBERO action
  representation, whose gripper command is always -1 or +1.
- Added a mandatory VLA cooldown after every historical action.
- Added absolute control-step policy seeding to align direct and routed
  stochastic SmolVLA calls after skips.
- Added a shadow preflight for low-risk skip density and candidate-to-VLA
  action error.
- Added corrected decision latency, GPU time per decision, fallback reasons,
  risk distribution, unsafe-adoption, and post-skip proxy-hazard telemetry.
- Added focused tests for empty/malformed memory data, gripper boundary,
  disagreement, hard/soft risk, dimensions and limits, independent gate
  decisions, cooldown, deterministic step seeds, and compact output.

## Verification

- Related Physical AI Python tests: 74 passed.
- Python compile, shell syntax, and diff checks: passed.
- EKS NVIDIA L40S shadow execution completed 619 observations across tasks 0
  and 5.
- Safety classified 136 observations as low risk, 483 as high risk, and zero
  as medium risk.
- Confidence, safety, and cooldown together admitted 10/619 observations
  (1.6%), below the 20% preflight gate, so routed execution did not start.
- Admitted candidate-to-VLA normalized action MAE p95 was 0.0981; ZeptoDB
  exact-search p95 was 4.337 ms.
- Temporary experiment Kubernetes resources were absent, both temporary EC2
  instances terminated, and shared bench NodePool CPU limits were zero.
- Cross-architecture C++ matrix is not applicable because no C++, SIMD,
  storage-layout, or product runtime code changed.

## Boundary

This is research-only negative simulator evidence. The preflight demonstrates
that the current confidence-safe overlap is too small for practical compute
savings. It does not establish collision avoidance, physical contact safety,
calibrated risk probabilities, or a supported runtime policy.

## Follow-Ups

- Add component-level hard-risk rejection counts before changing thresholds.
- Separate expected grasp/contact from collision contact and validate gripper
  phase inference before another routed pilot.
