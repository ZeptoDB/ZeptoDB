# Physical AI VLA Skip Region 028 Results

Generated at: 2026-07-17T16:04:16Z
Classification: Research-only
Status: fail
Suite: `libero_10`
VLA: `HuggingFaceVLA/smolvla_libero` at `6721902bc4d61e50a3bfdb11dfb4cb626f05d102`
Retrieval encoder: `google/siglip-base-patch16-224`

## Result

The staged run stopped at `closed_loop_pilot_gate_failed`.

Shadow steps: 801; successful pilot baselines: 1/2.
ZeptoDB search p95: 2.593 ms.

## Shadow Region

| Confidence | Min margin | Skip rate | Mean action MAE | p95 action MAE | Projected latency reduction |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0.760 | 0.010 | 28.8% | 0.1640 | 0.2612 | 25.8% |

The broadest shadow-safe candidate was confidence `0.68` with no margin
minimum. It projected 63.9% skips and 60.9% lower latency with mean/p95 action
MAE of 0.1925/0.3558, but it was outside the planned 20% to 30% calibration
target and was not executed in closed loop.

## Closed-Loop Pilot

- Paired regressions: 0.
- Actual skip rate: 13.3%.
- Actual latency reduction: not comparable; this run's routed timer included the simulator step while the shadow baseline did not.

No full ten-task routed run was started after the failed gate.

## Acceptance

| Criterion | Status |
| --- | --- |
| Task-partitioned retrieval | pass |
| Viable shadow skip region | pass |
| No paired pilot regression | pass |
| Actual pilot skip rate at least 20% | fail, 13.3% |
| Comparable actual latency reduction at least 15% | not evaluated |
| ZeptoDB exact-search p95 below 30 ms | pass, 2.593 ms |
| Temporary AWS resources deleted | pass |

## Interpretation

Experiment 028 found an offline-on-policy shadow region, but not a stable
closed-loop skip region. Executing retrieved actions changed subsequent
observations enough that eligibility fell from 28.8% to 13.3%. This is useful
potential evidence, not a demonstrated compute-saving policy.

The pilot decision-timer scope mismatch was found after cleanup. Skip counts,
action-error calibration, simulator success, regression count, and ZeptoDB
latency remain valid; the reported pilot latency reduction must not be used.
The harness now stops its decision timer before `env.step()` for a future run.

Temporary `c7i.xlarge` and `g6e.xlarge` instances reached `terminated`; the
experiment namespace, NodePools, and NodeClaims were absent; shared bench
NodePool CPU limits were zero.
