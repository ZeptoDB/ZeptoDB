# 234: Physical AI VLA Trajectory-Window Preflight

Date: 2026-07-19
Status: Complete

## Context

Experiments 032-034 showed that the 48 failed confidence/margin settings were
different counterfactuals over one structurally sparse trace, not independent
trials. The corrected task map also identified a real source-coverage gap:
suite task 5 maps to manifest task 9, whose midpoint bank has no open-hold
executable memory.

Experiment 035 was pre-registered to build a contact-observable trajectory
bank only after two pre-VLA gates: source-label provenance and frozen-trace
structural viability. The goal was to avoid spending GPU/cloud resources on a
paired bank comparison that could not meet its own acceptance criteria.

## Changes

- Add a dedicated research-only trajectory-window preflight tool.
- Validate the exact Experiment 034 compact, manifest, candidate-detail,
  harness, task-map, availability, precheck, and structural fingerprints.
- Pin and validate the top-level feature-name sets and episode/frame counts of
  `lerobot/libero_10_image` and `lerobot/libero_10_image_subtask`.
- Treat only authoritative contact telemetry or state-verified simulator replay
  as source-contact evidence. State/action limits and temporal thirds remain
  explicitly non-authoritative proxies.
- Compute the per-task reuse requirement with `ceil(0.20 * steps)` and identify
  whether a ceiling is bank-independent under the frozen Experiment 034
  precheck mask from eligible/candidate/missing-memory accounting.
- Fail closed before source rows, images, models, VLA calls, threshold grids,
  paired comparison, or EKS when provenance or structural gates fail.
- Emit finite canonical JSON and a human-readable report through no-clobber
  publication.
- Add focused tests for exact evidence identity, conservation, boundary counts,
  schema/revision drift, malformed/oversized/failed network responses,
  fail-closed progression, no-clobber behavior, and safety-claim boundaries.

## Experimental Boundary

This is research-only Python, tests, results, and documentation. It changes no
ZeptoDB runtime behavior, API, query planning, routing, telemetry, or product
feature. The conditional trajectory bank was not constructed, and no retrieved
action could execute.

## Result

The diagnostic passed and stopped at `pre_vla_structural_abort` on three
fail-closed gates in two blocker classes.

1. Neither pinned source top-level feature-name set contains authoritative
   historical contact. The auxiliary corpus adds `subtask_index`, but its 500
   episodes are not aligned to the frozen 379-episode source. It therefore
   establishes neither frozen-source semantic phase nor contact provenance;
   no historical replay mapping was provided or validated by this run.
2. Suite task 0 is already fully covered under the frozen precheck mask: all
   127 eligible observations produced candidates and none failed for missing
   executable memory. One-step cooldown reduces those candidates to 65/377
   (17.24%), below the required 76/377 (20%). A memory-bank change cannot raise
   that frozen ceiling without changing precheck, cooldown, workload, or policy
   scope.

The pooled midpoint control remains 65/595 (10.92%) with 10.133% projected
latency reduction. Suite task 5's 58 missing open-hold opportunities may still
be repairable with a new source bank, but cannot satisfy the independent
task-0 gate.

Execution accounting remained zero for source trajectory rows/images, model
loads, VLA calls, retrieved actions, paired comparisons, threshold grids, and
AWS resources. No cleanup was required.

## Verification

- New focused preflight tests: 20 passed.
- All Physical AI VLA Python tests: 144 passed.
- Live pinned-metadata preflight: diagnostic pass, structural abort.
- Canonical JSON/report staged-output byte equality and SHA-256 checks: passed.
- Python compilation and repository whitespace checks: passed.
- Cross-architecture product verification is not applicable. No C++ or product
  runtime changed, and no ML/cloud execution occurred.

## Artifacts

- [`physical_ai_vla_trajectory_window_preflight_experiment_035.md`](../research/physical_ai_vla_trajectory_window_preflight_experiment_035.md)
- [`physical_ai_vla_trajectory_window_preflight_035.json`](../research/results/physical_ai_vla_trajectory_window_preflight_035.json),
  SHA-256 `572f04026bb83bc4742b322831625493d361148148742b2842b042c92bff47d8`
- [`physical_ai_vla_trajectory_window_preflight_035.md`](../research/results/physical_ai_vla_trajectory_window_preflight_035.md),
  SHA-256 `5a28913b28ed6a0ff266196e677b37d6d24c9a742fea354e7156a239339fcfce`
- Preflight tool SHA-256:
  `d73452da16fcc714a4dd3372ee5756e830017816db5193ed68969cdae8cc694e`;
  pinned dataset-contract SHA-256:
  `f18d9d3fbc67ffa315d441d72cc3c8203ff9d28c1f11daf3bdfcec2f61e03ea2`

## Follow-Up

Do not build the bank or rerun the same two serial EKS episodes. The next
experiment should freeze multiple new query seeds and use pinned direct-VLA
actions to measure raw runtime contact eligibility plus cooldown placement
before any bank encoding or retrieval. Only if that workload-level ceiling is
viable should source-data work resume.
Calling historical windows `contact-free` and phase-local additionally requires
a new dataset with authoritative contact plus source-aligned semantic labels,
or a verified raw-demo replay map that reconstructs both.
