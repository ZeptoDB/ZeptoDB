# Action-Outcome Industry Research Scan

Date: 2026-06-18
Branch: `codex/aiops-time-series-memory-research`

## Question

What industry and research work exists near ZeptoDB's Action-Outcome Memory
direction?

## Short Answer

The market rarely uses the exact phrase "Action-Outcome Memory." The same idea
appears under several adjacent names:

- next-best-action recommendation,
- remediation recommendation,
- closed-loop remediation,
- runbook automation,
- autonomous AIOps agents,
- experiential agent learning,
- reinforcement learning for operations.

The direction is real and commercially validated, but most public material
stops at one of these layers:

```text
similar incident -> suggested action
alert -> predefined remediation workflow
RCA -> best-action plan
agent trial -> reflection memory
state -> RL action -> scalar reward
```

ZeptoDB's research should keep a sharper claim:

```text
queryable time-series action episode
  = pre-action state
  + action/policy
  + post-action observation window
  + outcome label
  + replayable future guardrail
```

## Industry And Research Clusters

### 1. AIOps Incident Action Recommendation

This is the closest commercial category.

IBM describes AIOps incident management as correlating events and fault
localization signals, then finding similar historical incidents for action
recommendation. IBM Cloud Pak for AIOps also exposes "Next Best Action"
recommendations from past incidents and built-in runbook automation.

BMC Helix positions "actionable recommendations for governed remediation":
correlate events, logs, changes, and topology to identify root causes,
recommend fixes, and execute remediations through pre-approved automations,
policy workflows, or self-healing actions.

PagerDuty and incident.io represent the adjacent incident automation category:
predefined remediation actions, validated workflows, human-in-the-loop
execution, action timelines, and audit-ready incident records.

Sources:

- IBM log data for AIOps incident management:
  https://www.ibm.com/think/topics/logs-for-incident-management
- IBM Cloud Pak for AIOps overview:
  https://www.ibm.com/docs/en/cloud-paks/cloud-pak-aiops/4.13.1?topic=overview
- BMC Helix Operations Management with AIOps:
  https://www.helixops.ai/products/bmc-helix-operations-management.html
- PagerDuty incident response automation:
  https://www.pagerduty.com/blog/automation/from-alert-to-resolution-how-incident-response-automation-cuts-mttr-and-closes-gaps/
- incident.io runbook automation landscape:
  https://incident.io/blog/runbook-automation-tools-2026-the-complete-guide

Interpretation:

The market agrees that AIOps must move from insight to action. The public gap is
not "can we recommend an action?" It is whether the system stores action
episodes as replayable, time-series-native, user-owned learning data.

### 2. Closed-Loop Remediation

Dynatrace defines closed-loop remediation as extending automated remediation
with automated observation of the executed action's result, then using observed
impact to automate next remediation actions until resolution.

This is highly aligned with Action-Outcome Memory. It validates the loop:

```text
remediation action -> observe result -> choose next action
```

Source:

- Dynatrace closed-loop remediation:
  https://www.dynatrace.com/knowledge-base/closed-loop-remediation/

Interpretation:

This is the strongest evidence that "outcome after action" is becoming an
industry concept. ZeptoDB should not claim the loop itself is novel. The novel
angle should be the data substrate: a SQL/time-series action-outcome memory
engine with replay, ablation, context gates, and user-owned persistence.

### 3. Autonomous AIOps Agent Benchmarks

Microsoft AIOpsLab is a major research signal. It argues that current AIOps
agent research lacks standard metrics, taxonomies, and realistic dynamic
benchmarks. AIOpsLab deploys microservice environments, injects faults,
generates workloads, exports telemetry, and provides an agent-cloud interface
for evaluating autonomous AIOps agents.

Sources:

- Microsoft Research AIOpsLab blog:
  https://www.microsoft.com/en-us/research/blog/aiopslab-building-ai-agents-for-autonomous-clouds/
- AIOpsLab project:
  https://microsoft.github.io/AIOpsLab/

Interpretation:

AIOpsLab is benchmark infrastructure, not an action-outcome memory database.
It is a good future comparison/evaluation target for ZeptoDB. Our experiments
should eventually export AIOpsLab-style tasks into ZeptoDB action episodes.

### 4. AIOps Literature And Taxonomy

The AIOps incident-management literature describes a broad pipeline:

```text
detect / predict incidents
-> identify root causes
-> automate healing actions
```

It also notes that AIOps is still decentralized and lacks standardized
frameworks for data management, target problems, implementation details,
requirements, and capabilities.

Source:

- AIOps Solutions for Incident Management: Technical Guidelines and
  Comprehensive Literature Review:
  https://arxiv.org/html/2404.01363v1

