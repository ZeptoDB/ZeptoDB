# Experiment 025: Physical AI Vision Retrieval On EKS

Date: 2026-07-16
Status: Research complete
Classification: Research-only

## Goal

Replace Experiment 024's deterministic observation proxy with real robot
camera images and a real vision-language encoder, then measure retrieval
quality, GPU encoder cost, and ZeptoDB Agent Memory latency.

## Hypothesis

SigLIP embeddings from LIBERO front-camera frames can retrieve episodes of the
same task, and adding the task instruction to the image embedding will achieve
at least 0.80 Recall@1 and 0.95 Recall@5 while ZeptoDB search remains below
30 ms p95 for this bounded memory set.

## Scope

- Public Apache-2.0 `lerobot/libero_10_image` data.
- One middle front-camera frame per selected episode.
- Ten tasks, 19 memory episodes and 10 held-out query episodes per task.
- Public `google/siglip-base-patch16-224` encoder on an NVIDIA L40S.
- Image-only and normalized image-plus-instruction embedding variants.
- Real ZeptoDB Agent Memory insert and search APIs.

The balanced split uses 19 rather than 20 memory episodes because task 6 has
only 29 episodes. It preserves ten held-out queries for every task.

## Procedure

1. Resolve the 290 selected dataset rows locally into a temporary image URL
   manifest before creating billable AWS resources.
2. Create unique temporary EKS Auto Mode NodePools restricted to
   `c7i.xlarge` for ZeptoDB and `g6e.xlarge` for CUDA.
3. Deploy the current ZeptoDB amd64 bench image on the Intel CPU node.
4. Download the selected middle front-camera frames from the image CDN.
5. Encode all 290 images with SigLIP on CUDA.
6. Compute image-only and image-plus-instruction retrieval metrics locally.
7. Insert both memory variants into ZeptoDB and search every held-out query.
8. Record Recall@1, Recall@5, MRR, GPU memory, encoder latency, and ZeptoDB
   insert/search p50 and p95.
9. Recover the compact result through `/dev/termination-log`, then delete the
    namespace, NodePool, NodeClaims, node, and EC2 instance.

## Acceptance Criteria

| Criterion | Required |
| --- | ---: |
| All ten tasks represented | pass |
| Ten held-out queries per task | pass |
| Image-plus-instruction ZeptoDB Recall@1 | at least 0.80 |
| Image-plus-instruction ZeptoDB Recall@5 | at least 0.95 |
| Image-plus-instruction ZeptoDB search p95 | below 30 ms |
| CUDA SigLIP encoder produced real embeddings | pass |
| Temporary AWS resources deleted | pass |

## Failure Behavior

Any data, model, CUDA, ZeptoDB, acceptance, or cleanup failure stops the
sequence before Experiment 026. The runner refuses an unexpected AWS account
or EKS endpoint and an existing result path. It deletes only its unique
namespace and NodePools, leaves the shared bench NodePools unchanged, and
publishes the rendered report with an atomic no-clobber step. The harness does
not depend on kubelet log streaming.

## Command

```bash
tests/k8s/run_physical_ai_vision_retrieval.sh
```

## Result

See `docs/research/results/physical_ai_vision_retrieval_025.md`.

- Image-only ZeptoDB Recall@1/Recall@5/MRR: 0.70/0.98/0.810.
- Image-plus-instruction ZeptoDB Recall@1/Recall@5/MRR:
  0.90/1.00/0.926.
- Image-plus-instruction ZeptoDB search p50/p95: 1.032/1.361 ms.
- SigLIP encoder time: 1.478 ms per image on NVIDIA L40S.
- Peak allocated GPU memory: 539.2 MiB.
- All acceptance criteria passed.
- Both temporary EC2 instances terminated; namespace, NodePools, NodeClaims,
  and nodes were deleted; shared bench NodePools remained at CPU limit 0.

## Interpretation

Real image embeddings are viable on the EKS GPU path, and adding the task
instruction improved ZeptoDB Recall@1 by 0.20 and MRR by 0.115 over image-only
retrieval. ZeptoDB p95 search latency remained far below the 30 ms criterion.

The local exact-cosine result was higher than ZeptoDB retrieval, especially for
image-only Recall@1 (0.97 versus 0.70). The bounded ANN candidate path therefore
trades some quality for speed under this configuration and should remain
explicit in later comparisons.

This experiment does not measure VLA action generation, simulator task success,
tokens, or end-to-end decision latency.

## Next Research Step

Experiment 026 may now add a real VLA policy model and measure action-generation
latency and task quality with and without retrieved memory context.
