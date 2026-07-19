# Physical AI VLA Calibration Failure Root-Cause Synthesis

Date: 2026-07-18
Classification: Research-only
Scope: Experiments 032-034

## Executive Finding

The 48/48 calibration failures are a structural candidate-density result, not
a threshold-search or ZeptoDB search-latency result. Experiment 034 preserves
the Experiment 032 failure conclusion while correcting one inherited source
attribution error: suite task 5 maps to manifest task 9, and the missing
executable partition is `open_hold`, not task-5 closed-hold.

No result establishes a risk-free action, physical safety, action correctness,
or production routing readiness. All measurements are shadow-only reference
agreement over two serially correlated episodes.

## Evidence Chain

- Experiment 032 report:
  [`physical_ai_vla_calibration_failure_attribution_032.md`](physical_ai_vla_calibration_failure_attribution_032.md).
- Experiment 033 preserved candidate rows: 127 rows, SHA-256
  `67994a36b2335a1a46fde4992a5c42bfda6d45758a38c7cd6507ee342bd32561`.
- Experiment 034 corrected report:
  [`physical_ai_vla_veto_separability_034.md`](physical_ai_vla_veto_separability_034.md),
  SHA-256 `97bb3fe6e8ce67a34b9bdf1d050a3f7baefcf998efcdb835b3ca96c13db4ca5a`.
- Experiment 034 schema-2 detail:
  [`physical_ai_vla_veto_separability_candidates_034.json`](physical_ai_vla_veto_separability_candidates_034.json),
  SHA-256 `f41225183345e526503bc24576567a7c461e348f4927486bb6b339ab2c84de56`.
- Experiment 034 canonical compact result:
  [`physical_ai_vla_veto_separability_compact_034.json`](physical_ai_vla_veto_separability_compact_034.json),
  SHA-256 `6320991252857db66ae06f2772a6654813327a09944ac0c39ef37fb1c5be733d`.
- Experiment 033 and 034 candidate row arrays are value-identical. Their
  schema-independent canonical row SHA-256 is
  `466467ef024f62bc815069dfa849838cd3a2ec3408319c022fa88a85b6f4552a`.
- The semantic manifest SHA-256 is unchanged:
  `0624630ce232f33c36dbe20159ce3e88729ab0feffb6b67905f4bd2b180e85ba`.

## What The 48 Failures Mean

The 48 rows are 16 confidence thresholds by three margins over the same two
ordered traces. They are parameter counterfactuals, not 48 independent trials.
They collapse to two distinct outcomes:

| Region | Points | Reuse | Action MAE | Projected latency | Failed gates |
| --- | ---: | ---: | --- | ---: | --- |
| Confidence 0.60-0.78, every margin | 30 | 2/595 (0.34%) | mean 0.050840, p95 0.054028 | -0.5% | reuse floor, latency floor |
| Confidence 0.80-0.90, every margin | 18 | 0/595 | unmeasured | -0.9% | reuse floor, missing mean/p95 evidence, latency floor |

All 48 points failed the 20% reuse floor and the 15% projected-latency floor.
The 18 action-quality flags do not mean measured high error; they mean zero
actions were eligible, so the quality requirement could not be demonstrated.
Every configured margin was inert at a fixed threshold.

## Evidence-Backed Blockers

These blockers overlap and are not causal percentage allocations.

1. **Candidate placement plus cooldown creates a hard ceiling on this frozen
   trace.** Precheck found 127/595 candidates (21.3%), only eight above the 119
   actions required by the 20% floor. Applying the mandatory one-step cooldown
   to their observed positions reduced the optimistic ceiling to 65/595
   (10.9%) before confidence, margin, or veto decisions. Experiment 034 projects
   only 10.1% latency reduction at that optimistic ceiling. This does not claim
   that cooldown has the same ceiling on a different trace.
2. **The configured veto collapses the remaining support.** It matched
   123/127 candidates, leaving four before cooldown and two after cooldown.
   Fixed veto plus cooldown therefore produced 2/595 (0.34%) and -0.5%
   projected latency reduction.
3. **Task coverage is one-sided.** Suite task 0 maps to manifest task 5, whose
   partition contains 9 open-hold, 0 closed-hold, and 10 suppression records.
   Suite task 5 maps to manifest task 9, whose partition contains 0 open-hold,
   8 closed-hold, and 11 suppression records. Suite task 5 observed 68 early
   open-hold steps; 58 were rejected as `no_executable_memory`, and the task
   produced zero candidates.
