# Physical AI Vision Retrieval 025 Results

Generated at: 2026-07-16T16:07:45Z
Classification: Research-only
Dataset: `lerobot/libero_10_image` at `main`
Encoder: `google/siglip-base-patch16-224`

## Scope

This run uses real LIBERO front-camera frames, a real SigLIP encoder on
an NVIDIA GPU, and the real ZeptoDB Agent Memory HTTP path. It measures
task-level episode retrieval, not VLA action generation or simulator success.

## Data And Runtime

| Tasks | Memories | Held-out queries | Frame | GPU | Embedding dims | Peak GPU MiB | Encoder ms/image |
| ---: | ---: | ---: | --- | --- | ---: | ---: | ---: |
| 10 | 190 | 100 | middle | NVIDIA L40S | 768 | 539.2 | 1.478 |

## Retrieval Quality

| Variant | Path | Recall@1 | Recall@5 | MRR |
| --- | --- | ---: | ---: | ---: |
| image | local cosine | 0.970 | 1.000 | 0.985 |
| image | ZeptoDB | 0.700 | 0.980 | 0.810 |
| image_text | local cosine | 1.000 | 1.000 | 1.000 |
| image_text | ZeptoDB | 0.900 | 1.000 | 0.926 |

## ZeptoDB Latency

| Variant | Insert p50 ms | Insert p95 ms | Search p50 ms | Search p95 ms |
| --- | ---: | ---: | ---: | ---: |
| image | 1.261 | 1.858 | 1.037 | 1.362 |
| image_text | 1.622 | 2.168 | 1.032 | 1.361 |

## Acceptance

| Criterion | Status |
| --- | --- |
| 10 held-out queries per task | pass |
| Real CUDA encoder produced embeddings | pass |
| Image + instruction ZeptoDB Recall@1 >= 0.80 | pass |
| Image + instruction ZeptoDB Recall@5 >= 0.95 | pass |
| All 10 tasks represented | pass |
| Image + instruction ZeptoDB search p95 < 30 ms | pass |
| Temporary AWS resources deleted | pass |

## Result

Overall status: pass.

## Interpretation

A pass proves that real visual embeddings can be created on the EKS GPU
path and retrieved through ZeptoDB within this bounded workload. It does
not prove faster or more accurate VLA policy inference; that remains the
next experiment.
