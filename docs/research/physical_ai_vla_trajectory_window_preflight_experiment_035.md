# Experiment 035: Physical AI VLA Trajectory-Window Preflight

Date: 2026-07-19
Status: Research complete - diagnostic-valid structural abort
Classification: Research-only

## Goal

Determine whether a contact-observable, source-diverse trajectory-window memory
bank is both scientifically constructible and structurally capable of clearing
the frozen VLA router gates before preparing images, loading models, creating
cloud resources, or executing a retrieved action.

The paired comparison, if admitted, uses LIBERO suite task 0 mapped to manifest
task 5 and suite task 5 mapped to manifest task 9. It compares a midpoint
control with trajectory-window treatment on one identical shadow trace.

## Prior Evidence

Experiment 034 corrected task identity and preserved the Experiment 033 trace:
595 steps, 127 candidates, 65 candidates after one-step cooldown, and zero
retrieved actions. Suite task 0 supplied all 127 candidates. Its manifest-task-5
partition already had executable open-hold memory and no
`no_executable_memory` observation. Suite task 5 mapped to manifest task 9 and
had 58 early open-hold observations rejected for missing executable memory.

The current `lerobot/libero_10_image` top-level feature-name set contains
images, robot state, action, time, frame, episode, global index, and task index.
It does not contain authoritative source contact, object state, simulator
state, task predicate, or source init-state identity. The pinned auxiliary
`lerobot/libero_10_image_subtask` feature-name set adds `subtask_index`, but not
source contact telemetry. It is a separate 500-episode corpus rather than an
annotation-aligned view of the frozen 379-episode source; no base-to-auxiliary
episode mapping is provided.

## Hypotheses

1. A source row can be called `contact-free` only when authoritative per-frame
   simulator contact telemetry exists, or when the exact historical source
   state is replayed in a pinned simulator and agrees with the recorded state
   to maximum absolute error at most `1e-4`. Robot width, action magnitude,
   images, or temporal thirds are proxies and cannot satisfy this gate.
2. Under the frozen Experiment 034 precheck/candidate mask, a memory-bank
   change cannot raise suite task 0 above its existing
   candidate-plus-cooldown count because all 127 precheck-eligible observations
   already produced candidates and the task had no missing executable-memory
   observation. Treatment-bank statistics may not recompute or alter this mask.
3. Consequently, suite task 0 is expected to stop the experiment at the
   per-task 20% reuse-ceiling gate: `65 / 377`, below the required
   `ceil(0.20 * 377) = 76` actions.
4. Manifest-task-9 open-hold source coverage may be technically rebuildable,
   but it cannot override an independent frozen-trace failure on task 0.

These are feasibility and accounting hypotheses. They are not hypotheses about
action correctness, task success, risk-free behavior, or physical safety.

## Frozen Inputs

- Experiment 034 canonical compact result SHA-256
  `6320991252857db66ae06f2772a6654813327a09944ac0c39ef37fb1c5be733d`.
- Experiment 034 semantic manifest SHA-256
  `0624630ce232f33c36dbe20159ce3e88729ab0feffb6b67905f4bd2b180e85ba`.
- Experiment 034 candidate detail SHA-256
  `f41225183345e526503bc24576567a7c461e348f4927486bb6b339ab2c84de56`.
- `lerobot/libero_10_image` revision
  `7e324b526699f444044952c82ce3f438e8d300d0`.
- `lerobot/libero_10_image_subtask` revision
  `06fdb000a8d6d3f43c79abb2545a24379265bef8`.
- Reuse-ceiling floor 20% pooled and for every suite task.
- Projected-latency floor 15% pooled and for every suite task.
- Mandatory one-step cooldown; no threshold, margin, veto, or cooldown change.
- The recorded Experiment 034 precheck/candidate mask is frozen for the
  structural bank-only counterfactual. A conditional paired stage must freeze
  the complete-control state means/standard deviations, action scale, gripper
  boundary, and every precheck threshold before treatment-bank evaluation.

## Conditional Trajectory-Window Protocol

This stage is permitted only if source contact/semantic provenance and frozen
structural preflights pass.

- Use five-frame windows `t-2..t+2`.
- Require zero robot, finger, and arm contact at every frame.
- Require one hold class throughout the window, width outside the 0.005 guard
  band, consecutive width delta at most 0.003, a matching hold command, center
  translation norm at most 0.75, and center rotation norm at most 0.15.
- Require one task-specific semantic subtask throughout the window. Temporal
  thirds are recorded only as `proxy_temporal_phase` and cannot replace the
  semantic label.
- Select centers in frame order with stride at least 8 and at most three centers
  per `(source episode, semantic phase, hold class)`.
- Limit the total source bank to 400 rows and the serialized Kubernetes
  ConfigMap object to 900,000 bytes.
- For every executable task/hold partition and each suppression partition,
  require at least 20 records, 10 unique source episodes, and maximum source
  share at most 15%. Manifest task 9 `open_hold` is mandatory.
- Use the same source episodes for midpoint control and treatment, freeze
  normalization statistics from the complete 190-row control, and require zero
  source/query episode overlap.
- Generate the query trace and pinned-VLA reference actions once. Both banks
  must consume the exact same query/trace digest.

## Procedure

1. Validate the Experiment 034 compact result identity, task map, semantic
   manifest digest, detail digest, action accounting, source availability,
   precheck counts, and structural rows.
2. Fetch the two pinned Hugging Face top-level feature-name sets and episode
   metadata with bounded responses and validate their requested revisions.
