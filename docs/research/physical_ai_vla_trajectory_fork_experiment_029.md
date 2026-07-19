# Experiment 029: Physical AI VLA Trajectory Fork On EKS

Date: 2026-07-17
Status: Research complete - tested cause not supported
Classification: Research-only

## Goal

Determine whether the Experiment 028 closed-loop skip-rate collapse is caused
by a retrieved historical action moving the robot into observations with lower
retrieval confidence and eligibility.

## Hypothesis

Starting from an identical MuJoCo state, one historical action will create
measurable robot-state and front-camera divergence from the direct VLA control
branch. Over the following four paired VLA steps, the historical branch will
show at least a ten-percentage-point eligibility drop or a mean confidence
drop of at least 0.01.

## Scope

- Pinned `HuggingFaceVLA/smolvla_libero` and SigLIP models from Experiments
  026-028.
- The same 190 task-partitioned historical action memories as Experiment 028.
- LIBERO-10 tasks 0 and 5, seed base 28, and at most 520 shadow steps.
- Three Experiment 028 candidate regions:
  - `broad`: confidence 0.68, minimum margin 0.
  - `middle`: confidence 0.72, minimum margin 0.005.
  - `selected`: confidence 0.76, minimum margin 0.01.
- At most one first eligible snapshot per task, phase, and region.
- One historical-action intervention per fork, followed by at most four
  deterministically paired VLA recovery steps.

This experiment isolates short-horizon simulator causality. It does not test a
production router, broad task success, physical safety, or multi-seed
robustness.

## Procedure

1. Refresh the balanced LIBERO manifest before creating AWS resources.
2. Start temporary `c7i.xlarge` and `g6e.xlarge` EKS Auto Mode NodePools.
3. Load pinned models and insert memories into task-specific ZeptoDB
   namespaces.
4. Run direct VLA shadow trajectories on tasks 0 and 5.
5. When a region is eligible for two consecutive observations, retain the
   first MuJoCo state snapshot in each early, middle, or late phase.
6. Create two fresh LIBERO environments and restore both to the same snapshot.
   Verify normalized state error at most `1e-8` and exact front pixels.
7. Apply the recorded direct VLA action to the control environment and the
   retrieved historical action to the branch environment.
8. At offsets 1 through 4, measure normalized robot-state MAE, front-pixel
   MAE, ZeptoDB/SigLIP confidence, and region eligibility.
9. Use the same policy seed for paired VLA recovery actions at each offset.
10. Test whether intervention action error correlates with subsequent state
    drift and whether the historical branch loses confidence or eligibility
    relative to control.
11. Recover compact results from `/dev/termination-log` and delete all
    temporary resources.

## Cause-Support Criteria

| Criterion | Required |
| --- | ---: |
| Fork coverage | at least 6, 2 tasks, 2 phases |
| Paired restored state | state MAE <= 1e-8 and pixel MAE = 0 |
| Post-action trajectory shift | normalized state MAE p50 >= 0.01 |
| Eligibility effect | eligibility drop >= 10pp or confidence drop >= 0.01 |
| Dose-response support | action-error/state-drift correlation >= 0.30 |
| ZeptoDB search p95 | below 30 ms |
| Temporary AWS resources deleted | pass |

Trajectory shift plus an eligibility effect is the causal core. Dose response
is reported separately because the bounded fork count may not support a stable
correlation estimate.

## Failure Behavior

Dependency, model, snapshot restore, simulator, ZeptoDB, timeout, compact
result, or cleanup errors stop the run. Fewer than six eligible fork targets
stop before counterfactual execution. Cleanup runs before failure
investigation, and pod logs are not used as the result channel.

## Command

```bash
tests/k8s/run_physical_ai_vla_trajectory_fork.sh
```

## Result

Fourteen counterfactual forks covered both tasks and all three trajectory
phases. Restoration was exact: maximum paired normalized state and front-pixel
MAE were both zero. One historical action produced normalized state drift p50
0.0113 and front-pixel MAE p50 0.00413. Historical-to-VLA action error strongly
predicted state drift (`r=0.960`).

The required retrieval effect did not occur. Control and historical branches
were each eligible on 94.6% of the 56 post-action observations, for a
zero-percentage-point eligibility difference. Mean branch-minus-control
confidence was -0.0002, far below the required 0.01 drop. Exact ZeptoDB search
p95 was 5.486 ms.

The causal hypothesis therefore failed: one historical action does shift the
short-horizon state and image trajectory, but that shift does not explain
Experiment 028's eligibility collapse. Temporary Kubernetes resources were
absent after cleanup, both temporary EC2 instances terminated, and shared
bench NodePool CPU limits returned to zero. See
`docs/research/results/physical_ai_vla_trajectory_fork_029.md`.

## Interpretation

The retrieval embedding is robust to the measured one-action trajectory shift.
Experiment 028's lower routed skip rate is more likely to involve repeated
historical-action bursts, which this experiment intentionally excluded, or
stochastic policy-stream divergence. In Experiment 028, a skip did not execute
SmolVLA and therefore did not consume the random samples that the direct path
used at that control step; a later fallback was no longer sampling from the
same per-step random stream.

## Next Research Step

Before changing thresholds, seed SmolVLA by absolute task/episode/control-step
for every fallback so direct and routed paths remain paired after skips. Then
compare one versus two consecutive historical actions and sample eligible
points across each phase instead of only the first stable pocket.
