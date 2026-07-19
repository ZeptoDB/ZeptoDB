# Experiment 033: Physical AI VLA Negative-Veto Separability

Date: 2026-07-18
Status: Research complete - telemetry pass, task-attribution report invalidated
Classification: Research-only

> Retrospective correction: pre-run references below to an Experiment 034
> trajectory bank and task-5 closed-hold coverage are superseded. Experiment
> 034 is the mapping-correction run; the trajectory bank is Experiment 035 and
> the corrected gap is suite-task-5/manifest-task-9 `open_hold`.

## Goal

Capture exploratory per-candidate evidence about whether the configured
suppression-memory veto is directionally associated with disagreement from the
pinned SmolVLA reference, and identify where the frozen midpoint memory bank
lacks query-phase and hold-state support. The experiment is forced shadow-only
and cannot execute a retrieved action.

## Prior Evidence

Experiment 032 reproduced all preserved Experiment 031 anchors. Of 127
precheck candidates, the configured negative veto matched 123, leaving four
before cooldown and two after it. Candidate-to-VLA normalized MAE was retained
only for the two routed-by-counterfactual candidates in the compact grid
report, which is not enough to decide whether the veto is useful or merely
detects representation collisions.

The four non-veto candidates from Experiment 032 are fewer than the frozen
minimum of 20 separated candidates. Exact replication therefore makes the
separability conclusion `underpowered` before Experiment 033 runs. The value of
this run is candidate-level attribution, error distribution, and coverage
evidence for the next bank design, not a conclusive separability decision.

The frozen source bank contains one midpoint row per demonstration episode. It
has no source phase, subgoal, contact-window, or dwell labels. Experiment 033
therefore measures separability and runtime query-phase coverage only. It does
not claim to validate phase-local source memory. Building deterministic
contact-free trajectory-window memory is a separate Experiment 034.

## Hypotheses

1. The veto is directionally useful only if vetoed candidates have a higher
   candidate-to-VLA high-error rate and higher p95 normalized MAE than separated
   candidates. `high error` is frozen as normalized MAE strictly above 0.15.
2. A separability conclusion requires at least 20 vetoed and 20 separated
   candidates. Experiment 032 makes an `underpowered` replication the expected
   result; retaining this threshold prevents exploratory point estimates from
   being promoted to a conclusion. Candidates with no suppression neighbor
   form a third group and are not relabeled as separated.
3. The midpoint bank will show asymmetric runtime support by task, observed
   hold state, or early/middle/late query phase. Any demanded partition with a
   `no_executable_memory` observation or zero candidates remains a coverage
   blocker for Experiment 034.
4. The candidate-plus-cooldown structural ceiling will remain below either the
   20% reuse floor or the 15% projected-latency floor. This is evaluated before
   any confidence/margin grid; no grid is run in Experiment 033.

These hypotheses use VLA agreement as a descriptive reference proxy. They are
not hypotheses about physical safety or task correctness.

## Frozen Scope

- LIBERO-10 tasks 0 and 5, one calibration episode per task.
- Calibration seed 28 and absolute-step policy seeding from Experiments 031-032.
- The same deterministic 190-record midpoint bank and semantic manifest.
- The same pinned SmolVLA and SigLIP revisions.
- The same precheck, candidate-safety proxy, negative-veto margin 0.01, and
  one-step cooldown configuration.
- High-error threshold 0.15 and minimum comparable-group size 20.
- Runtime query phases are deterministic early/middle/late thirds of the
  configured episode limit. They are not source-memory phase labels.

The Experiment 033 entry point overrides experiment identity, tasks, seeds,
memory counts, model names, route limits, and diagnostic mode after argument
parsing. Operational paths such as the run ID, result path, and Agent Memory
service URL remain runner-supplied.

## Telemetry

The compact result stores aggregate values only; it stores no images, actions,
or embeddings.

- Three-way candidate groups: `no_negative_support`, `separated`, and
  `vetoed`. Missing suppression support is never relabeled as separation.
- Per-group count, high-error count, candidate-to-VLA MAE mean/p95, positive and
  suppression similarity, positive-minus-suppression gap, confidence, positive
  top-two margin, neighbor-action disagreement, and top-source concentration.
