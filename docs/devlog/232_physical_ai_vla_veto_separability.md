# 232: Physical AI VLA Negative-Veto Separability

Date: 2026-07-18
Status: Complete - telemetry retained, source attribution invalidated

## Context

Experiment 032 showed that the configured negative veto matched 123/127
candidates, but the compact grid report retained action quality only for two
accepted candidates. The four total non-veto candidates also make the frozen
minimum of 20 separated candidates impossible under exact replication. The
experiment therefore captures exploratory attribution and coverage evidence;
it cannot make a conclusive separability claim at this scope.

Experiment 033 is a research-only, forced-shadow diagnostic over the unchanged
midpoint bank. The bank has no source phase labels, so the experiment measures
runtime query-phase support and defers a real trajectory-window bank to
Experiment 035. Experiment 034 is reserved for the immutable task-mapping
correction discovered after this run.

## Changes

- Record frozen-bank memory admission reasons without changing partition
  assignment.
- Retain bounded candidate support fields needed for aggregate positive versus
  suppression similarity, gap, disagreement, source concentration, and
  candidate-to-VLA MAE analysis.
- Add mutually exclusive vetoed, separated, and `no_negative_support` groups.
- Add task/query-phase/hold coverage, missing-partition demand, and per-task
  structural preflight aggregates without running another threshold grid.
- Add an Experiment 033 entry point that forces the pre-registered identity,
  parameters, and shadow-only mode.
- Add a dedicated EKS wrapper and compact report renderer.
- Preserve candidate-level numeric evidence as a canonical JSON artifact linked
  by count and SHA-256, while keeping the Kubernetes termination summary below
  3,900 bytes.
- Add a pre-AWS 900,000-byte ConfigMap input guard and post-pod detail-artifact
  integrity checks to the shared staged runner.
- Pin the actual SmolVLA, SigLIP, and VLA processor loads to the preserved
  revisions, not only the provenance lookup.
- Derive the zero-routed-action acceptance from per-episode VLA-call/reuse
  accounting and strengthen admission/high-error/structural conservation.
- Abort when the shared bench or a concurrent Experiment 033 run is active;
  cleanup deletes owned resources and verifies shared state without modifying
  another workload.

## Experimental Boundary

This work changes only research Python, test scripts, and research docs. It
does not change ZeptoDB runtime behavior or expose an API. No retrieved action
can execute. Candidate-to-VLA agreement and simulator predicates are not
physical-safety labels.

## Verification

- New Experiment 033 focused tests: 16 passed.
- Risk-router plus Experiment 033 focused tests: 59 passed.
- Related Experiments 024-033 Python tests: 133 passed.
- Python compile, shell syntax, diff, whitespace, exact termination boundary,
  and adversarial bounded compact-payload checks: passed.
- Current ConfigMap input using the refreshed Experiment 032 manifest shape:
  778,738 bytes, below the 900,000-byte pre-AWS guard.
- EKS run `2137990792`: 595 shadow steps, 595 VLA calls, zero retrieved
  actions, 127 candidates, 123 vetoes, and complete AWS cleanup.
- Original generated report SHA-256 before the invalidation banner:
  `5cad22daa50bf7d85ef18150794f3c665e24880fdb17b704fb7ff26004434844`.
- Frozen invalidated canonical report SHA-256:
  `4929fa4bb611e021f0b0755c3d1b122529705185122389e28432f934d6fd1748`.
- Candidate artifact: 127 rows, SHA-256
  `67994a36b2335a1a46fde4992a5c42bfda6d45758a38c7cd6507ee342bd32561`.

## Post-Run Invalidation

Candidate and trace telemetry completed, but the compact source-memory table
used suite task IDs as manifest indexes. Runtime retrieval itself used exact
task text. The valid rows are retained as immutable evidence, while the 0/5
source availability/admission attribution and task-5 closed-hold conclusion
are invalidated. Experiment 034 corrected the map to suite 0 -> manifest 5 and
suite 5 -> manifest 9 and reproduced all 127 candidate row values.

The corrected gap is suite-task-5 `open_hold` demand against manifest-task-9
open-hold count zero. Separability remains underpowered and cannot support a
veto-effectiveness, risk-free-action, or physical-safety claim.

## Follow-Ups

- Use the corrected Experiment 034 result to specify Experiment 035
  trajectory-window memory rather than
  weakening the veto.
- Keep routed execution and manipulation-phase VLA feature caching separate.
