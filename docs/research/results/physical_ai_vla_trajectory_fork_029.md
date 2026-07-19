# Physical AI VLA Trajectory Fork 029 Results

Generated at: 2026-07-17T17:42:28Z
Classification: Research-only
Status: fail

## Causal Result

Paired counterfactual forks: 14 across 2 tasks and 3 trajectory phases.

| Metric | Result |
| --- | ---: |
| Historical-to-VLA action MAE | 0.1415 |
| Post-action normalized state drift p50 | 0.0113 |
| Post-action front-pixel MAE p50 | 0.00413 |
| Branch minus control confidence | -0.0002 |
| Control eligibility | 94.6% |
| Historical branch eligibility | 94.6% |
| Eligibility drop | 0.0% |
| Action-error/state-drift correlation | 0.960 |

## By Region

| Region | Forks | Action MAE | State drift | Pixel MAE | Confidence delta | Control eligible | Branch eligible |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| broad | 5 | 0.1500 | 0.0135 | 0.00488 | -0.0001 | 100.0% | 100.0% |
| middle | 5 | 0.1500 | 0.0135 | 0.00488 | -0.0001 | 100.0% | 100.0% |
| selected | 4 | 0.1201 | 0.0101 | 0.00484 | -0.0004 | 81.2% | 81.2% |

## Acceptance

| Check | Status |
| --- | --- |
| coverage | pass |
| dose response | pass |
| eligibility effect | fail |
| restore exact | pass |
| search p95 | pass |
| trajectory shift | pass |
| aws resources deleted | pass |

ZeptoDB search p50/p95: 5.139/5.486 ms.

## Interpretation

The fork begins from an identical restored MuJoCo state. One branch uses
the direct VLA action and the other uses one retrieved historical action;
both then use deterministically paired VLA actions for up to four steps.
This isolates short-horizon trajectory shift but is not physical-robot or
multi-seed production evidence.

The tested cause is not supported. A single historical action did move the
trajectory: normalized state drift p50 was 0.0113, front-pixel MAE p50 was
0.00413, and action error strongly correlated with state drift (`r=0.960`).
However, the shift did not reduce retrieval eligibility. Control and
historical branches were both eligible on 94.6% of post-action observations,
and mean branch-minus-control confidence was only -0.0002.

The Experiment 028 skip-rate collapse therefore cannot be attributed to the
immediate four-step confidence effect of one historical action in these
forks. Remaining candidates include two consecutive historical actions,
sampling bias toward stable first-eligible pockets, and stochastic SmolVLA RNG
stream misalignment: the routed path did not consume a policy sample on a
skip, so its next fallback did not use the baseline step's random stream.

Temporary `c7i.xlarge` and `g6e.xlarge` instances reached `terminated`; the
experiment namespace, NodePools, and NodeClaims were absent; shared bench
NodePool CPU limits were zero.
