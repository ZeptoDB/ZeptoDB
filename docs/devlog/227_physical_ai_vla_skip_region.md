# 227: Physical AI VLA Skip Region Harness

Date: 2026-07-17
Status: Complete

## Context

Experiment 027 preserved paired LIBERO quality but made zero skips because its
offline threshold did not transfer to sequential on-policy observations.
Experiment 028 must identify a real online eligibility region before spending
on another full routed run.

## Changes

- Added task-specific ZeptoDB memory namespaces to eliminate cross-task
  retrieval candidates.
- Added shadow trace collection for confidence, top-1/top-2 margin, trajectory
  phase, candidate action error, and retrieval overhead.
- Added bounded threshold and margin search with explicit action-error,
  projected-latency, and 20% to 30% skip-rate constraints.
- Added a two-task closed-loop gate that stops the run before the remaining
  tasks on regression, insufficient skips, or insufficient latency savings.
- Limited action reuse to two consecutive eligible observations and at most
  two consecutive skips.
- Generalized the existing EKS VLA runner while preserving Experiment 027 as
  its default entrypoint.
- Added focused unit tests for phase boundaries, task resets, burst limits,
  candidate selection, empty input, invalid task IDs, and compact output.

## Verification

- Related Physical AI Python tests: 46 passed.
- Python compile, shell syntax, and diff checks: passed.
- EKS shadow calibration selected confidence `0.76` and minimum margin `0.01`:
  28.8% projected skips, mean/p95 normalized action MAE 0.1640/0.2612, and
  25.8% projected latency reduction.
- The closed-loop pilot had zero paired regressions but only 13.3% actual
  skips, so the staged gate stopped before the remaining eight tasks.
- ZeptoDB exact-search p95 was 2.593 ms.
- A post-run audit found that the pilot routed timer included `env.step()`
  while the shadow baseline did not. The invalid latency comparison is marked
  unusable in the result, and the timer now stops before simulator execution.
- Temporary EKS resources were absent, both `c7i.xlarge` and `g6e.xlarge`
  instances were terminated, and shared bench NodePool CPU limits were zero.
- Cross-architecture C++ matrix: not applicable; no C++, SIMD, storage-layout,
  or product runtime code changed.

## Boundary

This remains research-only tooling. The experiment found a shadow region, not
a stable compute-saving closed-loop policy. One initial state per task and
calibration on two deterministic tasks cannot establish robustness to seeds,
stale memory, camera shift, corrupted retrieval, or physical execution.

## Follow-Ups

- Run multiple seeds and initial states only if the staged deterministic run
  demonstrates real compute savings without unacceptable paired regressions.
- Add stale-memory, corrupted-retrieval, and out-of-distribution scenarios
  before considering any runtime path.
