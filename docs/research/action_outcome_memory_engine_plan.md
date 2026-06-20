# Action-Outcome Memory Engine Research Plan

Date: 2026-06-13
Branch: `codex/aiops-time-series-memory-research`

Execution roadmap:
`docs/research/action_outcome_research_execution_roadmap.md`

## Decision

The best high-risk/high-upside research direction is an **Action-Outcome Memory
Engine** for AIOps first, with a later path into physical AI and agentic SOC.

The engine should not start as a fully autonomous production actor. It should
start as a memory and evaluation layer that learns which remediation actions
worked under which temporal conditions, then gradually move from replay to
recommendation to guarded execution.

Core thesis:

> The reusable primitive is not "incident summary memory". It is a time-series
> memory of state, action, policy, recovery, and outcome.

## What The Engine Must Remember

An action-outcome memory entry should capture an operational episode:

| Field Group | Examples | Purpose |
| --- | --- | --- |
| Situation | service, resource, topology, alert, metric/log/trace windows, recent deploys | Defines the pre-action state. |
| Hypothesis | candidate root causes, evidence, confidence, rejected causes | Explains why an action was considered. |
| Policy | allowed action class, risk tier, approval, blast-radius limit, rollback requirement | Makes automation governable. |
| Action | tool, command, target, parameters, actor, timestamp, idempotency key | Records what changed. |
| Observation | post-action metric/log/trace windows, recovery curve, side effects | Measures what happened next. |
| Outcome | success, partial success, failure, rollback, recurrence, human override | Turns an event into reusable memory. |
| Reflection | short lesson, failure mode, future guardrail, runbook delta | Makes retrieval useful for agents. |

## Recommended Procedure

### 1. Define A Narrow Automation Envelope

Start with 3-5 reversible action classes. Do not begin with arbitrary shell
access or broad Kubernetes admin access.

Recommended first actions:

- restart a known stateless service or pod,
- scale out a deployment within a fixed bound,
- roll back to the last known-good deploy,
- drain traffic from a bad instance or zone,
- purge a cache or reset a bounded queue consumer.

Each action class needs:

- allowlisted target types,
- preconditions,
- success criteria,
- rollback plan,
- maximum blast radius,
- required approval level,
- timeout and abort rules.

### 2. Capture The Full Incident Timeline

The engine should ingest and align:

- alerts,
- metrics,
- logs,
- traces,
- topology snapshots,
- deploy/config/feature-flag events,
- runbook/tool actions,
- human approvals,
- agent tool calls,
- postmortem labels.

Use event time as the primary axis and ingestion time as an audit axis. This
matters because delayed logs, edge devices, and replay systems can otherwise
distort causal order.

### 3. Store Facts As Time-Series, Lessons As Agent Memory

ZeptoDB should use two complementary storage layers:

1. **Time-series tables**
   - Raw or summarized operational facts.
   - Incident windows.
   - Agent tool calls.
   - Remediation actions.
   - Recovery curves.
   - Policy decisions.
   - Outcome labels.

2. **Agent Memory records**
   - Human-readable lessons.
   - Postmortem summaries.
   - Remediation reflections.
   - Reusable runbook notes.
   - Known failure signatures.

This follows the existing ZeptoDB design: the time-series engine stores what
happened, while Agent Memory stores what an agent learned or should reuse.

### 4. Build The Core Tables

Suggested first tables:

| Table | Role |
| --- | --- |
| `incident_events` | Alert lifecycle, severity, affected service, incident ids. |
| `incident_windows` | References to pre/during/post telemetry windows. |
| `topology_snapshots` | Service/resource/dependency graph at a point in time. |
| `change_events` | Deploys, config changes, feature flags, migrations. |
| `agent_tool_calls` | Agent prompts, tool calls, tool outputs, latency, errors. |
| `policy_decisions` | Action allow/deny/escalate decisions and reasons. |
| `remediation_actions` | Actions attempted by humans or agents. |
| `recovery_observations` | Post-action metric/log/trace recovery windows. |
| `action_outcomes` | Success/failure/rollback/recurrence labels. |
| `memory_reflections` | Lessons extracted from the episode. |

Large raw payloads should be referenced by hash/path/object URI. ZeptoDB should
store aligned metadata, features, and time-window references.

### 5. Generate Episode Records

An episode is created when one of these happens:

- alert fires,
- incident opens,
- agent starts investigation,
- remediation action is proposed,
- remediation action is executed,
- incident resolves,
- postmortem is completed.

Each episode should include:

- `episode_id`,
- `incident_id`,
- pre-action window,
- action timestamp,
- post-action observation window,
- target entity,
- action class,
- policy decision,
- outcome label,
- evidence references,
- reflection text.