3. Record authoritative contact, corpus alignment, and semantic-phase feature
   observability. A semantic feature from an unaligned corpus cannot label the
   frozen source. Do not infer source contact from state/action proxies.
4. For each suite task, compare precheck-eligible, candidate, missing-memory,
   candidate-plus-cooldown, required action count, and projected latency.
5. Under the frozen precheck mask, mark a task ceiling bank-independent only
   when every precheck-eligible row already produced a candidate and the task
   had no missing executable-memory observation.
6. Abort before source-image preparation, model loading, EKS, threshold grids,
   or paired bank evaluation when any provenance or structural gate fails.
7. Write canonical finite JSON and a human-readable report. The result must
   state separately whether diagnostic accounting passed and whether the
   scientific progression gates passed.

## Acceptance Criteria

The preflight diagnostic is valid only when:

- the exact Experiment 034 anchors and suite-to-manifest mapping validate;
- both dataset requests resolve to the pinned revisions, top-level feature-name
  sets, and episode/frame counts;
- contact observability is never promoted from a proxy;
- semantic phase is available from the frozen source itself or through a
  separately validated source-alignment map;
- required counts use `ceil(0.20 * steps)` and all task accounting conserves;
- a bank-independent failure under the frozen precheck mask forbids all
  downstream experimental stages;
- VLA calls, retrieved actions, threshold-grid points, and cloud resources are
  all zero after an abort;
- JSON contains no image, URL, embedding, raw action, or non-finite value; and
- focused tests cover empty/malformed input, boundary counts, network failure,
  response-size limits, revision mismatch, and fail-closed progression.

If admitted to the conditional paired stage, pooled and every task must each
reach 20% candidate-plus-cooldown reuse and 15% projected latency reduction.
Separability remains skipped unless separated and vetoed groups each have at
least 20 candidates plus the pre-registered source/time diversity floors.

## Failure Behavior And Disable Plan

Any missing/corrupt anchor, feature-name/count or revision mismatch, non-finite
value, or network exhaustion returns a bounded error and creates no cloud
resource. Unobservable contact, unaligned semantic phase, or a bank-independent
structural failure under the frozen precheck mask emits a bounded,
diagnostic-valid abort result and starts no downstream stage. There is no
product runtime path to disable because this experiment changes only research
tooling, tests, results, and documentation.

## Interpretation Boundary

A valid structural abort is a successful diagnostic and a negative progression
result. It does not mean the memory bank was compared, that open-hold coverage
was rebuilt, or that any action is safe. Even a future paired shadow pass would
measure agreement with a pinned VLA and simulator proxies, not risk-free action
or physical-robot safety.

## Command

```bash
python3 docs/research/tools/physical_ai_vla_trajectory_window_preflight.py \
  --prior-result docs/research/results/physical_ai_vla_veto_separability_compact_034.json \
  --result /tmp/physical_ai_vla_trajectory_window_preflight_035.json \
  --report /tmp/physical_ai_vla_trajectory_window_preflight_035.md
```

## Result

The local preflight completed with status `pass` and finding
`structural_abort`. It validated the exact Experiment 034 compact result,
task mapping, manifest/detail identities, accounting, and both pinned dataset
top-level feature-name sets and episode counts. Neither pinned feature-name set
exposes authoritative contact telemetry. The auxiliary source contains
`subtask_index`, but its 500 episodes are not aligned to the frozen 379-episode
source. Frozen-source semantic phase is therefore also unobservable. No
historical simulator replay mapping was provided or validated by this run.

Suite task 0's ceiling is bank-independent under the frozen Experiment 034
precheck mask. All 127 precheck-eligible rows already produced candidates,
and there were zero missing-memory observations. One-step cooldown reduced
them to 65/377
(17.24%). The per-task floor requires 76/377. Its optimistic projected latency
reduction was 15.961%, so reuse—not search latency—failed the task-0 gate. The
frozen pooled control remained 65/595 (10.92%) and 10.133% projected latency
reduction. Suite task 5 retained 58 potentially repairable missing-memory
observations, but changing that bank cannot satisfy the independent task-0
requirement under the frozen mask.

The experiment stopped before source-row or image download, bank construction,
model loading, VLA calls, paired comparison, separability analysis, threshold
grid, or cloud-resource creation. Retrieved actions and AWS resources created
were both zero.

Immutable artifacts:

- [`results/physical_ai_vla_trajectory_window_preflight_035.json`](results/physical_ai_vla_trajectory_window_preflight_035.json),
  SHA-256 `572f04026bb83bc4742b322831625493d361148148742b2842b042c92bff47d8`.
- [`results/physical_ai_vla_trajectory_window_preflight_035.md`](results/physical_ai_vla_trajectory_window_preflight_035.md),
  SHA-256 `5a28913b28ed6a0ff266196e677b37d6d24c9a742fea354e7156a239339fcfce`.
- Preflight tool SHA-256
  `d73452da16fcc714a4dd3372ee5756e830017816db5193ed68969cdae8cc694e`;
  pinned dataset-contract SHA-256
  `f18d9d3fbc67ffa315d441d72cc3c8203ff9d28c1f11daf3bdfcec2f61e03ea2`.

## Next Step

Because this experiment aborted on the frozen task-0 ceiling, do not rebuild
the bank or rerun the same VLA trace. Pre-register a separate multi-seed
query-eligibility
experiment to test whether the contact/cooldown ceiling persists outside the
two serial Experiment 034 episodes. Source reconstruction requires a new
provenance-preserving dataset with authoritative contact plus aligned semantic
labels, or an explicit raw-demo-to-simulator replay mapping that reconstructs
both, before trajectory windows can be called contact-free and phase-local.
