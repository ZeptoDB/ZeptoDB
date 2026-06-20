# AIOps Time-Series Industry Scan

Date: 2026-06-18
Branch: `codex/aiops-time-series-memory-research`

## Question

How do current AIOps and observability products use time-series data, and what
is the defensible innovation point for ZeptoDB's Action-Outcome Memory work?

## Short Answer

Current AIOps products use time-series data mainly to detect, forecast,
correlate, and explain operational anomalies:

```text
telemetry time series -> anomaly / forecast / correlation -> investigation / RCA
```

ZeptoDB's opening should be one step later in the loop:

```text
telemetry time series
  -> action taken
  -> post-action observation window
  -> outcome label
  -> reusable action policy memory
```

The innovation point is not "better anomaly detection." The stronger claim is
**state-action-outcome memory for operations**: a queryable, replayable,
policy-aware memory of what actions worked or failed under which temporal
conditions.

## How Vendors Use Time-Series Today

| Vendor | Publicly visible time-series use | What it is good at | Product boundary for our research |
| --- | --- | --- | --- |
| Datadog Watchdog | Continuously monitors observability data and surfaces alerts, insights, and root-cause analyses. Anomaly monitors use historical data to predict expected metric bounds. | Noise reduction, anomaly detection, cross-signal investigation. | Public materials emphasize detection/investigation, not an open action-outcome episode store that users can replay as a policy memory. |
| Datadog Bits AI SRE / Bits Investigation | Expands metric query time ranges, correlates logs/metrics/infrastructure state, creates hypothesis trees, exposes agent traces, and performs autonomous alert investigations. | Agentic investigation over broad telemetry. | Very close strategically. The differentiator must be user-owned, structured action-outcome memory and replay/evaluation, not generic AI investigation. |
| Dynatrace Davis / Dynatrace Intelligence | Uses causal topology and captured/ingested information to rank root-cause contributors and combine connected anomalies into a single problem. | Causal RCA, dependency graph, alert compression. | Strong at "why did this happen?" Less public emphasis on "what should we do next based on prior action outcomes?" |
| New Relic anomaly detection | Uses streaming alerting, sensitivity thresholds, and automatic or configured seasonality for any entity/signal. | Adaptive alerts and seasonal anomaly detection. | Primarily alerting and anomaly configuration, not closed-loop action memory. |
| Splunk ITSI | Applies anomaly detection to KPIs, generates notable events, and lets responders deep-dive over a time range with related KPIs. | Service health, KPI anomaly triage, episode review. | Operational workflow is strong, but action-outcome learning is not the central primitive in public docs. |
| Grafana Cloud ML | Forecasts time-series metrics, displays uncertainty bands, and supports dynamic alerting when metrics are predicted to exceed limits. | Forecasting and adaptive alerting for Prometheus-style metric workflows. | Useful signal layer, but not an action/outcome memory layer. |
| Elastic AIOps | Uses ML for log rate anomaly detection, log pattern analysis, forecasting, confidence intervals, and latency/error/failure correlation. | Log/time-series anomaly and pattern analysis. | Focuses on finding anomalies and likely causes; less on policy-gated action reuse. |

Sources reviewed:

- Datadog Watchdog docs: https://docs.datadoghq.com/watchdog/
- Datadog anomaly monitor docs: https://docs.datadoghq.com/monitors/types/anomaly/
- Datadog Bits AI SRE update: https://www.datadoghq.com/blog/bits-ai-sre-deeper-reasoning/
- Datadog Bits Investigation: https://www.datadoghq.com/product/ai/bits-investigation/
- Dynatrace root-cause analysis docs: https://docs.dynatrace.com/docs/dynatrace-intelligence/root-cause-analysis
- Dynatrace Intelligence: https://www.dynatrace.com/platform/artificial-intelligence/
- New Relic anomaly detection docs: https://docs.newrelic.com/docs/alerts/create-alert/set-thresholds/anomaly-detection/
- Splunk ITSI KPI anomaly detection docs: https://help.splunk.com/en/splunk-it-service-intelligence/splunk-it-service-intelligence/visualize-and-assess-service-health/4.19/kpi-alerting/apply-anomaly-detection-to-a-kpi-in-itsi
- Grafana Cloud forecasting docs: https://grafana.com/docs/grafana-cloud/machine-learning/dynamic-alerting/forecasting/
- Elastic AIOps overview: https://www.elastic.co/observability/aiops
- AIOps time-series anomaly survey: https://arxiv.org/abs/2308.00393

## Common AIOps Time-Series Pattern

Across vendors, time-series appears in seven recurring ways.

1. Anomaly detection
   - Learn normal bands from historical metrics.
   - Detect values outside seasonal or adaptive ranges.
   - Examples: Datadog anomaly monitors, New Relic anomaly detection, Elastic
     ML anomaly jobs, Splunk ITSI KPI anomalies.

2. Forecasting
   - Predict future capacity, utilization, traffic, or active series growth.
   - Alert before a limit is crossed.
   - Examples: Grafana Cloud metric forecasts, Elastic predictive analysis.

3. Alert noise reduction
   - Combine related time-series anomalies into fewer incidents.
   - Use duration windows, recovery windows, entity cohesion, and dependency
     context to avoid one alert per symptom.
   - Examples: Dynatrace combines connected anomalies; Splunk generates notable
     events; Datadog Watchdog surfaces signals that matter.

4. Root-cause analysis
   - Rank anomalous contributors.
   - Correlate metrics, logs, traces, topology, deploys, and infrastructure
     state over the incident window.
   - Examples: Dynatrace causal topology, Datadog Bits AI SRE hypothesis trees,
     Elastic failure correlation.

