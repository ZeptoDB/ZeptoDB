# Experiment 031: Physical AI Risk-Partitioned Free-Space Router On EKS

Date: 2026-07-18
Status: Research complete - calibration gate failed
Classification: Research-only

## Goal

Test whether Agent Memory can reduce real VLA latency and GPU work when direct
historical-action reuse is restricted to contact-free, stable-gripper states,
while non-executable memories can only veto reuse.

## Hypothesis

On LIBERO-10 tasks 0 and 5, hard-partitioning memories by task and gripper hold
state, applying a cheap safety precheck before retrieval, and consulting a
separate suppression partition will sustain at least 20% free-space action
reuse and reduce corrected decision latency and GPU time by at least 15%
without a paired task regression or post-reuse proxy hazard.

## Scope

- Pinned SmolVLA, SigLIP, LeRobot, and LIBERO versions from Experiments 026-030.
- The existing 190 midpoint demonstration memories.
- Calibration episodes use seed 28; held-out shadow and paired routed episodes
  use seed 1028.
- Initial execution is limited to tasks 0 and 5.
- Direct memory actions are limited to at most one step before a mandatory VLA
  cooldown.
- Contact, gripper stability, state distance, action bounds, and memory
  partitions are simulator proxies.

The source demonstration records do not contain per-step contact force or
post-action hazard labels. Their admission as bounded candidates is not proof
that each source action is safe. The held-out shadow and closed-loop gates are
research safeguards, not physical-robot certification.

## Risk-Aware Memory Policy

Memory confidence and safety remain separate.

1. Each source memory is assigned to `open_hold`, `closed_hold`, or
   `suppression`.
2. A memory enters a hold partition only when its gripper command agrees with
   its observed width, its width is outside the ambiguous guard band, and its
   translation and rotation are bounded.
3. All other source memories enter the suppression partition and cannot
   produce an executable action.
4. Task and memory class are hard ZeptoDB namespace/type filters.
   `metadata_json` is retained for audit but is not trusted as a search filter.
5. Runtime retrieval is attempted only when there is no robot contact, gripper
   width is stable, width is outside the ambiguous band, state z-score is
   bounded, and the matching executable partition is non-empty.
6. A close suppression match vetoes action reuse even when the positive
   partition has a high-confidence candidate.
7. Every rejection falls back to SmolVLA.

## Compute Policy

- Contact, transition, ambiguous, outlier, and missing-partition states bypass
  SigLIP and ZeptoDB and run SmolVLA directly.
- Only free-space hold states pay retrieval cost.
- A free-space candidate skips SmolVLA only after confidence, margin, action
  safety, negative-veto, and cooldown checks pass.
- VLA feature caching is not implemented in this experiment. Contact and
  manipulation phases remain VLA-only and become the input to a later
  feature-cache experiment.

## Procedure

1. Refresh the deterministic sample manifest before creating AWS resources.
2. Start temporary `c7i.xlarge` and `g6e.xlarge` EKS Auto Mode NodePools.
3. Build task/gripper executable partitions and task-specific suppression
   partitions in ZeptoDB.
4. Run calibration shadow episodes and search frozen confidence/margin
   thresholds.
5. Stop unless calibration provides 20-35% projected reuse, action MAE mean at
   most 0.10, action MAE p95 at most 0.15, and at least 15% projected latency
   reduction.
6. Run held-out shadow episodes with seed 1028 and stop unless the frozen
   region again reaches 20% reuse, 0.15 action MAE p95, and 15% projected
   latency reduction.
7. Only after both shadow gates pass, run paired routed episodes from the same
   held-out seeds and absolute-step SmolVLA random streams.
8. Stop the decision timer before `env.step()`.
9. Check robot contact immediately after every reused action and state
   outliers at the next observation.
10. Recover compact output through `/dev/termination-log`, delete temporary
    resources, and return shared bench NodePools to zero CPU.

## Acceptance Criteria

| Criterion | Required |
| --- | ---: |
| Held-out shadow reuse rate | at least 20% |
| Held-out action MAE p95 | at most 0.15 |
| Held-out projected latency reduction | at least 15% |
| Routed success delta | no worse than direct |
| Paired regressions | 0 |
| Actual free-space reuse rate | at least 20% |
| Corrected mean decision latency reduction | at least 15% |
| GPU-time reduction per decision | at least 15% |
| Post-reuse contact/state proxy hazards | 0 |
| ZeptoDB search p95 | below 30 ms |
| Temporary AWS resources deleted | pass |

## Failure Behavior

Missing or malformed memories, empty executable partitions, dependency/model
errors, simulator errors, ZeptoDB errors, threshold failure, held-out shadow
failure, timeout, compact-result overflow, or cleanup failure stop the run.
Routed execution cannot start after either shadow gate fails. Any ambiguous
runtime state or memory decision fails closed to SmolVLA.

## Command

```bash
tests/k8s/run_physical_ai_vla_risk_router.sh
```

## Result

The EKS calibration shadow completed 595 observations across tasks 0 and 5.
No region in the 16 confidence thresholds by three margin values passed the
calibration gate, so the harness stopped at
`no_viable_free_space_region` before held-out or routed execution.

The runtime precheck admitted 127/595 observations (21.34%). It rejected 390
for finger contact, 58 for a missing executable memory partition, 12 for
gripper movement, 6 for state outliers, and 2 as first observations. ZeptoDB
combined search p95 was 6.871 ms. The compact result did not retain individual
metrics for the 48 grid points, so it cannot attribute their exact gate
failures.

The experiment namespace and temporary NodePools were deleted, both temporary
EC2 instances terminated, and the shared bench NodePools returned to zero CPU.

## Interpretation

The 20% reuse floor required at least 119 actions, while only 127 observations
passed precheck. The remaining confidence, margin, candidate-safety,
negative-veto, and cooldown checks could therefore reject at most eight
eligible observations, a structurally narrow operating window. Task 5 also had
no `closed_hold` source memory, which explains its missing-partition gap.

These are descriptive simulator findings. Because no memory action was routed,
Experiment 031 supplies no closed-loop action-reuse evidence and no physical
safety evidence.

## Next Research Step

Experiment 032 completed the planned shadow-only instrumented replication. It
matched all five preserved Experiment 031 anchors and showed that cooldown
alone caps reuse at 65/595 (10.9%), while the configured negative veto leaves
only four candidates before cooldown and two after it. All candidates came
from task 0 `open_hold`; task 5 lacks `closed_hold` memory. The next experiment
must diagnose veto separability and rebuild phase-local contact-free trajectory
memory before another threshold grid. Safety gates and routed execution remain
unchanged.