### 6. Retrieve Prior Outcomes Before Recommending Action

Retrieval should be hybrid. Do not rely only on vector similarity.

Recommended retrieval features:

- symptom similarity: alerts, error codes, log templates, trace failures,
- temporal motif similarity: shape of metric deviations before action,
- topology similarity: same service, dependency, node, region, device, fleet,
- change similarity: similar deploy/config/feature-flag context,
- action similarity: same candidate remediation class,
- outcome similarity: previous success/failure/rollback,
- policy similarity: same risk tier and allowed-action envelope.

Suggested scoring model for the first prototype:

```text
score =
  0.20 * symptom_similarity +
  0.20 * temporal_motif_similarity +
  0.15 * topology_similarity +
  0.15 * change_similarity +
  0.15 * action_outcome_similarity +
  0.10 * postmortem_text_similarity +
  0.05 * recency_score -
  risk_penalty
```

The exact weights should be configurable and benchmarked.

### 7. Produce An Evidence Pack

Before any action recommendation, the engine should return a structured evidence
pack:

- current incident summary,
- top similar prior episodes,
- aligned timelines,
- matching signals,
- candidate root causes,
- action options,
- prior action outcomes,
- safety constraints,
- rollback plan,
- missing evidence,
- confidence and uncertainty.

The agent should cite this evidence pack rather than invent reasoning from raw
context.

### 8. Execute In Four Modes

The same engine should support four operating modes:

| Mode | Behavior | Use |
| --- | --- | --- |
| Replay | Evaluate old incidents and compare recommended actions with historical actions. | Research and offline benchmarking. |
| Shadow | Watch live incidents and recommend actions without showing or executing them. | Calibration and safety testing. |
| Advisory | Show evidence packs and recommended actions to humans. | First production use. |
| Guarded execution | Execute reversible, policy-approved actions with approval or strict automation gates. | High-upside product path. |

Do not jump directly to autonomous execution.

### 9. Measure Recovery

Outcome labels should be computed from post-action time windows, not only human
notes.

Suggested measurements:

- error rate delta,
- latency p95/p99 delta,
- saturation change,
- alert state change,
- user-facing SLO burn change,
- dependency health change,
- recurrence within N minutes/hours,
- rollback required,
- human override,
- side-effect alerts.

The system should store both machine-computed labels and human-confirmed labels.

### 10. Write The Outcome Back

After action and observation, write:

- `action_outcomes` row,
- `recovery_observations` rows,
- `memory_reflections` row,
- Agent Memory record of type `remediation_outcome`,
- optional runbook delta.

Negative outcomes must be first-class. A failed action is often more valuable
than a successful one because it prevents repeated damage.

## Safety Design

Guarded execution requires policy before intelligence.

Minimum controls:

- agent identity and scoped credentials,
- action allowlist,
- target allowlist,
- approval requirements by risk tier,
- blast-radius budget,
- dry-run when available,
- canary execution before broad execution,
- idempotency keys,
- rollback hooks,
- timeout and abort conditions,
- immutable audit log,
- prompt/tool-output redaction,
- memory poisoning checks,
- tenant and namespace isolation.

The engine should treat every action as a transaction-like episode:

1. prepare evidence,
2. request policy decision,
3. prepare action,
4. execute or deny,
5. observe outcome,
6. commit memory,
7. publish audit trace.

## Evaluation Plan

### Offline Benchmarks

Use public and synthetic benchmarks first:

- AIOpsLab for interactive fault injection and agent evaluation.
- RCAEval for root-cause analysis datasets and baselines.
- OpenRCA for real incident cases with telemetry.
- CCF AIOPS 2025-style HipsterShop data for metrics/logs/traces.

Offline comparisons:

| Baseline | Description |
| --- | --- |
| Text-only RAG | Retrieve postmortem/runbook text only. |
| Vector incident memory | Retrieve incident summaries by embedding similarity. |
| Time-series evidence pack | Retrieve aligned telemetry windows and topology/change context. |
| Action-outcome memory | Retrieve prior actions and observed recovery curves. |

### Lab Environment

Build a small Kubernetes or microservice lab and inject recurring faults:

- bad deploy,
- memory leak,
- CPU saturation,
- dependency timeout,
- DB connection pool exhaustion,
- queue backlog,
- bad config,
- zonal/network degradation.

Pair each fault with allowed reversible actions:

- rollback,
- restart,
- scale out,
- traffic drain,
- config revert,
- queue consumer reset.

### Metrics