5. Change impact analysis
   - Align anomalies with deploys, config changes, feature flags, or infra
     events.
   - Datadog Bits AI SRE publicly highlights expanded data sources including
     changes, source code, events, RUM, DB monitoring, network path, and
     profiler data.

6. Investigation traceability
   - Expose what the AI queried and how hypotheses were formed.
   - Datadog's Agent Trace view is important here because regulated customers
     need to inspect the reasoning path.

7. Human workflow routing
   - Create incident tickets, notify Slack/Teams, route to teams, or open deep
     dives.
   - This is the bridge from insight to operational action, but it is usually
     still not a structured memory of action outcomes.

## Gap We Can Own

The current market is strong at this:

```text
What changed?
What is anomalous?
Which entity is probably responsible?
Which evidence explains the incident?
```

Our research should own this:

```text
Given this state and policy envelope, which action should be tried,
which action should be blocked, and why did prior attempts succeed or fail?
```

That means the unit of memory should not be a metric anomaly, log cluster, or
incident summary. It should be an **action episode**:

```text
pre-action state window
  + topology/change context
  + policy decision
  + action parameters
  + post-action recovery window
  + outcome label
  + future guardrail
```

## Recommended Innovation Claim

Use this wording:

> ZeptoDB turns observability time-series into Action-Outcome Memory: a
> replayable record of which remediation actions worked, failed, or should be
> suppressed under specific temporal, topology, change, and policy conditions.

Avoid this weaker wording:

> ZeptoDB does anomaly detection for AIOps.

That second claim puts us directly against Datadog, Dynatrace, New Relic,
Splunk, Grafana, and Elastic on their strongest ground. The first claim moves
the comparison from "detecting incidents" to "learning safe operational action."

## Differentiation Pillars

### 1. State-Action-Outcome, Not Signal-Only AIOps

Most products start from signals and produce insight. ZeptoDB should start from
signals and remember the result of actions.

Research artifact already built:

- `action_outcome_episodes`
- `action_outcome_episode_metrics`
- `action_outcome_replay_recommendations`
- `action_outcome_gate_suppressions`
- `action_outcome_retrieval_quality_labels`

### 2. Time-Series Native Outcome Windows

The post-action observation window matters as much as the incident window.

This gives ZeptoDB a natural advantage because the engine can query:

- pre-action metric trajectory,
- action timestamp,
- post-action recovery curve,
- recurrence window,
- delayed side effects,
- rollback events.

### 3. Context Gate For Safety

Experiment 005 showed the most important algorithmic result so far:

| Variant | Top-3 Hit Rate | Failed-Action Avoidance | Gate Suppressions |
| --- | ---: | ---: | ---: |
| full_guarded | 1.00 | 0.67 | 0 |
| context_gated | 1.00 | 1.00 | 21 |

The message is: past success is dangerous if current context differs. The
memory engine must suppress stale or mismatched action memories.

### 4. Queryable Replay, Not Black-Box Memory

Action-Outcome Memory should be SQL-checkable:

- Load the exact historical seed.
- Replay the recommendation.
- Join recommendations with outcomes.
- Use window functions to audit rank transitions.
- Compare to JSON/control baselines.

Experiment 009 now validates:

- seed load: 203/203 statements,
- native string window: pass,
- native string JOIN: pass,
- numeric JOIN/window acceptance: pass.

### 5. User-Owned Operational Memory

Large vendors have platform-level intelligence and broad proprietary telemetry.
ZeptoDB can position as the customer's local, edge-capable, or private
operational memory:

- customer-owned action/outcome history,
- explainable SQL replay,
- low-latency local decision support,
- suitable for edge AIOps and later physical AI,
- not dependent on sending every operational episode to a SaaS model.

## Strongest Product Wedge

The first sellable wedge is not "autonomous remediation." It is:

> Shadow-mode Action-Outcome Memory for SRE teams.

Workflow:

1. Ingest incident timelines, metrics, logs, traces, deploy events, and human
   actions.
2. Store every remediation action with pre/post windows and outcome labels.
3. During new incidents, retrieve similar action episodes.
4. Recommend or suppress actions with evidence.
5. Let humans accept/reject.
6. Feed the outcome back into memory.

This can be sold as:

- on-call copilot memory,
- incident replay/evaluation database,
- remediation recommendation audit layer,
- postmortem-to-policy engine,
- private AIOps memory substrate.

## Where Physical AI Still Fits

Physical AI uses the same primitive:

```text
robot state -> action -> sensor trajectory -> success/failure -> policy update
```

But AIOps should come first commercially because:

- incident telemetry is already time-series,
- outcomes can be labeled from tickets/postmortems,
- the first actions can be reversible and policy-bounded,
- buyers already pay for observability and reliability tooling.

Physical AI becomes the second story after the primitive is proven:

```text
AIOps: service state -> remediation action -> recovery outcome
Physical AI: world state -> robot action -> task outcome
```

## Next Research Steps

1. Convert this industry scan into a vendor-inspired benchmark plan.
   - Status: complete in Experiment 010.
   - Result: `context_gated_action_outcome` preserved Top-3 hit rate at 1.00
     and improved failed-action avoidance to 1.00 versus 0.67-0.83 for the
     vendor-inspired baselines.
2. Add alias-aware hash JOIN `WHERE` predicates so Experiment 009 can validate
   top-action semantics directly on native Action-Outcome JOINs.
3. Add a shadow-mode product mock:
   - "recommended action",
   - "suppressed action",
   - "matching prior episodes",
   - "post-action outcome feedback."
4. Design a public demo around one reversible AIOps loop:
   - deploy regression,
   - latency spike,
   - candidate actions,
   - context gate,
   - human approval,
   - outcome writeback.
