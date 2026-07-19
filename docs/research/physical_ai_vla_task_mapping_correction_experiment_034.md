# Experiment 034: Physical AI VLA Task-Mapping Correction

Date: 2026-07-18
Status: Research complete - corrected attribution pass
Classification: Research-only

## Goal

Repeat the frozen Experiment 033 shadow diagnostic with explicit, verified
mapping between LIBERO suite task IDs and manifest memory task indexes. Preserve
the valid candidate/veto telemetry while correcting source availability,
admission, and hold-coverage attribution. No retrieved action can execute.

## Correction Trigger

Experiment 033 runtime retrieval used exact task text and therefore queried the
correct memory namespace, but its compact report keyed source-memory tables by
the requested suite IDs. The two orderings differ:

| LIBERO suite task ID | Manifest task index | Instruction |
| ---: | ---: | --- |
| 0 | 5 | put both the alphabet soup and the tomato sauce in the basket |
| 5 | 9 | pick up the book and place it in the back compartment of the caddy |

The frozen midpoint partitions for the actual queried manifest tasks are:

| Manifest task index | Open hold | Closed hold | Suppression |
| ---: | ---: | ---: | ---: |
| 5 | 9 | 0 | 10 |
| 9 | 0 | 8 | 11 |

Consequently, Experiment 033's 58 missing-executable-memory observations must
be tested as suite task 5 / manifest task 9 `open_hold` demand, not as missing
task 5 `closed_hold` source memory.

## Frozen Scope And Hypotheses

- The same 190-memory/100-query semantic manifest, suite tasks 0 and 5,
  calibration seed 28, 520-step limit, policy seeding, actual SmolVLA/SigLIP
  revisions, risk rules, and 0.01 veto margin as Experiment 033.
- The same two shadow episodes only. No threshold grid, held-out episode, or
  routed execution.
- The candidate trace should reproduce 595 steps, 127 candidates, 123 vetoed,
  four separated, and the same candidate detail values as Experiment 033.
- The explicit mapping should report source admission only for manifest tasks 5
  and 9, and query/structural rows only by LIBERO suite task ID.
- Suite task 5 should show `open_hold` demand with no manifest-task-9 open-hold
  memory; this is expected to keep query support and per-task structural
  viability false.
- Separability remains expected to be `underpowered` because the frozen trace
  has only four separated candidates. Directional error differences remain
  exploratory associations, not evidence of action correctness or safety.

## Schema And Telemetry

- Compact diagnostic schema 2 uses `scope.suite_tasks` and an exact
  `scope.task_map` of suite ID, manifest index, and instruction.
- Query-phase and structural tables use suite task IDs. Availability and source
  admission use manifest task indexes.
- Candidate detail schema 2 names its first field `suite_task_id`, embeds task
  mapping fields/rows and codebooks, and retains no image, raw action, URL, or
  embedding.
- Schema-independent canonical candidate-row SHA-256 must reproduce Experiment
  033's preserved 127 rows:
  `466467ef024f62bc815069dfa849838cd3a2ec3408319c022fa88a85b6f4552a`.
- The historical Experiment 032 mis-keyed availability anchor is retained only
  as `replication_032_legacy_reported`; its match is payload compatibility, not
  queried-partition evidence.
- All Experiment 033 accounting, finite-value, counterfactual, detail-SHA,
  compact-size, zero-action, and AWS cleanup checks remain required.

## Procedure

1. Preserve the immutable Experiment 033 result, compact payload, and candidate
   artifact under invalidation evidence; never overwrite them.
2. Run focused tests for task-map conservation, actual 5/9 admission,
   schema/version semantics, immutable output refusal, and payload size.
3. Confirm shared EKS NodePools are idle and no Experiment 034 Kubernetes or EC2
   resources exist.
4. Reuse the completed deterministic manifest checkpoint to reconstruct the
   same semantic manifest, then create uniquely named temporary CPU/GPU
   NodePools and an isolated namespace.
5. Run suite tasks 0 and 5 in forced shadow mode, return after calibration, and
   validate the compact and candidate artifacts before cleanup.
6. Delete all owned namespace/NodePool/NodeClaim/node/EC2 resources and verify
   the shared bench remains idle without modifying shared workloads.

## Diagnostic Acceptance

Experiment 034 passes only when:

- suite-to-manifest mapping is one-to-one, exact-text verified, and conserved in
  every trace;
