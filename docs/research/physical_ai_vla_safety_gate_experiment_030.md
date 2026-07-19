# Experiment 030: Physical AI VLA Confidence-Safety Dual Gate On EKS

Date: 2026-07-17
Status: Research complete - shadow preflight failed
Classification: Research-only

## Goal

Test whether confidence and an independent, VLA-free safety index can identify
a low-risk historical-action slice that preserves paired LIBERO task quality
while reducing VLA calls, corrected decision latency, and GPU work.

## Hypothesis

On LIBERO-10 tasks 0 and 5, a task-partitioned confidence gate followed by a
conservative safety gate will execute historical actions only in low-risk
states, sustain at least 20% actual skips, reduce corrected mean decision
latency by at least 15%, and introduce no paired task regression or proxy
hazard.

## Scope

- Pinned SmolVLA, SigLIP, LeRobot, LIBERO, and 190 task-partitioned memories
  from Experiments 026-029.
- Tasks 0 and 5 with deterministic initial states and seed base 28.
- Confidence threshold 0.76 and minimum top-1/top-2 margin 0.01.
- At most one historical action before a mandatory VLA cooldown step.
- SmolVLA sampling seeded by absolute task and control step in both direct and
  routed paths.
- Safety inputs available without running VLA:
  - Simulator robot contact count as a proxy for physical contact sensing.
  - Current gripper width and candidate command direction.
  - Top-five historical-action gripper consensus.
  - Candidate translation and rotation magnitude.
  - Top-five historical-action disagreement.
  - Current state distance from the memory distribution.

This is not a physical-robot safety certification. Contact and state limits are
bounded simulator proxies.

## Safety Policy

Confidence and safety are not combined into one scalar.

1. Low confidence or margin rejects historical reuse.
2. Any robot contact, gripper transition, or gripper-candidate disagreement is
   high risk and rejects reuse.
3. Translation above 0.75, rotation above 0.15, normalized neighbor
   disagreement above 0.30, or state z-score above 3.0 is medium risk and
   rejects reuse.
4. Only high-confidence, low-risk observations with no skip on the previous
   step may execute a historical action.
5. Every rejection falls back to SmolVLA.

The gripper hold/transition boundary is derived from the memory bank's open and
closed state distributions. VLA output is used only for shadow evaluation, not
as a runtime safety-gate input.

## Procedure

1. Refresh the deterministic LIBERO sample manifest before AWS creation.
2. Start temporary `c7i.xlarge` and `g6e.xlarge` EKS Auto Mode NodePools.
3. Load pinned models and insert memories into task-specific ZeptoDB
   namespaces.
4. Run direct VLA shadow episodes with retrieval and safety scoring.
5. Stop before routed execution unless at least 20% of shadow decisions are
   confidence-eligible and low risk and their candidate-to-VLA action MAE p95
   is at most 0.50.
6. If preflight passes, run paired dual-gated episodes from the same task seeds.
7. Seed every VLA call by absolute task/control step so skips cannot shift the
   fallback random stream.
8. Stop the decision timer before `env.step()`.
9. Record risk distribution, fallback reasons, actual skips, simulator
   success, paired regressions, contact/state proxy hazards, VLA calls,
   latency, GPU time per decision, and ZeptoDB search latency.
10. Recover compact output through `/dev/termination-log` and delete all
    temporary resources.

## Acceptance Criteria

| Criterion | Required |
| --- | ---: |
| Shadow low-risk eligible rate | at least 20% |
| Accepted shadow action MAE p95 | at most 0.50 |
| Routed success delta | no worse than direct |
| Paired regressions | 0 |
| Actual skip rate | at least 20% |
| Corrected mean decision latency reduction | at least 15% |
| High/medium-risk historical adoptions | 0 |
| Post-skip contact/state proxy hazards | 0 |
| ZeptoDB search p95 | below 30 ms |
| Temporary AWS resources deleted | pass |

## Failure Behavior

Manifest, dependency, model, simulator, ZeptoDB, timeout, compact-result, or
cleanup errors stop the run. A failed shadow preflight prevents routed
execution. Any rejected historical action falls back to VLA. Cleanup completes
before failure investigation, and pod logs are not used as the result channel.

## Command

```bash
tests/k8s/run_physical_ai_vla_safety_gate.sh
```

## Result

The direct-VLA shadow run completed 619 observations across tasks 0 and 5.
The safety gate classified 136 observations (22.0%) as low risk and 483 as
high risk; no medium-risk case occurred. Confidence, safety, and cooldown
together admitted only 10 observations (1.6%), below the required 20%.

The admitted candidates were accurate relative to the shadow VLA: normalized
action MAE p95 was 0.0981 against the 0.50 limit. Exact ZeptoDB search p95 was
4.337 ms. The staged runner correctly stopped before historical actions were
executed, so routed task quality, actual latency reduction, and GPU reduction
were not measured.

Temporary Kubernetes resources were absent after cleanup, both temporary EC2
instances terminated, and shared bench NodePool CPU limits returned to zero.
See `docs/research/results/physical_ai_vla_safety_gate_030.md`.

## Interpretation

Separating confidence and safety worked mechanically but exposed an overlap
problem: the current memory representation rarely produces observations that
are simultaneously high-confidence and low-risk. A 1.6% skip slice cannot
provide practical compute savings after SigLIP and ZeptoDB overhead.

The 483 high-risk classifications came from the hard-risk group: robot
contact, gripper hold/transition mismatch, or top-five gripper command
disagreement. This run did not retain counts for each component, so it does not
justify weakening any one guard.

## Next Research Step

Add component-level rejection telemetry and distinguish expected manipulation
contact from collision contact. Validate the gripper transition inference
against simulator task phase and gripper events. Only then test whether a
better-calibrated safety classifier increases the confidence-safe overlap
without admitting hazardous actions.
