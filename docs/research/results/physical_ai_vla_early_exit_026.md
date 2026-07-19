# Physical AI VLA Early Exit 026 Results

Generated at: 2026-07-17T02:16:53Z
Classification: Research-only
Dataset: `lerobot/libero_10_image`
VLA: `HuggingFaceVLA/smolvla_libero` at `6721902bc4d61e50a3bfdb11dfb4cb626f05d102`
Retrieval encoder: `google/siglip-base-patch16-224`

## Scope

This offline replay invokes the real CUDA VLA and compares it with a
ZeptoDB confidence-gated historical-action early exit. Action MAE against
the recorded expert action is an offline proxy, not simulator task success.

## Runtime

| GPU | VLA parameters | Peak GPU MiB | Memories | Calibration | Evaluation |
| --- | ---: | ---: | ---: | ---: | ---: |
| NVIDIA L40S | 604934176 | 1625.0 | 190 | 50 | 50 |

## Compute And Latency

| Path | VLA calls | Mean decision ms | p95 decision ms | GPU total ms | Energy J |
| --- | ---: | ---: | ---: | ---: | --- |
| Direct VLA | 50 | 475.572 | 491.442 | 23728.155 | 2417.4 |
| ZeptoDB routed | 0 | 10.205 | 10.648 | 365.314 | 60.7 |

- VLA call reduction: 100.0%.
- Total online GPU-time reduction: 98.5%.
- Mean decision-latency reduction: 97.9%.
- Query encoder mean: 7.343 ms.
- ZeptoDB search p50/p95: 2.298/2.565 ms.

## Offline Action Quality

| Path | Normalized action MAE | Allowed routed limit |
| --- | ---: | ---: |
| Direct VLA | 0.107831 | - |
| ZeptoDB routed | 0.099922 | 0.113222 |

## Acceptance

| Criterion | Status |
| --- | --- |
| Routed normalized action MAE within quality limit | pass |
| Total online GPU time reduced by at least 20% | pass |
| Mean decision latency reduced by at least 15% | pass |
| Pinned real SmolVLA loaded and invoked | pass |
| VLA calls reduced by at least 30% | pass |
| ZeptoDB exact-search p95 below 30 ms | pass |
| Temporary AWS resources deleted | pass |

## Result

Overall status: pass.

## Interpretation

The result applies only to the deterministic middle-frame LIBERO replay,
the fixed memory split, and this model revision. It does not establish
closed-loop control quality, safety, or production VLA routing readiness.
