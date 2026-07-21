# Physical AI VLA Negative-Veto Separability 033 Results

> **Invalidated for task-attribution reporting.** Retrieval used the correct
> text-resolved memory partition, but this attempt reported source admission
> using LIBERO suite task IDs as manifest task indexes. The candidate artifact
> is preserved unchanged; the separately pre-registered Experiment 034 supersedes this
> report.

Generated at: 2026-07-18T13:59:17Z
Classification: Research-only
Status: invalidated - task-attribution reporting defect

## Result

Completed at `veto_separability_complete` after 595 shadow steps on tasks 0, 5.
Separability finding: `underpowered`.
No retrieved action was executed and no confidence/margin routing grid
was evaluated. This run captures exploratory candidate attribution and
coverage evidence; the frozen scope cannot supply a conclusive separated
comparison group.

Candidate-to-VLA agreement is a reference proxy, not action correctness,
risk-free behavior, or physical-safety evidence. The source bank contains
one midpoint per episode; source contact windows, semantic phases, and
subgoals are not observable in this experiment.

## Diagnostic Acceptance

| Check | Status |
| --- | --- |
| candidate groups consistent | pass |
| counterfactuals consistent | pass |
| detail rows consistent | pass |
| phase attribution consistent | pass |
| routed actions zero | pass |
| source admission consistent | pass |
| support attribution consistent | pass |
| task structural consistent | pass |
| aws resources deleted | pass |

## Provenance

- Run ID: `2137990792`.
- VLA revision: `6721902bc4d61e50a3bfdb11dfb4cb626f05d102`.
- SigLIP model/revision: `google/siglip-base-patch16-224` / `7fd15f0689c79d79e38b1c2e2e2370a7bf2761ed`.
- Harness bundle SHA-256: `292fde8b56ccd40b0c80ef71d0ce272f1c77aec39a7aa7778b319da9592cdc2b`.
- Semantic manifest SHA-256: `0624630ce232f33c36dbe20159ce3e88729ab0feffb6b67905f4bd2b180e85ba`.
- Candidate detail: [`physical_ai_vla_veto_separability_candidates_033.json`](physical_ai_vla_veto_separability_candidates_033.json), 127 rows, SHA-256 `67994a36b2335a1a46fde4992a5c42bfda6d45758a38c7cd6507ee342bd32561`.
- Experiment 032 replication anchors: steps=match, precheck=match, memory partitions=match, semantic manifest=match, VLA revision=match, SigLIP revision=match.

## Veto Groups

Configured veto margin: 0.010; high-error threshold: > 0.150; minimum comparable-group size: 20.

| Group | N | High error | Rate | MAE mean / p95 | Positive p50 | Negative p50 | Gap p50 / p95 | Disagreement mean | Unique top sources | Max source share |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| no_negative_support | 0 | 0 | N/A | N/A / N/A | N/A | N/A | N/A / N/A | N/A | 0 | N/A |
| separated | 4 | 0 | 0.0% | 0.055657 / 0.067481 | 0.797079 | 0.775330 | 0.016255 / 0.027979 | 0.075174 | 1 | 100.0% |
| vetoed | 123 | 26 | 21.1% | 0.121529 / 0.185638 | 0.741049 | 0.756193 | -0.015800 / 0.001992 | 0.075057 | 3 | 61.0% |

`no_negative_support` means no suppression neighbor existed. It is not
evidence that the positive candidate was safely separated.

### Veto-margin counterfactual

These rows reclassify the same candidates without simulating route reuse.

| Margin | No negative | Separated | Vetoed | Separated MAE mean / p95 | Separated high error |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0.000 | 0 | 21 | 106 | 0.079355 / 0.107910 | 0 |
| 0.005 | 0 | 7 | 120 | 0.067790 / 0.102781 | 0 |
| 0.010 | 0 | 4 | 123 | 0.055657 / 0.067481 | 0 |
| 0.020 | 0 | 2 | 125 | 0.050559 / 0.053466 | 0 |

## Query-Phase Candidate Attribution

Query phase is the early/middle/late third of the configured episode
limit, not a semantic source-memory phase.

| Task | Query phase | Hold | N | No negative | Separated | Vetoed | High error | MAE mean / p95 | Gap p50 | Unique sources |
| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | early | open_hold | 62 | 0 | 1 | 61 | 15 | 0.125150 / 0.183306 | -0.022468 | 3 |
| 0 | middle | open_hold | 65 | 0 | 3 | 62 | 11 | 0.114022 / 0.215102 | -0.004604 | 3 |

## Query Support And Source Admission

| Task | Query phase | Observed hold | Steps | Candidates | Missing executable memory |
| ---: | --- | --- | ---: | ---: | ---: |
| 0 | early | open_hold | 174 | 62 | 0 |
| 0 | late | closed_hold | 30 | 0 | 0 |
| 0 | middle | ambiguous | 2 | 0 | 0 |
| 0 | middle | closed_hold | 22 | 0 | 0 |
| 0 | middle | open_hold | 149 | 65 | 0 |
| 5 | early | ambiguous | 1 | 0 | 0 |
| 5 | early | closed_hold | 105 | 0 | 0 |
| 5 | early | open_hold | 68 | 0 | 58 |
| 5 | middle | closed_hold | 44 | 0 | 0 |

| Task | Open hold | Closed hold | Suppression | Admission reasons | Admission-mask histogram |
| ---: | ---: | ---: | ---: | --- | --- |
| 0 | 13 | 1 | 5 | open_hold=13, closed_hold=1, gripper_ambiguous=1, translation_limit=4 | 0:14, 1:1, 4:4 |
| 5 | 9 | 0 | 10 | open_hold=9, gripper_ambiguous=1, gripper_command_mismatch=7, translation_limit=2 | 0:9, 1:1, 2:6, 4:2, 10:1 |

Admission-mask bits are 1=gripper ambiguous, 2=command mismatch,
4=translation limit, and 8=rotation limit; a mask of 0 means none of these
source state/action rules fired. Source contact is not observable.

## Structural Preflight

| Scope | Steps | Candidates | Candidate + cooldown | Safety proxy + cooldown | Vetoed | Fixed pre-cooldown | Fixed + cooldown | Candidate latency | Fixed latency |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| pooled | 595 | 127 | 65 | 65 | 123 | 4 | 2 | 10.0% | -0.6% |
| task 0 | 377 | 127 | 65 | 65 | 123 | 4 | 2 | 15.7% | -1.0% |
| task 5 | 218 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0% | 0.0% |

## Timing

VLA mean 429.281 ms; encoder mean 10.882 ms; positive/suppression search mean 4.906/4.363 ms; combined search p95 9.618 ms.

## Interpretation

Comparable-group support sufficient: False; vetoed/separated high-error rates: 21.1%/0.0%; delta 21.1%.
Query support complete: False; pooled/per-task structural preflight viable: False/False.

The two episodes are serially correlated. Group differences are
descriptive associations on one ordered trace, not independent causal
effects. Underpowered groups cannot support either a separation or
non-separation claim. The veto remains unchanged.

## Next Research Step

Use the observed support and admission gaps to pre-register a separate
trajectory-window memory experiment with source contact/phase labels, task 5
closed-hold coverage, and a frozen-bank paired control. Routed execution
remains blocked.