Interpretation:

This supports our need for explicit schemas, replay harnesses, and benchmark
baselines. It also means a polished Action-Outcome schema could become a useful
research contribution, not only a product feature.

### 5. Agent Experiential Learning

Outside AIOps, LLM-agent research already treats action outcomes as memory.

Reflexion converts task feedback into verbal reflections and stores them in an
episodic memory buffer to improve later decisions without model fine-tuning.

ExpeL autonomously gathers success and failure experiences, extracts reusable
insights, and recalls past successful trajectories at inference time.

Voyager stores executable skills in a growing skill library and improves via
environment feedback, execution errors, and self-verification.

Sources:

- Reflexion:
  https://arxiv.org/abs/2303.11366
- ExpeL:
  https://arxiv.org/html/2308.10144v2
- Voyager:
  https://arxiv.org/abs/2305.16291

Interpretation:

These papers validate the general agent-learning primitive:

```text
trial -> feedback/outcome -> memory -> improved future action
```

Their gap for our purpose is operational grounding. They usually do not model
service topology, incident windows, deploy context, approval policy, rollback
plans, and post-action recovery curves as a structured time-series database.

### 6. Reinforcement Learning For Operations

Reinforcement learning is the classic action-outcome framework: state, action,
reward, policy update. In cloud operations, it appears most concretely in
autoscaling and resource allocation. IBM Research, for example, presents
RL-based autoscaling as a way to overcome fixed HPA parameters, handle sudden
load spikes, and support custom parameters.

Source:

- IBM Research RL autoscaling:
  https://research.ibm.com/publications/optimizing-cloud-workloads-autoscaling-with-reinforcement-learning

Interpretation:

RL gives a formal foundation, but production incident remediation is harder than
autoscaling:

- rewards are delayed and noisy,
- unsafe exploration is unacceptable,
- human approvals matter,
- one incident can include multiple coupled actions,
- failures must be remembered, not averaged away.

For ZeptoDB, the near-term path should be replay/shadow recommendation and
context gating, not direct online RL in production.

## Where ZeptoDB Can Be Different

### Already Validated By Industry

These are not unique enough by themselves:

- anomaly detection,
- RCA,
- "recommend a fix",
- runbook automation,
- autonomous remediation as a broad concept,
- learning from past incidents in generic text form.

### Defensible Innovation Area

ZeptoDB should focus on the part that current public products and papers do not
make explicit enough:

1. **Action episodes as first-class database rows**
   - Store state, policy, action, recovery, outcome, and reflection.

2. **Pre/post time-series windows**
   - Treat the post-action recovery curve as part of memory, not as a comment
     in a ticket.

3. **Context-conditioned suppression**
   - Do not only recommend actions that worked before.
   - Suppress actions whose prior success/failure occurred under mismatched
     topology, deploy, service, or risk context.

4. **Replayable SQL evaluation**
   - Re-run the exact historical action recommendation.
   - Join recommendations to outcomes.
   - Audit ranking with window functions.
   - Compare baselines.

5. **User-owned memory substrate**
   - Datadog/Dynatrace/IBM/BMC intelligence is platform-native.
   - ZeptoDB can be positioned as private action memory beneath or beside those
     platforms.

## Research Positioning

Bad positioning:

> We do AIOps action recommendation.

Better positioning:

> We build a time-series Action-Outcome Memory layer for AIOps agents: a
> private, replayable database of which operational actions worked, failed, or
> should be suppressed under specific temporal and policy contexts.

## Recommended Next Research Step

Turn the scan into an evaluation matrix:

| Baseline | What it represents | Expected weakness |
| --- | --- | --- |
| Similar-incident retrieval | IBM-style historical incident action recommendation | Can recommend superficially similar but unsafe actions. |
| Runbook-only automation | PagerDuty/incident.io/BMC-style predefined workflow | Does not learn whether an action worked under current conditions. |
| Closed-loop remediation | Dynatrace-style observe-and-act loop | May not expose user-owned replayable memory or ablation controls. |
| Reflexion/ExpeL-style memory | Agent self-reflection from outcomes | Lacks structured operational time-series windows and policy schema. |
| Context-gated Action-Outcome Memory | ZeptoDB research target | Must prove it improves safety and decision quality under noisy distractors. |

The next experiment should therefore compare:

```text
similar incident only
vs. runbook/action prior only
vs. reflection memory only
vs. context-gated action-outcome memory
```

on the same AIOps replay fixture.

Status: complete in Experiment 010
(`docs/research/action_outcome_vendor_baseline_experiment_010.md`). The
context-gated Action-Outcome variant preserved Top-3 hit rate at 1.00 and
improved failed-action avoidance to 1.00 while recording 21 context suppressions.