| Metric | Meaning |
| --- | --- |
| top-k similar episode recall | Does retrieval find prior relevant incidents? |
| RCA top-1/top-3 accuracy | Does evidence improve root-cause ranking? |
| action success rate | Do recommended actions recover the system? |
| unsafe recommendation rate | How often does the engine recommend a harmful action? |
| unnecessary action rate | How often would the system act when observation is enough? |
| rollback rate | How often does remediation require rollback? |
| MTTA / MTTM / MTTR delta | Time to acknowledge, mitigate, and resolve. |
| recurrence reduction | Whether repeated incidents decrease. |
| evidence precision | Whether cited evidence actually supports the recommendation. |
| audit reproducibility | Whether reviewers can replay why the decision was made. |

## Phased Build Plan

### Phase 0: Research Fixture

Goal: make the data comparable.

- Define `incident_memory` and action-outcome episode schema.
- Import or simulate benchmark incidents.
- Store telemetry windows and action/outcome records.
- Build read-only evidence pack generation.

Exit criteria:

- A user can replay an incident and see prior similar episodes with evidence.

### Phase 1: Shadow Recommender

Goal: recommend actions without executing them.

- Add hybrid retrieval scoring.
- Add action ranking based on prior outcomes.
- Add confidence, uncertainty, and missing-evidence output.
- Compare recommendations with historical human actions.

Exit criteria:

- Recommendations beat text-only/vector-only baselines on top-k action match and
  evidence precision.

### Phase 2: Advisory Copilot

Goal: put recommendations in front of humans.

- Generate evidence packs for live or replayed incidents.
- Add approval workflow hooks.
- Add runbook links and rollback plans.
- Record human accept/reject decisions.

Exit criteria:

- Human operators accept recommendations at a useful rate and report better
  triage speed.

### Phase 3: Guarded Execution

Goal: execute only narrow reversible actions.

- Implement policy engine and allowlisted tools.
- Execute in lab first.
- Add canary and rollback.
- Store action result and recovery curve.

Exit criteria:

- The engine safely resolves a small set of recurring lab incidents with lower
  MTTR than human/manual baselines.

### Phase 4: Edge And Physical AI Extension

Goal: reuse the same primitive outside cloud AIOps.

- Store robot/device state-action-outcome windows.
- Add local recorder mode.
- Generate compact edge incident signatures.
- Sync learned outcomes to fleet/global memory.

Exit criteria:

- A robot or edge gateway can replay a pre/post anomaly window and retrieve
  prior action outcomes from similar episodes.

## ZeptoDB-Specific Architecture

Recommended architecture:

```text
OTLP / Prometheus / logs / traces / PagerDuty / deploy events
        |
        v
ZeptoDB time-series tables
        |
        +--> incident window builder
        +--> topology/change joiner
        +--> action/outcome writer
        |
        v
Action-Outcome Memory Engine
        |
        +--> hybrid retrieval
        +--> evidence pack generator
        +--> policy gate
        +--> optional executor
        |
        v
Agent Memory
        |
        +--> remediation_outcome memories
        +--> failure_signature memories
        +--> runbook_delta memories
```

Near-term implementation should use existing Agent Memory APIs for durable
lessons and ordinary ZeptoDB tables for operational facts. Avoid adding a new
storage subsystem until the data model proves useful.

## What Not To Do

- Do not build a generic vector memory product.
- Do not let an LLM execute arbitrary production commands.
- Do not summarize incidents without keeping evidence links.
- Do not treat successful outcomes as the only useful memories.
- Do not ignore event-time versus ingestion-time differences.
- Do not begin with broad autonomous remediation.

## Recommended First Research Artifact

Build a replay-only prototype:

**ActionOutcomeReplay**

Input:

- current incident id or synthetic incident fixture,
- current alert and service,
- time window,
- optional candidate action.

Output:

- similar prior episodes,
- prior actions,
- recovery curves,
- likely safe actions,
- likely unsafe actions,
- evidence pack,
- confidence and missing evidence.

This gives a strong research result without risking live production systems.

## Sources

- AIOpsLab:
  <https://microsoft.github.io/AIOpsLab/>
- AIOpsLab paper:
  <https://arxiv.org/abs/2501.06706>
- Autonomous Incident Resolution at Hyperscale:
  <https://arxiv.org/html/2606.09122v1>
- AIOps "AI Oops" safety analysis:
  <https://arxiv.org/html/2508.06394v2>
- OpenTelemetry GenAI observability:
  <https://opentelemetry.io/blog/2026/genai-observability/>
- Datadog OTel GenAI semantic convention support:
  <https://www.datadoghq.com/blog/llm-otel-semantic-convention/>
- Reflexion:
  <https://arxiv.org/abs/2303.11366>
- Experiential Reflective Learning:
  <https://arxiv.org/html/2603.24639v1>
- Voyager:
  <https://arxiv.org/abs/2305.16291>
- Physical Intelligence MEM:
  <https://www.pi.website/research/memory>
