# Experiment 026: Physical AI VLA Early Exit On EKS

Date: 2026-07-17
Status: Research complete
Classification: Research-only

## Goal

Determine whether confidence-gated reuse of historical actions retrieved from
ZeptoDB can reduce real VLA calls, online GPU time, and mean decision latency
without materially degrading offline action quality.

## Hypothesis

On a held-out LIBERO middle-frame replay, a calibrated ZeptoDB early exit will
reduce VLA calls by at least 30%, total online GPU time by at least 20%, and
mean decision latency by at least 15%, while normalized action MAE remains
within 5% or 0.005 absolute of direct VLA inference.

## Scope

- Public `lerobot/libero_10_image` data.
- Public `HuggingFaceVLA/smolvla_libero` at commit
  `6721902bc4d61e50a3bfdb11dfb4cb626f05d102`.
- Front camera, wrist camera, eight-dimensional state, and seven-dimensional
  recorded expert action from the middle frame of each episode.
- Ten tasks and 190 historical memories.
- Fifty calibration queries and a disjoint fifty-query evaluation set.
- SigLIP front-image plus instruction retrieval, with normalized proprioceptive
  state appended to the retrieval vector.
- Exact ZeptoDB Agent Memory search over five matches.

The experiment is offline replay. It does not run LIBERO simulation, measure
closed-loop task success, establish safety, or change ZeptoDB runtime behavior.

## Procedure

1. Resolve the deterministic 290-row data manifest before creating AWS
   resources.
2. Start temporary `c7i.xlarge` and `g6e.xlarge` EKS Auto Mode NodePools.
3. Deploy the current ZeptoDB bench image with Agent Memory ANN disabled.
4. Load SigLIP and the pinned SmolVLA policy on the NVIDIA L40S.
5. Insert 190 historical middle-frame embeddings and expert actions.
6. On 50 calibration queries, run retrieval and direct VLA inference, then
   select the threshold with the highest skip rate that satisfies the
   calibration action-MAE limit.
7. On the disjoint 50-query evaluation set, run direct VLA for every query.
8. Run the routed path: encode and search every query, reuse the retrieved
   action above the threshold, and invoke the real VLA below it.
9. Record CUDA-event time, wall latency, call counts, action MAE, peak allocated
   GPU memory, and NVML energy when supported.
10. Recover the compact result through `/dev/termination-log` and delete all
    temporary AWS resources before rendering the report.

## Acceptance Criteria

| Criterion | Required |
| --- | ---: |
| Pinned real SmolVLA loaded and invoked | pass |
| Evaluation VLA call reduction | at least 30% |
| Evaluation total online GPU-time reduction | at least 20% |
| Evaluation mean decision-latency reduction | at least 15% |
| Routed normalized action MAE | at most direct VLA limit |
| ZeptoDB exact-search p95 | below 30 ms |
| Temporary AWS resources deleted | pass |

The action-quality limit is the larger of direct VLA MAE multiplied by 1.05
and direct VLA MAE plus 0.005. Routed p95 latency is reported but is not an
acceptance gate because any fallback rate above 5% retains direct-VLA samples
in the p95 tail.

## Failure Behavior

Any manifest, dependency, model revision, CUDA, ZeptoDB, or cleanup error stops
the sequence. An acceptance failure still produces a result report after
cleanup. The runner refuses an unexpected AWS account or EKS endpoint and an
existing result path. It deletes only its unique namespace and NodePools,
leaves the shared bench NodePools unchanged, and publishes the rendered report
with an atomic no-clobber step. No runtime fallback silently changes the model
or search mode.

## Command

```bash
tests/k8s/run_physical_ai_vla_early_exit.sh
```

## Result

- The pinned 604,934,176-parameter SmolVLA ran on an NVIDIA L40S.
- Calibration selected threshold `0.890841` and skipped 49/50 queries while
  remaining inside its offline action-MAE limit.
- Evaluation skipped 50/50 VLA calls.
- Mean decision latency fell from 475.572 ms to 10.205 ms, a 97.9% reduction.
- Total online GPU time fell from 23,728.155 ms to 365.314 ms, a 98.5%
  reduction.
- Measured energy fell from 2,417.4 J to 60.7 J.
- Routed normalized action MAE was 0.099922 versus 0.107831 for direct VLA,
  below the routed limit of 0.113222.
- ZeptoDB exact-search p50/p95 was 2.298/2.565 ms.
- All acceptance and AWS cleanup criteria passed.

See `docs/research/results/physical_ai_vla_early_exit_026.md`.

## Interpretation

For this deterministic middle-frame replay, historical action reuse produced a
practical reduction in VLA calls, GPU work, energy, and mean latency without
worsening the offline action proxy. The result is unusually strong because all
50 evaluation samples exceeded the calibrated threshold.

This does not prove that a 100% skip rate is safe under temporal drift,
out-of-distribution scenes, or closed-loop control. The result remains
research-only and must not be described as simulator task success or a
production routing policy.

## Next Research Step

If the offline compute and quality gates pass, validate the router in a
closed-loop LIBERO simulator with episode success, temporal stability, and
false-early-exit analysis before considering any runtime integration.
