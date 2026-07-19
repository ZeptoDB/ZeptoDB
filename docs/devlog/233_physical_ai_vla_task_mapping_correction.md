# 233: Physical AI VLA Task-Mapping Correction

Date: 2026-07-18
Status: Complete

## Context

Experiment 033 completed its forced-shadow telemetry but exposed an inherited
reporting defect: LIBERO suite task IDs were used as manifest task indexes in
source availability/admission output. Retrieval itself already used exact task
text, so candidate/veto values remain useful, but the coverage interpretation
was not promotable.

Experiment 034 is a new immutable run identity. It corrects the mapping to
suite 0 to manifest 5 and suite 5 to manifest 9, preserves Experiment 033 as
invalidated evidence, and keeps all actions on the pinned VLA path.

## Changes

- Return resolved memory task index and exact instruction from each episode and
  attach the resolved index to every shadow trace.
- Add schema-2 `scope.suite_tasks` and task mapping, while keying source memory
  availability/admission by actual manifest indexes.
- Rename the candidate-detail task field to `suite_task_id`; add task mapping
  metadata and codebooks.
- Separate the historical mis-keyed Experiment 032 availability fingerprint
  from corrected queried-partition evidence.
- Label all query and structural report axes as LIBERO suite tasks and source
  tables as manifest task indexes.
- Make the EKS runner fail closed rather than overwrite an existing result or
  detail artifact.
- Require the collector to verify exact mapping, schema 2, 595 steps, 127
  candidate rows, group counts 0/4/123, and the schema-independent Experiment
  033 row digest before publishing an Experiment 034 artifact.
- Archive live Experiment 033 execution while retaining backward-compatible
  rendering of its preserved schema-1 compact payload.
- Bound error payloads by encoded bytes and publish generated artifacts through
  an atomic no-clobber hard-link step.
- Add a dedicated Experiment 034 entry point and EKS wrapper. The previously
  planned trajectory-window bank moves to Experiment 035.

## Experimental Boundary

This is research-only Python, test, runner, and documentation work. It changes
no ZeptoDB runtime API or product behavior. No retrieved action can execute.

## Verification

- Focused risk-router plus mapping-correction tests: 65 passed.
- Related Experiments 024-034 tests: 139 passed.
- Python compile, shell syntax, `git diff --check`, schema-1 legacy rendering,
  exact row-digest, encoded-error-bound, real-034 worst payload, and immutable
  output refusal checks: passed.
- ConfigMap input: 792,261 bytes, below the 900,000-byte pre-AWS guard.
- Canonical compact result: 3,229 bytes, below the 3,900-byte termination
  limit.
- EKS run `1321011761`: diagnostic pass, mapping and trace replication pass,
  zero routed actions, and complete Kubernetes/EC2 cleanup.
- Cross-architecture product verification is not applicable: this change is
  research Python, shell, tests, and documentation only. The ML execution used
  an amd64 GPU node and is not x86_64/aarch64 product-runtime validation.

## Result And Insight

Suite task 0 maps to manifest task 5 and suite task 5 maps to manifest task 9.
Their actual open/closed/suppression counts are 9/0/10 and 0/8/11. The 58
missing-executable-memory outcomes are suite-task-5 `open_hold` demand against
manifest-task-9 open-hold count zero. The schema-2 candidate rows reproduce all
127 Experiment 033 row values exactly.

The veto comparison remains underpowered and confounded. All four separated
rows are nearby steps 173, 174, 177, and 178 and use positive source episode
363. Negative source episode 181 accounts for 121/123 vetoes and every one of
the 26 high-error vetoed rows. The observed 21.1-point high-error-rate delta is
therefore descriptive source/time concentration, not evidence that the veto
causes safer or more correct actions.

The corrected mapping leaves Experiment 032's 48/48 calibration failure intact:
cooldown still caps the candidate ceiling at 65/595 (10.9%), fixed veto plus
cooldown still leaves 2/595, and projected latency remains below the 15% gate.
Only the source-partition explanation changed.

Artifacts:

- [`physical_ai_vla_veto_separability_compact_034.json`](../research/results/physical_ai_vla_veto_separability_compact_034.json),
  SHA-256 `6320991252857db66ae06f2772a6654813327a09944ac0c39ef37fb1c5be733d`.
- [`physical_ai_vla_veto_separability_034.md`](../research/results/physical_ai_vla_veto_separability_034.md),
  SHA-256 `97bb3fe6e8ce67a34b9bdf1d050a3f7baefcf998efcdb835b3ca96c13db4ca5a`.
- [`physical_ai_vla_veto_separability_candidates_034.json`](../research/results/physical_ai_vla_veto_separability_candidates_034.json),
  SHA-256 `f41225183345e526503bc24576567a7c461e348f4927486bb6b339ab2c84de56`.
- [`physical_ai_vla_failure_root_cause_synthesis_032_034.md`](../research/results/physical_ai_vla_failure_root_cause_synthesis_032_034.md),
  the cross-experiment 48-failure and source-confounding analysis.

## Follow-Up

Use the corrected suite-task-5/manifest-task-9 open-hold gap to design
Experiment 035 trajectory-window memory. Do not reinterpret the old 0/5 source
admission table or weaken veto/cooldown thresholds.
