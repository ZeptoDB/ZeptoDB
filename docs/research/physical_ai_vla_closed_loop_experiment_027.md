# Experiment 027: Physical AI VLA Closed Loop On EKS

Date: 2026-07-17
Status: Research complete - acceptance failed
Classification: Research-only

## Goal

Test whether the Experiment 026 ZeptoDB action-reuse path retains real
closed-loop LIBERO task success while reducing VLA calls, GPU work, and
decision latency.

## Hypothesis

Across one paired initial state for each LIBERO-10 task, a guarded historical
action router will reduce VLA calls by at least 30% and mean decision latency
by at least 15%, while routed simulator success remains within ten percentage
points of direct VLA and no more than one direct-success task regresses.

## Scope

- Public `HuggingFaceVLA/smolvla_libero` pinned at
  `6721902bc4d61e50a3bfdb11dfb4cb626f05d102`.
- LeRobot 0.4.4 and `hf-libero` 0.1.4 with headless MuJoCo EGL.
- All ten LIBERO-10 tasks, one deterministic initial state per task.
- At most 520 control steps per task and policy variant.
- Direct VLA and routed VLA run from paired task, initial-state, and seed
  settings.
- The Experiment 026 memory split: 190 middle-frame historical actions.
- Exact ZeptoDB Agent Memory search over five candidates.

The fixed Experiment 026 confidence threshold is `0.890841`. The closed-loop
router additionally requires two consecutive high-confidence observations,
rejects top-match task mismatches, and forces VLA fallback after at most four
consecutive historical-action reuses.

## Procedure

1. Refresh the deterministic 290-row LIBERO manifest before creating AWS
   resources.
2. Start temporary `c7i.xlarge` and `g6e.xlarge` EKS Auto Mode NodePools.
3. Deploy ZeptoDB with Agent Memory ANN disabled.
4. Install the pinned LeRobot, LIBERO, CUDA, and EGL dependencies.
5. Load SigLIP and the pinned 604,934,176-parameter SmolVLA on NVIDIA L40S.
6. Encode and insert 190 historical middle-frame action memories.
7. For each LIBERO-10 task, create a fresh environment and run direct VLA from
   the paired initial state until success or 520 steps.
8. Recreate the same task environment and run the guarded ZeptoDB router from
   the same initial state and seed.
9. Record simulator success, paired regressions, steps, VLA calls, forced and
   confidence fallbacks, wall latency, CUDA-event time, ZeptoDB latency, and
   NVML energy when supported.
10. Recover the compact result through `/dev/termination-log`, delete all
    temporary resources, and render the immutable result report.

## Acceptance Criteria

| Criterion | Required |
| --- | ---: |
| Real LIBERO closed-loop steps executed | pass |
| Direct VLA simulator success | at least 1/10 |
| Routed success-rate delta versus direct VLA | no worse than -10pp |
| Paired direct-success regressions | at most 1 |
| VLA call reduction | at least 30% |
| Routed fallback rate | 5% to 95% |
| Mean decision-latency reduction | at least 15% |
| ZeptoDB exact-search p95 | below 30 ms |
| Temporary AWS resources deleted | pass |

## Failure Behavior

Any manifest, dependency, EGL, model, environment, ZeptoDB, timeout, or cleanup
error stops the sequence. An acceptance failure still produces a result report
after cleanup. The runner does not silently reduce the task count, episode
length, or model revision.

## Command

```bash
tests/k8s/run_physical_ai_vla_closed_loop.sh
```

## Result

See `docs/research/results/physical_ai_vla_closed_loop_027.md`.

The direct and routed paths each succeeded on 5/10 tasks with no paired
regressions. The fixed Experiment 026 threshold did not transfer to online
closed-loop observations: median routed confidence was `0.737856`, below the
fixed `0.890841` threshold, and the router made zero historical-action skips.
The apparent 0.1% VLA-call difference came from five fewer simulator steps on
one routed episode, not from action reuse.

| Check | Direct VLA | ZeptoDB routed |
| --- | ---: | ---: |
| Simulator success | 5/10 | 5/10 |
| Control steps | 4,291 | 4,286 |
| VLA calls | 4,291 | 4,286 |
| Historical-action skips | 0 | 0 |
| Mean decision latency | 454.169 ms | 469.756 ms |
| Online GPU time | 1,877,964.838 ms | 1,927,346.806 ms |
| Energy | 200,326.4 J | 204,915.6 J |

ZeptoDB exact-search p95 was 2.671 ms. The routed path had 100% fallback,
3.4% higher mean decision latency, and 2.6% more online GPU time. Temporary
EKS and EC2 resources were deleted and shared bench NodePool CPU limits were
returned to zero.

## Interpretation

Experiment 027 rejects the hypothesis for this threshold and memory
representation. ZeptoDB search itself was fast, and guarded fallback retained
paired task quality, but the router provided no useful early exits. Running
SigLIP and search before every fallback added work instead of reducing it.
The Experiment 026 offline confidence distribution therefore must not be used
as evidence of online compute savings.

## Next Research Step

Calibrate against sequential on-policy observations rather than isolated
dataset frames, then require a preflight run to demonstrate a non-trivial
skip/fallback slice before repeating the full ten-task evaluation. A successor
must use multiple initial states and seeds, report confidence by task and
trajectory phase, and add distribution-shift, stale-memory, and
corrupted-retrieval scenarios before any physical-robot or runtime claim.