- reported availability/admission keys are exactly manifest tasks 5 and 9;
- query and structural rows are explicitly labeled as suite-task axes;
- candidate/detail/accounting invariants pass and retrieved actions remain 0;
- the trace reproduces 595 steps, group counts 0/4/123, the frozen precheck
  vector, and the Experiment 033 canonical candidate-row digest;
- compact standard JSON is at most 3,900 bytes and detail count/SHA match;
- all temporary AWS resources are deleted and shared NodePools remain at zero.

Candidate-count or error-value mismatch against Experiment 033 does not get
silently accepted; it limits the corrected comparison and must be explained.

## Interpretation Boundary

Candidate-to-VLA normalized MAE is a reference-agreement proxy. The simulator
predicates and memory labels do not establish risk-free behavior, physical
safety, or task correctness. Two serially correlated episodes cannot support a
general separability conclusion.

## Next Research Step

If mapping and trace replication pass, Experiment 035 should build deterministic
contact-free trajectory-window memory with semantic source-phase labels and
explicit suite-task-5/manifest-task-9 open-hold coverage. The rebuilt bank must
use a frozen-bank paired control and clear per-task density and projected
latency preflights before any new grid. Routed execution remains blocked.

> Experiment 035 outcome: the pre-VLA gates stopped this plan before bank
> construction. The frozen source exposes neither authoritative historical
> contact nor source-aligned semantic phase, and suite task 0 has a
> bank-independent candidate-plus-cooldown ceiling under the frozen Experiment
> 034 precheck mask of 65/377 (17.24%), below its required 20%. The next
> experiment moves to multi-seed query eligibility/cooldown measurement; the
> veto and cooldown
> remain unchanged.

## Command

```bash
tests/k8s/run_physical_ai_vla_task_mapping_correction.sh
```

## Result

EKS run `1321011761` completed with diagnostic status `pass`. It reproduced
595 shadow steps, 127 candidate rows, group counts 0/4/123, the frozen precheck
vector, and canonical Experiment 033 candidate-row SHA-256
`466467ef024f62bc815069dfa849838cd3a2ec3408319c022fa88a85b6f4552a`.
The schema-2 candidate rows are value-identical to Experiment 033; only the
self-describing header and task mapping changed.

The corrected source partitions are manifest task 5 with open/closed/
suppression counts 9/0/10 and manifest task 9 with 0/8/11. Suite task 5 had 68
observed early `open_hold` steps, including 58 `no_executable_memory` outcomes,
against manifest task 9's zero open-hold records. This replaces the invalid
task-5 closed-hold interpretation. It does not change the earlier 48/48 reuse
and projected-latency failures.

Separability remains `underpowered`: only four candidates were separated,
while 123 were vetoed. All four separated candidates are adjacent trace steps
173, 174, 177, and 178 with positive source episode 363. Negative source
episode 181 supplies 121/123 vetoes and all 26 high-error vetoed rows. The
directional 21.1-point high-error-rate difference is therefore source/time
confounded and cannot establish veto effectiveness, action correctness, or
safety.

The canonical compact JSON was 3,229 bytes. The immutable artifacts are:

- [`results/physical_ai_vla_veto_separability_compact_034.json`](results/physical_ai_vla_veto_separability_compact_034.json),
  3,229 payload bytes plus newline, SHA-256
  `6320991252857db66ae06f2772a6654813327a09944ac0c39ef37fb1c5be733d`.
- [`results/physical_ai_vla_veto_separability_034.md`](results/physical_ai_vla_veto_separability_034.md),
  SHA-256 `97bb3fe6e8ce67a34b9bdf1d050a3f7baefcf998efcdb835b3ca96c13db4ca5a`.
- [`results/physical_ai_vla_veto_separability_candidates_034.json`](results/physical_ai_vla_veto_separability_candidates_034.json),
  127 rows, SHA-256
  `f41225183345e526503bc24576567a7c461e348f4927486bb6b339ab2c84de56`.

No retrieved action executed: all 595 actions came from the pinned VLA. The
owned namespace, NodePools, NodeClaims, nodes, and EC2 instances
`i-01b55e2c7c3f9d757` and `i-0256b34ae2d80326f` were deleted/terminated.
Shared x86 and arm64 bench NodePools remained at CPU 0 with zero nodes.
