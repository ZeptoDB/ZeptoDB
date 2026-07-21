# Physical AI VLA Closed Loop 027 Results

Generated at: 2026-07-17T08:57:14Z
Classification: Research-only
Suite: `libero_10`
VLA: `HuggingFaceVLA/smolvla_libero` at `6721902bc4d61e50a3bfdb11dfb4cb626f05d102`
Retrieval encoder: `google/siglip-base-patch16-224`

## Scope

Direct VLA and the ZeptoDB-routed policy ran from paired LIBERO initial
states. Success is reported by the real simulator. This bounded run uses
one initial state per task and is not a production safety claim.

| Tasks | Init states/task | Max steps | Memories | Threshold | Guard |
| ---: | ---: | ---: | ---: | ---: | --- |
| 10 | 1 | 520 | 190 | 0.890841 | 2 high-confidence, max 4 skips |

## Closed-Loop Quality

| Path | Successes | Success rate | Steps |
| --- | ---: | ---: | ---: |
| Direct VLA | 5/10 | 50.0% | 4291 |
| ZeptoDB routed | 5/10 | 50.0% | 4286 |

Paired regressions: 0; paired improvements: 0.

## Compute And Latency

| Path | VLA calls | Skips | Mean decision ms | p95 ms | GPU total ms | Energy J |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Direct VLA | 4291 | 0 | 454.169 | 469.241 | 1877964.838 | 200326.4 |
| ZeptoDB routed | 4286 | 0 | 469.756 | 482.133 | 1927346.806 | 204915.6 |

- VLA call reduction: 0.1%.
- Fallback rate: 100.0%.
- Mean decision-latency reduction: -3.4%.
- Online GPU-time reduction: -2.6%.
- ZeptoDB search p50/p95: 2.425/2.671 ms.

## Per-Task Result

| Task | Direct success | Routed success | Direct steps | Routed steps | Routed VLA | Skips |
| ---: | --- | --- | ---: | ---: | ---: | ---: |
| 0 | yes | yes | 451 | 451 | 451 | 0 |
| 1 | yes | yes | 273 | 273 | 273 | 0 |
| 2 | yes | yes | 250 | 245 | 245 | 0 |
| 3 | yes | yes | 507 | 507 | 507 | 0 |
| 4 | no | no | 520 | 520 | 520 | 0 |
| 5 | yes | yes | 210 | 210 | 210 | 0 |
| 6 | no | no | 520 | 520 | 520 | 0 |
| 7 | no | no | 520 | 520 | 520 | 0 |
| 8 | no | no | 520 | 520 | 520 | 0 |
| 9 | no | no | 520 | 520 | 520 | 0 |

## Acceptance

| Criterion | Status |
| --- | --- |
| Direct VLA succeeds on at least one task | pass |
| Fallback rate is between 5% and 95% | fail |
| Mean decision latency reduced by at least 15% | fail |
| At most one paired task regression | pass |
| Real LIBERO closed-loop steps executed | pass |
| Routed success rate no more than 10pp below direct VLA | pass |
| VLA calls reduced by at least 30% | fail |
| ZeptoDB exact-search p95 below 30 ms | pass |
| Temporary AWS resources deleted | pass |

## Result

Overall status: fail.

## Interpretation

This run tests real closed-loop behavior for a bounded deterministic slice.
It does not establish confidence intervals, drift robustness, collision
safety, or readiness to control a physical robot.
