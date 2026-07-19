# Physical AI Calibration Failure Attribution 032 Results

Generated at: 2026-07-18T11:24:35Z
Classification: Research-only
Status: pass

## Result

Completed at `calibration_failure_attribution_complete` before routed execution.
Calibration scope: tasks 0, 5; 595 shadow steps.
Calibration finding: no viable grid point.
Experiment 031 replication fingerprint: steps=match, precheck=match, memory partitions=match, semantic manifest=match, VLA revision=match.
AWS temporary-resource cleanup: pass.

No memory action was executed. The grid points reuse the same ordered
calibration traces; they are parameter counterfactuals, not independent
trials. This diagnostic cannot establish physical-robot safety.

## Diagnostic Acceptance

| Check | Status |
| --- | --- |
| counts consistent | pass |
| grid 48 complete | pass |
| route attribution complete | pass |
| routed actions zero | pass |
| aws resources deleted | pass |

## Provenance And Timing

- Run ID: `4639958745`.
- VLA revision: `6721902bc4d61e50a3bfdb11dfb4cb626f05d102`.
- SigLIP model/revision: `google/siglip-base-patch16-224` / `7fd15f0689c79d79e38b1c2e2e2370a7bf2761ed`.
- Harness bundle SHA-256: `2237e57106cccb58a525d37fcdfac325bbbaf2f8bac93a2af2310f7cdb69d25f`.
- Semantic manifest SHA-256: `0624630ce232f33c36dbe20159ce3e88729ab0feffb6b67905f4bd2b180e85ba`.
- Calibration seed: 28.

| Timing component | Value (ms) |
| --- | ---: |
| VLA policy mean | 440.576 |
| Query encoder mean / p95 | 10.643 / 11.486 |
| Positive search mean | 3.770 |
| Suppression search mean | 3.349 |
| Combined search mean / p95 | 7.119 / 7.741 |

## Structural Funnel

The pooled reuse floor requires 119/595 actions (20.0%). The table shows counterfactual ceilings on this one
trace at the configured safety and veto settings.

| Counterfactual stage | Reuses | Rate | Projected latency reduction |
| --- | ---: | ---: | ---: |
| Precheck candidates, no cooldown | 127 | 21.3% | N/A |
| Candidates + cooldown | 65 | 10.9% | 10.1% |
| Candidate-safety proxy + cooldown | 65 | 10.9% | N/A |
| Veto + cooldown | 2 | 0.3% | N/A |
| Candidate-safety proxy + veto, no cooldown | 4 | 0.7% | N/A |
| Candidate-safety proxy + veto + cooldown | 2 | 0.3% | -0.5% |

The configured candidate-safety proxy rejected 0 candidates; the negative-veto proxy matched 123. They may overlap, and
their individual counts are not causal effect estimates or physical-safety labels.

| Precheck outcome | Count |
| --- | ---: |
| eligible | 127 |
| finger_contact | 390 |
| first_observation | 2 |
| gripper_motion | 12 |
| state_outlier | 6 |
| no_executable_memory | 58 |

| Task | Steps | Precheck outcomes |
| ---: | ---: | --- |
| 0 | 377 | eligible=127, finger_contact=242, first_observation=1, gripper_motion=7 |
| 5 | 218 | finger_contact=148, first_observation=1, gripper_motion=5, state_outlier=6, no_executable_memory=58 |

| Task | Open hold | Closed hold | Suppression |
| ---: | ---: | ---: | ---: |
| 0 | 13 | 1 | 5 |
| 5 | 9 | 0 | 10 |

## Calibration Gate Attribution

A grid point may fail more than one gate, so counts overlap. Gate
frequency depends on the configured threshold/margin grid and is not an
observation-level or causal failure frequency.
Zero-reuse points have no measured MAE and are shown as N/A; their
quality bits mean the quality requirement could not be demonstrated.

| Gate | Requirement | Failed points | Passed points |
| --- | --- | ---: | ---: |
| reuse_below_min | >= 20.0% | 48 | 0 |
| reuse_above_max | <= 35.0% | 0 | 48 |
| mean_action_mae | <= 0.100000 | 18 | 30 |
| p95_action_mae | <= 0.150000 | 18 | 30 |
| projected_latency_reduction | >= 15.0% | 48 | 0 |

Failure-mask histogram: 17 (reuse_below_min, projected_latency_reduction)=30; 29 (reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction)=18.
No-reuse grid points: 18.

### Representative frontier

The closest point minimizes failed-gate count, then normalized gate
violation. It is a descriptive heuristic, not an optimum or held-out
recommendation. Pareto points are non-dominated on pooled reuse, mean/p95
MAE, and projected latency; identical metric rows are deduplicated.

Pareto grid indexes: 0.

| Anchor | Grid | Threshold | Margin | Reuse | Mean MAE | p95 MAE | Latency | Failures |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| closest | 0 | 0.600 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| max reuse | 0 | 0.600 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| best latency | 0 | 0.600 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| best mean | 0 | 0.600 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| best p95 | 0 | 0.600 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |

