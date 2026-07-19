# Physical AI VLA Trajectory-Window Preflight 035 Results

Generated at: 2026-07-19T02:19:02+00:00
Classification: Research-only
Status: pass - diagnostic-valid structural abort

## Result

Experiment 035 stopped before source-window extraction, model loading,
EKS, or VLA execution. The pinned frozen source exposes neither
authoritative historical contact nor semantic phase, and the suite-task-0
candidate-plus-cooldown ceiling is bank-independent under the frozen
Experiment 034 precheck mask at 65/377
(17.24%), below the required 76/377 (20%).

This is a valid negative progression result. It is not a paired bank
comparison and provides no action-correctness, risk-free, or safety evidence.

## Source Observability

| Dataset | Episodes | Frozen-source aligned | Contact feature | Semantic subtask feature |
| --- | ---: | --- | --- | --- |
| `lerobot/libero_10_image`@`7e324b526699f444044952c82ce3f438e8d300d0` | 379 | yes | none | none |
| `lerobot/libero_10_image_subtask`@`06fdb000a8d6d3f43c79abb2545a24379265bef8` | 500 | no | none | subtask_index |

The separate 500-episode auxiliary corpus exposes `subtask_index`, but
it is not aligned to the frozen 379-episode source bank. The validated
frozen-source top-level feature-name set exposes no contact, object state,
simulator state, or semantic subtask. No historical simulator replay
mapping was provided or validated by this run. State/action rules
therefore remain kinematic
proxies and were not promoted to `contact-free` or semantic labels.

## Frozen Structural Preflight

| Scope | Steps | Eligible | Missing memory | Candidates | After cooldown | Required | Reuse ceiling | Candidate latency | Bank-independent under frozen precheck |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| pooled | 595 | - | - | 127 | 65 | 119 | 10.92% | 10.13% | no |
| suite task 0 / manifest 5 | 377 | 127 | 0 | 127 | 65 | 76 | 17.24% | 15.96% | yes |
| suite task 5 / manifest 9 | 218 | 0 | 58 | 0 | 0 | 44 | 0.00% | 0.00% | no |

Suite task 0 already converted every precheck-eligible observation into
a candidate and had zero missing-memory observations. A source-bank
change cannot add candidates to that frozen trace without changing the
precheck, cooldown, workload, or policy scope. Those changes were outside
the pre-registration.

Manifest task 9's 58 missing open-hold opportunities may be repairable,
but they cannot repair the independent per-task failure on suite task 0.

## Diagnostic Acceptance

| Check | Status |
| --- | --- |
| prior evidence valid | pass |
| dataset revisions pinned | pass |
| dataset feature name sets and counts valid | pass |
| contact provenance available | not met |
| frozen source semantic phase available | not met |
| bank independent under frozen precheck viable | not met |
| downstream stages blocked after failure | pass |
| zero vla calls | pass |
| zero retrieved actions | pass |
| zero cloud resources | pass |
| diagnostic valid | pass |

`contact_provenance_available`,
`frozen_source_semantic_phase_available`, and
`bank_independent_under_frozen_precheck_viable` are scientific
progression gates; their expected failure does not invalidate the
preflight accounting.

## Execution Accounting

- Source rows/images downloaded: 0 / 0.
- Models loaded and VLA calls: 0 / 0.
- Retrieved actions: 0.
- Threshold grids and paired-bank comparisons: not started.
- AWS/EKS resources created: 0; cleanup was not required.

## Interpretation

Do not build the proposed trajectory bank or rerun the same EKS trace.
The next useful test is a separately pre-registered, multi-seed shadow
experiment that uses pinned direct-VLA actions to measure whether query
contact eligibility and cooldown placement remain below the target on
new frozen traces before any bank encoding or retrieval. Provenance-
preserving contact plus source-aligned semantic labels, or a raw-demo
simulator replay mapping that reconstructs both, is still required before
historical windows can be called contact-free and phase-local.