- A veto/high-error contingency table and sample-support status. Precision,
  recall, or rate differences are descriptive VLA-reference metrics only.
- Shadow-only veto-margin counterfactuals at 0.000, 0.005, 0.010, and 0.020.
  These reclassify the same candidates and do not simulate route reuse.
- Per-task/query-phase/hold candidate, veto, high-error, MAE, gap, and unique
  top-source counts.
- Per-task/query-phase/observed-hold precheck demand and missing executable
  partition counts.
- Source-memory admission-reason counts derived from the frozen state/action
  rows without changing their assigned partitions.
- Pooled and per-task candidate-plus-cooldown and configured-fixed-gate ceilings
  with projected latency.
- Encoder, positive/suppression search, and VLA timing plus the Experiment 032
  replication fingerprint and code/data/model provenance.

Candidate-level numeric telemetry is written to a separate canonical JSON
artifact and linked to the compact result by row count and SHA-256. Each row
contains task, episode seed, step, query-phase/hold codes, three-way veto state,
neighbor counts/similarities/source episodes, suppression admission mask,
confidence, margins, disagreement, scalar and seven-dimension normalized
candidate-to-VLA error, and configured candidate-safety-proxy reason. It stores
no raw action, image, URL, or embedding. The EKS runner transports this artifact
as wrapped base64 log lines, validates its JSON, count, and SHA before cleanup,
and refuses ConfigMap inputs above 900,000 bytes before creating AWS resources.

All ratios and quantiles retain their sample counts. Nonexistent group metrics
are encoded as standard-JSON `null`, never `NaN` or `Infinity`.

## Procedure

1. Implement and locally test the frozen diagnostic entry point, accounting
   identities, empty-group behavior, exact thresholds, and the 3,900-byte
   Kubernetes termination-message boundary.
2. Confirm no Experiment 033 AWS resources exist, the shared EKS bench
   NodePools have zero CPU limits, and no shared NodeClaim, Helm release, or
   load generator is active. Abort rather than alter another bench run.
3. Refresh the deterministic sample manifest before creating cloud resources.
4. Create uniquely named temporary CPU/GPU NodePools and an isolated Agent
   Memory namespace.
5. Build the unchanged risk-partitioned midpoint bank while recording admission
   reasons.
6. Run tasks 0 and 5 once in shadow mode. Every simulator action remains the
   pinned SmolVLA action.
7. Attribute every candidate into exactly one veto group and every trace into
   task/query-phase/observed-hold support aggregates.
8. Stop after diagnostics. Do not search a threshold grid, run held-out
   episodes, or enter routed execution.
9. Recover compact JSON from `/dev/termination-log`, delete all owned temporary
   namespace/NodePool/NodeClaim/EC2 resources, verify the shared NodePools
   remained at zero without modifying shared workloads, and then render the
   report.

## Diagnostic Acceptance Criteria

Experiment 033 passes when diagnostic collection is complete, regardless of
whether the veto is separable or any structural preflight is viable.

| Criterion | Required |
| --- | ---: |
| Candidate groups | mutually exclusive and sum to all candidates |
| Veto counterfactuals | conserve candidates and are monotonic by margin |
| High-error contingency | sums to all candidates with finite MAE |
| Query-phase/hold attribution | sums to the relevant trace and candidate counts |
| Per-task structural ceilings | internally consistent |
| Candidate detail artifact | canonical JSON count and SHA match compact result |
| Retrieved actions executed | 0 |
| Compact result | finite standard JSON, at most 3,900 bytes |
| Temporary AWS resources deleted | pass |

Replication-anchor mismatch limits comparison with Experiment 032 but does not
turn otherwise complete Experiment 033 telemetry into a diagnostic failure.

## Interpretation Rules

- If either comparable veto group has fewer than 20 candidates, report
  `underpowered`; do not claim separation or non-separation.
- With sufficient samples, call the direction `supported` only when vetoed
  high-error rate is at least 10 percentage points above separated and vetoed
  p95 MAE is not lower. Call it `not supported` when the rate difference is at
  most zero; otherwise call it `mixed`.