### Closest candidate routing breakdown

Grid 0 uses confidence 0.600 and
margin 0.000. Routing outcomes below are the
first failing condition in route order and therefore do not represent
independent gate counts.

| Route outcome | Count |
| --- | ---: |
| accepted | 2 |
| no_candidate | 468 |
| negative_veto | 123 |
| cooldown | 2 |

| Task | Steps | Candidates | Reuses | Mean MAE | p95 MAE | Latency |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 377 | 127 | 2 | 0.050840 | 0.054028 | -0.8% |
| 5 | 218 | 0 | 0 | N/A | N/A | 0.0% |

| Task | Hold partition | Candidates | Reuses | Mean MAE | p95 MAE |
| ---: | --- | ---: | ---: | ---: | ---: |
| 0 | open | 127 | 2 | 0.050840 | 0.054028 |

## All 48 Calibration Points

| Grid | Threshold | Margin | Reuse | Mean MAE | p95 MAE | Latency | Failures |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 0 | 0.600 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 1 | 0.600 | 0.005 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 2 | 0.600 | 0.010 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 3 | 0.620 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 4 | 0.620 | 0.005 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 5 | 0.620 | 0.010 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 6 | 0.640 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 7 | 0.640 | 0.005 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 8 | 0.640 | 0.010 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 9 | 0.660 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 10 | 0.660 | 0.005 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 11 | 0.660 | 0.010 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 12 | 0.680 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 13 | 0.680 | 0.005 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 14 | 0.680 | 0.010 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 15 | 0.700 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 16 | 0.700 | 0.005 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 17 | 0.700 | 0.010 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 18 | 0.720 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 19 | 0.720 | 0.005 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 20 | 0.720 | 0.010 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 21 | 0.740 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 22 | 0.740 | 0.005 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 23 | 0.740 | 0.010 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 24 | 0.760 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 25 | 0.760 | 0.005 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 26 | 0.760 | 0.010 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 27 | 0.780 | 0.000 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 28 | 0.780 | 0.005 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 29 | 0.780 | 0.010 | 2/595 (0.3%) | 0.050840 | 0.054028 | -0.5% | reuse_below_min, projected_latency_reduction |
| 30 | 0.800 | 0.000 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 31 | 0.800 | 0.005 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 32 | 0.800 | 0.010 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 33 | 0.820 | 0.000 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 34 | 0.820 | 0.005 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 35 | 0.820 | 0.010 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 36 | 0.840 | 0.000 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 37 | 0.840 | 0.005 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 38 | 0.840 | 0.010 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 39 | 0.860 | 0.000 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 40 | 0.860 | 0.005 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 41 | 0.860 | 0.010 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 42 | 0.880 | 0.000 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 43 | 0.880 | 0.005 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 44 | 0.880 | 0.010 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 45 | 0.900 | 0.000 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 46 | 0.900 | 0.005 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |
| 47 | 0.900 | 0.010 | 0/595 (0.0%) | N/A | N/A | -0.9% | reuse_below_min, mean_action_mae, p95_action_mae, projected_latency_reduction |

## Interpretation

The most common grid gate flag(s) were `reuse_below_min`, `projected_latency_reduction` (48/48 points). This describes this grid,
not independent failures or causal dominance.
Experiment 032 matches all preserved Experiment 031 aggregate and
semantic-input anchors. It is an instrumented replication of that
setup, not recovery of the discarded original per-step trace.
The cooldown-only ceiling is already below the reuse floor, so
threshold or margin tuning cannot make this design viable. Candidate
coverage and/or cooldown policy would need to change on this trace.
Metrics are pooled across two episodes and weighted by their step counts;
they do not prove that either task individually passes. Task and hold MAE
must be interpreted with their accepted-action counts, especially for p95.
Projected latency uses measured encoder plus combined positive/suppression
search time and VLA fallback time; it excludes cheap precheck and routing
overhead. Any next experiment should remain shadow-only until a held-out
region clears every gate.

## Decision And Next Experiment

The 48 grid points collapse to 2 distinct outcome rows.
Every configured margin produced the same outcome at a given
confidence threshold, so another margin sweep over this memory bank
would add little information.
The configured negative veto matched 123/127 candidates (96.9%), while the configured candidate-safety proxy rejected 0.
This identifies veto separability as the next uncertainty; it is not
evidence that the veto should be weakened.
Mean encoder plus search cost was 17.762 ms versus a
440.576 ms VLA call (4.0%). Search latency is
not the binding constraint at the observed reuse density.
Task(s) without a closest-point candidate: 5;
missing-executable-memory precheck blocks: 58.
Rebuild memory from contact-free, phase-local trajectory windows
before interpreting another threshold grid.
The next pre-registered experiment should remain shadow-only and record
positive/suppression similarity gaps, phase support, neighbor
disagreement, admission reasons, and candidate-to-VLA error for vetoed
and allowed candidates. It should abort before a grid unless per-task
coverage and the candidate-plus-cooldown ceiling can satisfy both reuse
and projected-latency floors.
