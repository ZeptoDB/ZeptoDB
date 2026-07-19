# Experiment 028: Physical AI VLA Skip Region Discovery On EKS

Date: 2026-07-17
Status: Research complete - acceptance failed
Classification: Research-only

## Goal

Find a bounded region of sequential LIBERO observations where a
task-partitioned ZeptoDB historical action can replace a SmolVLA call with
measurable latency and GPU savings while preserving paired simulator success.

## Hypothesis

Task-partitioned retrieval plus on-policy shadow calibration can identify a
confidence and top-1/top-2-margin region that produces 20% to 30% eligible
skips, at least 15% lower mean decision latency, and no more than one paired
task regression across LIBERO-10.

## Scope

- Public `HuggingFaceVLA/smolvla_libero` pinned at
  `6721902bc4d61e50a3bfdb11dfb4cb626f05d102`.
- LeRobot 0.4.4 and `hf-libero` 0.1.4 with headless MuJoCo EGL.
- The same 190 historical middle-frame action memories as Experiments 026 and
  027, stored in one ZeptoDB namespace per current task.
- One deterministic initial state per LIBERO-10 task and at most 520 control
  steps per episode.
- Pilot tasks 0 and 5, both direct-VLA successes in Experiment 027.
- Two consecutive eligible observations and at most two consecutive skips.

This experiment does not claim multi-seed robustness, physical-robot safety,
or a production routing policy.

## Procedure

1. Refresh the deterministic LIBERO sample manifest before creating AWS
   resources.
2. Start temporary `c7i.xlarge` and `g6e.xlarge` EKS Auto Mode NodePools.
3. Load pinned SmolVLA and SigLIP on NVIDIA L40S and insert memories into ten
   task-specific ZeptoDB namespaces.
4. Run direct SmolVLA on pilot tasks 0 and 5 while shadow retrieval records
   confidence, top-1/top-2 margin, trajectory phase, candidate-to-VLA
   normalized action MAE, encoder latency, and search latency.
5. Search confidence thresholds from 0.68 through 0.82 and margin thresholds
   0, 0.005, 0.01, and 0.02.
6. Freeze a region only when its simulated guarded skip rate is 20% to 30%,
   mean action MAE is at most 0.25, p95 action MAE is at most 0.75, and
   projected latency reduction is at least 15%.
7. Run the frozen router on the two pilot tasks. Stop before the remaining
   tasks if either task regresses, actual skip rate is below 20%, or actual
   mean latency reduction is below 15%.
8. If the pilot passes, run direct shadow and routed episodes for the
   remaining eight tasks without changing the frozen region.
9. Report actual simulator success, phase-specific eligibility, VLA calls,
   decision latency, GPU time per decision, ZeptoDB search latency, and paired
   regressions.
10. Recover compact results through `/dev/termination-log` and delete all
    temporary Kubernetes and EC2 resources.

## Acceptance Criteria

| Criterion | Required |
| --- | ---: |
| Task-partitioned retrieval | pass |
| Viable shadow skip region | pass |
| Two-task closed-loop pilot | pass |
| Routed success-rate delta | no worse than -10pp |
| Paired direct-success regressions | at most 1 |
| Actual historical-action skip rate | at least 20% |
| Actual mean decision-latency reduction | at least 15% |
| ZeptoDB exact-search p95 | below 30 ms |
| Temporary AWS resources deleted | pass |

## Failure Behavior

Manifest, dependency, model, EGL, simulator, ZeptoDB, timeout, compact-result,
or cleanup errors stop the sequence. A calibration or pilot acceptance failure
produces a result and prevents the remaining eight tasks from starting. The
runner cleans up before failure investigation and does not use pod logs as the
result channel.

## Command

```bash
tests/k8s/run_physical_ai_vla_skip_region.sh
```

## Result

The staged run found a shadow candidate at confidence `0.76` and minimum
top-1/top-2 margin `0.01`. With the two-hit and two-skip guards, it marked
231/801 observations eligible (28.8%), had mean/p95 normalized candidate
action MAE of 0.1640/0.2612, and projected 25.8% lower latency. ZeptoDB search
p95 was 2.593 ms.

The closed-loop pilot had no paired regression, but only 13.3% of decisions
actually skipped SmolVLA, below the 20% gate. The run therefore stopped before
the remaining eight tasks. A post-run audit found that the pilot routed timer
included simulator execution while the shadow baseline did not; the pilot
latency percentage is invalid and the harness was corrected for future runs.
The skip-rate failure independently rejects the acceptance hypothesis.

All temporary Kubernetes resources were absent after cleanup, both temporary
EC2 instances were terminated, and the shared bench NodePool CPU limits were
zero. See
`docs/research/results/physical_ai_vla_skip_region_028.md`.

## Interpretation

Task partitioning removed cross-task retrieval ambiguity and a useful shadow
region exists. It did not remain dense enough after historical actions changed
the trajectory. The next experiment should model trajectory shift directly,
calibrate with short action-reuse bursts in shadow forks or simulator clones,
and use a corrected decision timer before another full ten-task run.

## Next Research Step

Do not broaden to confidence `0.68` merely because it projects 63.9% shadow
skips. First run multiple short paired pilot forks across thresholds and
phases, measure post-skip state divergence, and select a region by actual
closed-loop skip persistence and task outcome.
