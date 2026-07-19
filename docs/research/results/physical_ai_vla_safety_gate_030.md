# Physical AI VLA Confidence-Safety Gate 030 Results

Generated at: 2026-07-17T19:47:04Z
Classification: Research-only
Status: fail

## Result

Stopped at `shadow_safety_preflight_failed` before routed execution.
Shadow low-risk eligible rate: 1.6%.
Accepted candidate action MAE p95: 0.0981.
ZeptoDB search p95: 4.337 ms.

Risk counts: low 136, medium 0, high 483.

## Acceptance

| Criterion | Status |
| --- | --- |
| Shadow low-risk, high-confidence rate at least 20% | fail, 1.6% |
| Accepted shadow action MAE p95 at most 0.50 | pass, 0.0981 |
| ZeptoDB exact-search p95 below 30 ms | pass, 4.337 ms |
| Routed success and regression checks | not run |
| Actual latency and GPU reduction | not run |
| Temporary AWS resources deleted | pass |

## Interpretation

The independent safety gate was selective and the few accepted candidates were
close to the VLA action, but confidence and low risk rarely overlapped. Safety
alone classified 136/619 observations (22.0%) as low risk; after applying the
confidence/margin gate and mandatory cooldown, only 10/619 observations (1.6%)
were eligible. That density cannot offset retrieval overhead or meet the
compute-saving target.

All 483 rejected safety observations were hard-risk cases involving at least
one of robot contact, a gripper hold/transition mismatch, or top-five gripper
command disagreement. No medium-risk motion, neighbor-disagreement, or state
outlier case occurred. This run did not retain component-level hard-risk
counts, so those three causes must be separated before changing the policy.

No historical action was executed in routed mode, so this result is a
preflight rejection rather than closed-loop safety evidence. The conservative
gate must not be loosened solely to increase skips.

Temporary `c7i.xlarge` and `g6e.xlarge` instances reached `terminated`; the
experiment namespace, NodePools, and NodeClaims were absent; shared bench
NodePool CPU limits were zero.