4. **Threshold and margin tuning cannot repair the funnel.** Thirty grid rows
   shared the same two accepted actions and 18 shared zero. Another sweep over
   the frozen midpoint bank would repeat the same structural limit.
5. **ZeptoDB search is not the binding latency constraint.** Experiment 032
   measured 7.741 ms combined search p95 against a 440.576 ms mean VLA call.
   Experiment 034 measured 7.064 ms search p95 against 445.808 ms VLA mean.
   Too few VLA calls can be skipped to amortize encoder and retrieval cost.

## Veto Separability Is Confounded

At the configured 0.01 veto margin, four candidates were separated and 123
were vetoed. The separated group had 0/4 errors above normalized MAE 0.15; the
vetoed group had 26/123 (21.1%). The group-size gate requires at least 20 per
group, so this remains `underpowered`.

The detailed rows show stronger source/time concentration than a veto effect:

- All four separated rows are adjacent steps 173, 174, 177, and 178 and use
  positive source episode 363.
- Positive source episodes 336, 199, and 363 contribute 75/37/15 rows and
  15/11/0 high-error rows.
- Negative source episode 181 contributes 121/123 vetoes and all 26 high-error
  vetoed rows; episode 340 contributes the other two vetoes.
- Every vetoed row has translation-limit mask 4 for its negative top neighbor.
  Across all 127 rows, mask 4 occurs 125 times, command-mismatch mask 2 once,
  and combined mask 10 once; the latter two are separated rows.
- The only nearby same-negative-source slice uses episode 340 at steps 177-180:
  two separated rows followed by two vetoed rows. Four contiguous observations
  are insufficient for a source-controlled effect estimate.
- Among high-error vetoed rows, the largest mean normalized dimension errors
  are action dimensions 2 (0.299753), 4 (0.250821), and 6 (0.194914). The
  disagreement is not isolated to one scalar action component.

The 21.1-point rate delta is therefore a descriptive association with source
episode and contiguous trace location. It cannot justify weakening, removing,
or promoting the veto.

## Decision For Experiment 035

Do not run another threshold/margin grid on the midpoint bank and do not execute
retrieved actions. Pre-register a new trajectory-window memory experiment with:

1. deterministic contact-free source windows, dwell/stride rules, semantic
   source-phase labels, and explicit suite-task-5/manifest-task-9 open-hold
   coverage;
2. a frozen paired control comparing the current midpoint bank with the rebuilt
   bank on identical query episodes and model revisions;
3. per-task and per-hold source density, unique-source counts, maximum source
   share, and leave-one-source-out or blocked-by-source diagnostics;
4. a pre-VLA structural abort unless candidate-plus-cooldown can reach both the
   20% reuse floor and 15% projected-latency floor, pooled and per task;
5. at least 20 source-diverse separated and 20 source-diverse vetoed candidates
   before any separability comparison;
6. shadow-only execution, 100% pinned-VLA action accounting, immutable detail
   evidence, and complete temporary-resource cleanup.

Task-balanced multi-seed held-out evaluation, stale/corrupt/OOD stress, and
direct action reuse remain later experiments gated on Experiment 035 clearing
these structural and source-diversity checks.

## Experiment 035 Outcome Update

Experiment 035 validated the proposed pre-VLA gates and stopped before source
window extraction, model loading, or EKS. The pinned base source contains no
authoritative historical contact or semantic subtask field. A separate pinned
500-episode source adds `subtask_index`, but it is not annotation-aligned to the
frozen 379-episode source and still has no contact telemetry. No historical
simulator replay mapping was provided or validated by the run.

The structural abort is independent of the manifest-task-9 open-hold gap.
Suite task 0 already produced candidates for every one of its 127
precheck-eligible observations and had no missing executable memory. One-step
cooldown leaves 65/377 (17.24%), below the required 76/377. Rebuilding source
memory cannot change that task ceiling under the frozen Experiment 034
precheck mask without changing workload, precheck, cooldown, or policy scope.

See [`physical_ai_vla_trajectory_window_preflight_035.md`](physical_ai_vla_trajectory_window_preflight_035.md).
The next diagnostic should use pinned direct-VLA actions to measure simulator
query eligibility and cooldown placement across multiple frozen seeds before
any bank encoding or retrieval.
