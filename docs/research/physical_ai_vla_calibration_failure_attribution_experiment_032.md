# Experiment 032: Physical AI VLA Calibration Failure Attribution On EKS

Date: 2026-07-18
Status: Research complete - diagnostic passed, calibration non-viable
Classification: Research-only

## Goal

Run an instrumented replication of the Experiment 031 calibration setup and
record the per-point evidence its first compact result omitted. This is a
shadow-only diagnostic experiment. It must not execute a retrieved action even
if an instrumented calibration point unexpectedly becomes viable.

## Prior Evidence

Experiment 031 evaluated 16 confidence thresholds from 0.60 through 0.90 and
three similarity margins, for 48 counterfactual regions over the same ordered
595-step calibration trace. It stopped at `no_viable_free_space_region` before
routed execution. The preserved result contains aggregate precheck counts and
ZeptoDB search p95, but not the per-region reuse, action-error, latency, or gate
failure metrics needed for exact attribution. Because the original per-step
trace was discarded, Experiment 032 cannot recover it. Attribution applies
directly to Experiment 031 only if the new run matches its preserved aggregate
funnel, partition availability, semantic manifest identity, and VLA revision;
otherwise it applies only to the new trace.

The original Experiment 031 result is immutable. Experiment 032 uses a new
experiment identity, tenant scope, runner, and result artifact because it
collects new EKS observations with modified instrumentation.

## Questions

1. How many of the 48 points fail each configured gate: reuse below 20%, reuse
   above 35%, mean normalized action MAE above 0.10, p95 MAE above 0.15, or
   projected latency reduction below 15%?
2. Does precheck density plus the mandatory one-step cooldown make the reuse
   floor impossible before confidence and margin are considered?
3. How much additional density is removed by the configured candidate-safety
   and negative-veto checks, separately and together?
4. Does one task or gripper-hold partition dominate the closest region's action
   error or reuse density?
5. Is measured query encoding or ZeptoDB search cost large enough to explain a
   projected latency failure?

## Hypothesis

The Experiment 031 failure is primarily a coverage-density constraint rather
than a ZeptoDB search-latency constraint: precheck plus the mandatory cooldown
will leave little or no room above the 20% reuse floor, task 5's missing
`closed_hold` partition will account for a material hard coverage gap, and
confidence/margin regions that improve action error will reduce reuse below
the reuse and projected-latency gates. This is a falsifiable descriptive
hypothesis over one calibration trace, not a causal or safety claim.

## Scope

- LIBERO-10 tasks 0 and 5, one calibration episode per task.
- Calibration seed 28 and the same absolute-step SmolVLA policy seeding used by
  Experiment 031.
- The same 190 midpoint demonstration records and risk-partition admission
  rules as Experiment 031.
- The same pinned SmolVLA revision and model/runtime dependencies.
- The default 16 x 3 threshold/margin grid, producing exactly 48 points.
- Temporary EKS `c7i.xlarge` and `g6e.xlarge` nodes in `ap-northeast-2`.

All 48 points reuse the same two ordered shadow traces. They are parameter
counterfactuals, not independent trials or 48 independent robot episodes.
Pooled metrics are weighted by episode length and cannot establish that either
task passes individually.

## Instrumentation

The compact result records the following without storing images, actions, or
embeddings:

- Every grid point's threshold, margin, reuse count, mean and p95 normalized
  action MAE, projected latency reduction, and five-bit failure mask.
- Overlapping gate counts, mask-signature counts, and zero-reuse point count.
- Candidate, cooldown-only, safety-only, veto-only, combined fixed-gate, and
  combined fixed-gate-plus-cooldown reuse ceilings.
- Projected latency at the candidate-plus-cooldown and configured
  safety-plus-veto-plus-cooldown ceilings.
- Independent unsafe and negative-veto counts, explicitly allowing overlap.
- The closest heuristic point's first-failing route reasons and task/hold
  reuse, mean MAE, p95 MAE, and task-level projected latency.
- Per-task precheck funnels, scoped memory-partition availability, policy,
  encoder, positive/suppression search timing, run ID, code/data hashes, and
  model provenance.

Gate decisions use full-precision values. Compact metrics use a `1e6` scale so
the report preserves six decimal places; the stored failure mask remains the
authoritative boundary decision.

## Procedure

1. Run focused local unit tests, Python compile checks, shell syntax checks,
   and the compact-payload boundary test.
2. Confirm the shared EKS bench is asleep and no prior VLA namespace,
   NodePool, NodeClaim, instance, or runner process remains.
3. Refresh the deterministic sample manifest before creating AWS resources.
4. Start temporary CPU and GPU NodePools and a scoped ZeptoDB Agent Memory pod.
5. Build the same task/hold/suppression memory partitions as Experiment 031.
6. Run only calibration shadow episodes for tasks 0 and 5.
7. Evaluate and compact all 48 counterfactual regions. The Experiment 032
   entry point forces diagnostic-only mode regardless of calibration viability.
8. Recover the standard JSON result through `/dev/termination-log`.
9. Delete the experiment namespace and temporary NodePools, wait for all
   temporary EC2 instances to terminate, and return shared bench NodePools to
   zero CPU before rendering the final report.

## Acceptance Criteria

Experiment 032 passes when diagnostic collection is complete, not when an
Experiment 031 routing region is viable.

| Criterion | Required |
| --- | ---: |
| Grid rows recorded | exactly 48 |
| Gate counts and mask signatures | internally consistent |
| Closest route/task/hold attribution | covers the full ordered trace |
| Retrieved actions executed | 0 |
| Compact JSON | finite standard JSON, at most 3,900 bytes |
| Temporary AWS resources deleted | pass |

`calibration.viable` is a research finding and is not an Experiment 032 pass
criterion.