- Never use `no_negative_support` as evidence that a candidate was safely
  separated.
- Candidate-to-VLA agreement, simulator contact, state bounds, and gripper
  checks are proxies. None certifies an action as risk-free.
- The two episodes are serially correlated and task-pooled aggregates are not
  independent trials or evidence of multi-seed generalization.

## Failure Behavior And Disable Plan

Dependency, model, manifest, simulator, ZeptoDB, accounting, nonfinite-value,
payload-size, timeout, or cleanup errors fail the diagnostic. The dedicated
entry point forces diagnostic-only execution and returns after the calibration
shadow. Interrupting the runner invokes owned-resource cleanup and verifies
that the pre-existing shared bench remains idle before any failure
investigation.

## Next Research Step

- Retrospective correction: the pre-run references below to an Experiment 034
  trajectory bank and task-5 `closed_hold` coverage were superseded after the
  suite-to-manifest mapping defect was found. Experiment 034 is the immutable
  mapping-correction run. The trajectory-window bank moves to Experiment 035,
  and the actual missing partition is suite task 5 to manifest task 9
  `open_hold`.
- Underpowered or non-separable veto evidence: keep the veto unchanged and
  improve representation and suppression labels before another route test.
- Missing query-phase/hold support or a failed structural ceiling: build
  Experiment 035 with deterministic contact-free trajectory windows, suite
  task 5 / manifest task 9 `open_hold`, dwell/stride/minimum-neighbor rules, and a paired frozen-bank
  control.
- Only after a rebuilt bank clears per-task density, projected latency, and
  VLA-reference quality should a task-balanced multi-seed held-out shadow be
  specified. Routed execution remains a later experiment.

## Command

```bash
tests/k8s/run_physical_ai_vla_veto_separability.sh
```

This live entry point is now archived and refuses execution. The command is
retained only as the historical invocation; schema-1 evidence remains
renderable.

## Result

The EKS run completed 595 shadow steps and all diagnostic accounting checks,
with 595 VLA calls, zero retrieved actions, 127 candidates, and 123 configured
veto matches. The result was directionally `underpowered`: the four separated
candidates had zero high-error observations, while 26/123 vetoed candidates
exceeded normalized MAE 0.15.

Post-run source-episode inspection found an inherited reporting defect. LIBERO
suite task IDs and manifest task indexes use different orders. Runtime search
already resolved partitions by exact task text, but the compact source
availability/admission table incorrectly used suite IDs 0 and 5 as manifest
indexes. The actual mappings are suite 0 to manifest 5 and suite 5 to manifest
9. The run payload and candidate artifact are preserved, with invalidation
labels on both report paths and a SHA-linked compact payload. Experiment 034
is pre-registered as the mapping-corrected shadow replication; no scientific
task-coverage conclusion is promoted from Experiment 033.

Preserved evidence:

- Invalidated canonical report:
  [`physical_ai_vla_veto_separability_033.md`](results/physical_ai_vla_veto_separability_033.md),
  SHA-256 `4929fa4bb611e021f0b0755c3d1b122529705185122389e28432f934d6fd1748`.
- Candidate rows:
  [`physical_ai_vla_veto_separability_candidates_033.json`](results/physical_ai_vla_veto_separability_candidates_033.json),
  127 rows, SHA-256
  `67994a36b2335a1a46fde4992a5c42bfda6d45758a38c7cd6507ee342bd32561`.
- Invalidated report copy:
  [`physical_ai_vla_veto_separability_033_attempt1_invalidated.md`](results/physical_ai_vla_veto_separability_033_attempt1_invalidated.md).
- Byte-identical invalidated candidate copy:
  [`physical_ai_vla_veto_separability_candidates_033_attempt1_invalidated.json`](results/physical_ai_vla_veto_separability_candidates_033_attempt1_invalidated.json).
- Raw compact payload:
  [`physical_ai_vla_veto_separability_compact_033_attempt1_invalidated.json`](results/physical_ai_vla_veto_separability_compact_033_attempt1_invalidated.json),
  SHA-256 `6e9479dd32ada5b1c5b7ff6ed49d556466f62b9d329a268f008e19d82598ad05`.
