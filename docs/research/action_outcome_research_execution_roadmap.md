# Action-Outcome Memory Research Execution Roadmap

Date: 2026-06-13
Branch: `codex/aiops-time-series-memory-research`

## Goal

Turn the Action-Outcome Memory Engine idea into a research program with
measurable experiments, comparable baselines, and a prototype path.

The research should answer:

1. Does action-outcome memory improve incident response recommendations over
   text-only or vector-only memory?
2. Which signals are necessary: metrics, logs, traces, topology, deploy events,
   action history, postmortems, or agent traces?
3. Can the system safely progress from replay to shadow recommendation to
   guarded execution?
4. Can the same primitive later support robot/physical-AI state-action-outcome
   memory?

## Recommended Research Strategy

Use a staged approach:

```text
Research fixture
  -> Replay-only evidence packs
  -> Baseline comparison
  -> Fault-injection lab
  -> Shadow recommender
  -> Advisory copilot
  -> Guarded execution
  -> Physical AI extension
```

The first serious artifact should be **ActionOutcomeReplay**, not a live
autonomous remediator.

## Phase 0: Lock The Research Scope

### Hypothesis

Action-outcome memory improves AIOps agents because it retrieves prior episodes
with:

- similar symptoms,
- similar temporal metric/log/trace patterns,
- similar topology,
- similar change context,
- similar attempted actions,
- known recovery outcomes.

### Scope

Start with cloud/SRE AIOps, not robots.

Robotics should be the second domain because it shares the same primitive:
state-action-outcome memory. Starting with AIOps is easier because incidents,
alerts, telemetry, and runbooks already exist in enterprise workflows.

### Non-goals

- No arbitrary production command execution.
- No broad autonomous remediation.
- No generic vector database positioning.
- No model fine-tuning requirement for the first paper/prototype.
- No claim that LLMs alone can perform reliable RCA.

### Deliverables

- Final problem statement.
- Formal hypothesis.
- Dataset list.
- Baseline list.
- Initial `incident_memory` and `action_outcome_episode` schema.

## Phase 1: Build The Research Dataset

### Data Sources

Use three layers of data:

| Layer | Source | Purpose |
| --- | --- | --- |
| Public benchmark | AIOpsLab, RCAEval, OpenRCA, CCF AIOPS-style HipsterShop data | Comparable academic evaluation. |
| Synthetic lab | Injected microservice/Kubernetes faults | Generate action-outcome labels missing from public datasets. |
| Internal ZeptoDB fixture | Small hand-authored incident timelines | Fast iteration and demo stability. |

### Minimum Episode Fields

Each episode should include:

- `episode_id`
- `incident_id`
- `service`
- `environment`
- `entity_refs`
- `pre_action_window`
- `action_ts`
- `post_action_window`
- `symptoms`
- `topology_context`
- `change_context`
- `candidate_root_causes`
- `action_class`
- `action_parameters`
- `policy_decision`
- `rollback_plan`
- `machine_outcome_label`
- `human_outcome_label`
- `recovery_curve_ref`
- `postmortem_ref`
- `reflection`

### Data Quality Checks

- Event timestamps and ingest timestamps must both exist.
- Every action must have a target entity.
- Every executed action must have a post-action observation window.
- Every outcome label must state who or what assigned it.
- Failed actions and rollbacks must be preserved, not filtered out.

### Deliverables

- `action_outcome_episode` schema.
- Small fixture with at least 20 episodes.
- Public benchmark import notes.
- Synthetic fault plan.

## Phase 2: Build ActionOutcomeReplay

### Purpose

Replay old incidents and ask:

> Given only the early incident window, would the engine retrieve useful prior
> outcomes and recommend a better next action?

### Inputs

- incident id or fixture id,
- current alert,
- service/entity,
- time window,
- optional candidate action.

### Outputs

- top similar episodes,
- aligned telemetry timelines,
- prior actions,
- recovery curves,
- successful prior actions,
- failed prior actions,
- recommended action ranking,
- evidence pack,
- uncertainty and missing evidence.

### Implementation Notes

Do this as a prototype outside the core database first. Use ZeptoDB tables and
existing Agent Memory APIs rather than adding new C++ storage internals.

### Deliverables

- Replay script or notebook.
- Evidence pack JSON format.
- Sample replay report for 3-5 incident types.

## Phase 3: Compare Against Baselines

### Baselines

| Baseline | Description |
| --- | --- |
| Keyword search | Search postmortems/runbooks by service and alert text. |
| Text-only RAG | Vector search over incident summaries and postmortems. |
| LLM-only RCA | Prompt LLM with alert text and selected logs. |
| Time-series evidence pack | Use aligned telemetry, topology, and change context. |
| Action-outcome memory | Add prior actions and recovery outcomes. |

### Metrics

| Metric | Target |
| --- | --- |
| top-k similar episode recall | Action-outcome memory beats text-only retrieval. |
| action top-k match | Recommended actions match known successful historical actions. |
| unsafe recommendation rate | Lower than LLM-only and text-only baselines. |
| evidence precision | Retrieved evidence supports the recommendation. |
| recovery prediction accuracy | Correctly predicts whether action helps within N minutes. |
| retrieval latency | Fast enough for incident workflow. |

### Deliverables