## Failure Behavior And Disable Plan

Any dependency, model, simulator, ZeptoDB, telemetry-consistency, compact-size,
timeout, or cleanup failure fails the diagnostic. The dedicated entry point
forces `diagnostic_only`; it returns immediately after calibration and has no
path to held-out or routed execution. Stopping the runner triggers cleanup,
and the shared bench is explicitly put to sleep before failure investigation.

## Limits

- Contact, state distance, action bounds, and gripper state remain simulator
  proxies, not physical safety certification.
- Source records lack phase labels, contact forces, and post-action hazard
  labels.
- The closest point is a descriptive heuristic, not an optimum or a frozen
  held-out recommendation.
- Gate frequencies depend on grid density and overlapping criteria; they do
  not identify causal dominance.
- Projected latency includes measured encoder, combined positive/suppression
  search, and VLA fallback time but excludes the cheap precheck/route overhead.

## Interpretation

The hypothesis was supported and narrowed. Candidate coverage and the mandatory
cooldown make the 20% reuse floor impossible before confidence or margin is
considered: 127/595 observations passed precheck, but at most 65/595 (10.9%)
could be reused after the one-step cooldown. Even this optimistic ceiling
projects only 10.1% latency reduction, below the 15% requirement.

The configured negative-veto check is the next binding uncertainty. It matched
123/127 candidates (96.9%), leaving four before cooldown and two after it. The
independent configured candidate-safety proxy rejected none. This does not justify
loosening the veto: action quality was measured for only the two accepted
actions, with mean/p95 normalized MAE 0.050840/0.054028. The next experiment
must determine whether the veto distinguishes unsafe or transition-like
memories from executable support, or instead reflects positive/suppression
representation collisions.

The 48-point grid supplied only two distinct outcome rows. Thresholds 0.60
through 0.78 reused the same two actions; thresholds 0.80 through 0.90 reused
none. Margins 0.000, 0.005, and 0.010 changed no outcome. More threshold or
margin sweeps over the same memory are therefore low-value.

Memory coverage is phase-asymmetric. Suite task 0 `open_hold` supplied every
one of the 127 candidates, while suite task 5 supplied none and incurred 58
`no_executable_memory` rejections. Experiments 033-034 later established that
the source table in the immutable Experiment 032 report used suite IDs as
manifest indexes. The corrected gap is suite task 5 -> manifest task 9
`open_hold`, whose executable count is zero; it is not task-5 closed-hold
memory. The existing bank of single midpoint records still does not cover the
observed task/hold phases well enough to support this router.

ZeptoDB search is not the binding latency constraint. Combined search p95 was
7.741 ms, below the 30 ms limit. Mean encoding plus search was 17.762 ms versus
a 440.576 ms mean VLA call. With only two VLA calls skipped, retrieval overhead
made projected latency 0.5% worse; this is a reuse-density result, not a search
performance failure.

All five preserved Experiment 031 anchors matched: step count, aggregate
precheck funnel, memory partitions, semantic manifest, and VLA revision. The
attribution is therefore a strong instrumented replication of that setup, but
not recovery of its discarded per-step trace. All 48 points share one ordered
two-episode trace, so the result is descriptive and cannot establish causal
gate importance, per-task generalization, closed-loop quality, or physical
safety.

## Next Research Step

Run a new, pre-registered shadow-only veto-separability and phase-support
experiment before changing any routing gate:

1. Record positive similarity, suppression similarity, their gap, candidate
   confidence/margin, neighbor disagreement, admission reason, phase/subgoal,
   and candidate-to-VLA error for vetoed and allowed candidates.
2. Rebuild candidate memory from contact-free trajectory windows rather than a
   single midpoint, including suite-task-5/manifest-task-9 `open_hold`,
   phase-local support, dwell length, and a minimum-neighbor requirement.
3. Run a structural preflight before any grid: abort unless per-task candidate
   coverage and the candidate-plus-cooldown ceiling can satisfy the reuse and
   projected-latency floors.
4. Only after that preflight passes, run task-balanced multi-seed held-out
   shadow validation. Routed execution, stale/corrupt/OOD stress, and direct
   action reuse remain blocked until a separately specified held-out gate
   passes.

The 20% reuse target and mandatory cooldown must also be reconciled with the
measured compute economics. A future design may increase contact-free phase
coverage or introduce a separately justified multi-step controller/checkpoint;
it must not simply remove cooldown or weaken the negative veto to force a pass.

## Command

```bash
tests/k8s/run_physical_ai_vla_calibration_failure_attribution.sh
```

## Result

The dedicated EKS diagnostic completed 595 shadow steps under run ID
`4639958745` and recorded all 48 grid points. Diagnostic acceptance passed:
counts were consistent, all 48 rows and route attributions were present, zero
retrieved actions executed, and all temporary AWS resources were deleted.
Calibration remained non-viable.

Every point failed the 20% reuse floor and 15% projected-latency-reduction
floor. Thirty points reused 2/595 actions and failed those two gates. Eighteen
points reused zero actions, so action quality was unmeasured and those points
also failed the two quality-evidence gates. The full report is
[`results/physical_ai_vla_calibration_failure_attribution_032.md`](results/physical_ai_vla_calibration_failure_attribution_032.md).

## Retrospective Task-Identity Correction

The immutable Experiment 032 report remains unchanged. Experiment 034 proved
that its source-memory availability/admission table was keyed with LIBERO suite
IDs 0/5 instead of the queried manifest indexes 5/9. This invalidates only the
source-table axis and the derived task-5 closed-hold explanation. The 595-step
trace, 48 grid rows, 30 two-reuse outcomes, 18 zero-reuse outcomes, cooldown
ceiling, projected-latency failures, and zero-action execution remain valid and
were reproduced through the Experiment 034 candidate-row digest.