- Baseline comparison table.
- Error analysis.
- Ablation study by signal type.

## Phase 4: Build A Fault-Injection Lab

### Why

Public datasets often contain RCA labels but not clean action-outcome labels.
The lab creates repeatable episodes where outcomes are known.

### Environment

Start with a small microservice or Kubernetes environment. AIOpsLab is a strong
candidate because it already supports fault injection, workloads, telemetry
export, and agent evaluation.

### Faults

- bad deploy,
- connection pool exhaustion,
- dependency timeout,
- CPU saturation,
- memory leak,
- queue backlog,
- bad config,
- partial zone/network degradation.

### Actions

- rollback,
- restart,
- scale out,
- traffic drain,
- config revert,
- queue consumer reset.

### Deliverables

- Fault/action matrix.
- 100+ labeled action-outcome episodes.
- Recovery curves for each action.
- Benchmark report.

## Phase 5: Shadow Recommender

### Purpose

Run on live or replayed incidents without showing or executing recommendations.
This tests whether recommendations would have been useful without creating
operational risk.

### Procedure

1. Observe incident data.
2. Generate evidence pack.
3. Recommend action.
4. Hide recommendation during live response or compare after replay.
5. Score against human action and final outcome.

### Deliverables

- Shadow mode logs.
- Recommendation-vs-human comparison.
- Unsafe recommendation review.
- Updated scoring weights.

## Phase 6: Advisory Copilot

### Purpose

Show recommendations to humans while preserving human control.

### Output Requirements

Every recommendation must include:

- why this action,
- what prior episodes match,
- what worked before,
- what failed before,
- expected recovery signal,
- rollback plan,
- risk tier,
- missing evidence.

### Human Feedback

Capture:

- accepted,
- rejected,
- modified,
- escalated,
- unsafe,
- insufficient evidence,
- wrong root cause.

### Deliverables

- Human feedback dataset.
- Operator acceptance rate.
- Time-to-decision improvement estimate.

## Phase 7: Guarded Execution

### Entry Criteria

Do not start this phase until:

- replay and shadow results beat baselines,
- unsafe recommendation rate is acceptably low,
- action policy is explicit,
- rollback is tested,
- audit trail is complete.

### Execution Constraints

Only allow:

- reversible actions,
- scoped credentials,
- allowlisted targets,
- bounded blast radius,
- approval gates or strict auto-approval rules,
- canary execution,
- rollback hooks.

### Deliverables

- Lab-only guarded execution demo.
- Before/after MTTR comparison.
- Safety incident report, including near misses.

## Phase 8: Physical AI Extension

### When To Start

Start after AIOps replay/shadow results show that action-outcome memory improves
recommendations.

### Translation

| AIOps | Physical AI |
| --- | --- |
| service topology | robot/device/environment state |
| metrics/logs/traces | sensors/control/state streams |
| remediation action | robot action or planner adjustment |
| recovery curve | task success, safety margin, operator override |
| postmortem | human correction or task critique |
| rollback | stop, retry, alternate path, human takeover |

### First Physical AI Fixture

Use a simple robot/fleet scenario:

- wheel slip,
- obstacle encounter,
- bad route,
- failed grasp,
- user preference violation,
- operator override.

### Deliverables

- Robot action-outcome episode schema.
- Local edge recorder design.
- 30-300 second pre/post anomaly replay.
- Similar episode retrieval demo.

## Suggested Timeline

| Timebox | Focus | Output |
| --- | --- | --- |
| Week 1 | Scope and schema | Final schema, hypotheses, benchmark plan. |
| Week 2 | Fixture data | 24 hand-authored episodes and evidence pack format. |
| Week 3 | Replay prototype | Similar episode retrieval and action ranking. |
| Week 4 | Baselines | Keyword, text-RAG, vector-RAG comparisons. |
| Weeks 5-6 | Fault lab | 100+ generated action-outcome episodes. |
| Week 7 | Shadow evaluation | Recommendation-vs-human or recommendation-vs-ground-truth report. |
| Week 8 | Paper/product decision | Decide whether to pursue advisory copilot or guarded execution demo. |

## Go / No-Go Criteria

### Go

Continue if:

- action-outcome retrieval beats text-only/vector-only baselines,
- recommendations are explainable with evidence,
- negative outcomes improve future recommendations,
- recovery prediction is useful for at least 3 recurring incident types,
- safety controls can prevent broad unsafe action.

### No-Go

Pause or redirect if:

- retrieval only works by memorizing service names,
- action recommendations are mostly generic,
- outcome labels are too noisy to evaluate,
- unsafe recommendation rate remains high,
- evidence packs are too slow or too large for incident workflows.

## Research Output Formats

The work should produce:

- a research paper-style report,
- benchmark dataset notes,
- schema definitions,
- evidence pack examples,
- baseline comparison tables,
- ablation study,
- demo script,
- product MVP memo.

## Most Important First Step

Create a small, high-quality fixture before building any automation.

Minimum useful fixture:

- 5 or more incident types,
- 4 episodes per type,
- at least 20 total episodes,
- at least one success and one failure per action class,
- pre-action and post-action time windows,
- human-readable postmortem/reflection,
- enough metadata to test retrieval.

If this fixture cannot show the value of action-outcome memory, a larger system
will not save the idea.
