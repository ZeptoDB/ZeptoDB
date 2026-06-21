# AIOps Time-Series Memory Research Data

Date: 2026-06-13
Branch: `codex/aiops-time-series-memory-research`

This file records the comparison data behind the AIOps research direction in
`docs/research/time_series_agent_memory_edge.md`. It is intentionally structured
as a research data log rather than a narrative memo.

Action-outcome engine procedure:
`docs/research/action_outcome_memory_engine_plan.md`

## Research Question

Can a time-series-native incident memory layer improve AIOps agents compared with
text-only incident RAG or dashboard-centric observability?

Primary hypothesis:

> Event-time-aligned incident memory that combines metrics, logs, traces,
> topology, deploy events, postmortems, and action outcomes will improve similar
> incident retrieval, root-cause ranking, and runbook recommendation for AIOps
> agents.

Commercial hypothesis:

> The first useful product is read-only Similar Incident Retrieval plus Evidence
> Pack APIs for AI SRE workflows. Guarded remediation can come later after trust,
> policy, and audit controls are proven.

## Market Data

Market estimates vary widely because reports define "AIOps" differently. This is
useful signal: the category is active, but product positioning must be precise.

| Source | Segment | Base Value | Forecast Value | CAGR | Notes | URL |
| --- | --- | ---: | ---: | ---: | --- | --- |
| Mordor Intelligence | AIOps | USD 18.95B in 2026 | USD 37.79B in 2031 | 14.8% | Mentions ML correlation engines and lower MTTR as demand drivers. | <https://www.mordorintelligence.com/industry-reports/aiops-market> |
| Fortune Business Insights | AIOps | USD 2.23B in 2025; USD 2.67B in 2026 | USD 11.8B in 2034 | 20.40% | Much smaller definition than Mordor; still shows strong growth. | <https://www.fortunebusinessinsights.com/aiops-market-109984> |
| Future Market Insights | AIOps platform | USD 15.8B in 2025; USD 19.8B by 2026-end | USD 187.2B in 2036 | 25.2% | Aggressive long-range estimate; use as upside case only. | <https://www.futuremarketinsights.com/reports/aiops-platform-market> |
| Market Research Future | AIOps platform | USD 10.52B in 2024; USD 12.43B in 2025 | USD 66.2B in 2035 | 18.2% | Mid-range AIOps platform estimate. | <https://www.marketresearchfuture.com/reports/aiops-platform-market-11745> |
| Coherent Market Insights | AIOps platform | USD 14.69B in 2026 | USD 68.88B in 2033 | 24.7% | Supports a high-growth platform view. | <https://www.coherentmarketinsights.com/market-insight/aiops-platform-market-2073> |
| MarketsandMarkets | AIOps platform | USD 11.7B in 2023 | USD 32.4B in 2028 | 22.7% | Older but widely cited platform estimate. | <https://www.marketsandmarkets.com/Market-Reports/aiops-platform-market-105974848.html> |
| Mordor Intelligence | Autonomous IT operations | USD 17.28B in 2026 | USD 38.34B in 2031 | 17.28% | Adjacent category directly tied to self-healing workflows. | <https://www.mordorintelligence.com/industry-reports/autonomous-it-operations-market> |
| Mordor Intelligence | Observability | USD 3.35B in 2026 | USD 6.93B in 2031 | 15.62% | Mentions AI-driven and edge-centric workloads as demand catalysts. | <https://www.mordorintelligence.com/industry-reports/observability-market> |
| Credence Research | Incident management software | USD 7.215B in 2024 | USD 15.579B in 2032 | 10.1% | Adjacent buyer budget for incident workflows. | <https://www.credenceresearch.com/report/incidence-management-software-market> |
| MarketsandMarkets | Predictive maintenance | USD 13.89B in 2026 | USD 23.79B in 2031 | 11.4% | Relevant for industrial AIOps and physical AI edge expansion. | <https://www.marketsandmarkets.com/Market-Reports/operational-predictive-maintenance-market-8656856.html> |

## Product Comparison

The market is moving toward AI SRE agents, agentic observability, and automated
incident workflows. The gap for ZeptoDB is a self-hosted or edge-deployable
time-series memory plane that can feed these agents with structured evidence.

| Vendor | Product / Capability | Stated Capabilities | Data Modalities | Action / Automation | Evidence / Audit Signal | ZeptoDB Opportunity | URL |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Datadog | Bits Investigation | AI SRE agent grounded in thousands of incidents; claims root-cause identification 90% faster. | Datadog telemetry and incident data. | Troubleshooting and root-cause investigation. | Product page emphasizes root-cause speed. | Customers may want customer-owned incident memory and exportable evidence packs. | <https://www.datadoghq.com/product/ai/bits-investigation/> |
| Datadog | Bits AI SRE | Autonomous alert investigations, synthetic API test investigations, APM graph-triggered investigations, recommended actions, Slack/Jira triage. | Metrics, logs, traces, dashboards, changes; later blog adds source code, RUM, DB monitoring, network path, profiler, events. | Recommended actions, ticket/chat actions, code-fix handoff through Bits AI Dev Agent. | Agent Trace view shows tools called, data queried, and intermediate analysis. | ZeptoDB can compete below the UI: long-term incident memory, third-party telemetry, edge/site deployments. | <https://www.datadoghq.com/blog/bits-ai-sre/> |
| Datadog | Bits AI SRE deeper reasoning update | Claims about 2x faster investigations, roughly 3-4 minutes depending on complexity; expanded data sources; third-party integrations in preview. | Full-stack Datadog plus selected third-party sources. | Triage, assignment, automation integrations. | Agent Trace and hypothesis tree. | A ZeptoDB memory layer should expose comparable agent traces and evidence provenance. | <https://www.datadoghq.com/blog/bits-ai-sre-deeper-reasoning/> |
| Dynatrace | Dynatrace Intelligence | Fuses deterministic insights with agentic action for autonomous prevention, remediation, and optimization. | Dynatrace observability and platform data. | Autonomous prevention, remediation, optimization. | Deterministic AI plus policies and guardrails. | ZeptoDB should not claim pure LLM RCA; it should combine deterministic temporal evidence with agent reasoning. | <https://www.dynatrace.com/platform/artificial-intelligence/> |
| Dynatrace | Agentic Dynatrace Assist | Uses skills, reference files, and MCP tools to query Dynatrace environment; user permissions scope results. | Dynatrace platform data and official support resources. | Can call MCP tools subject to permissions. | User-based access, permissions, PII prompt blocking behavior. | ZeptoDB needs scoped tool access, tenant isolation, and audit logs for agent queries. | <https://docs.dynatrace.com/docs/dynatrace-intelligence/agentic-and-generative-ai/agentic-and-generative-ai-getting-started> |
| Splunk | Observability Troubleshooting Agent | Automatically correlates metrics, events, logs, and traces; surfaces likely root causes and next steps. | Metrics, events, logs, traces. | Root-cause support and next-step recommendations. | Built into Observability Cloud alert workflows. | ZeptoDB can target teams that need portable/self-hosted incident memory across Splunk and non-Splunk data. | <https://www.splunk.com/en_us/blog/observability/splunk-observability-ai-agent-monitoring-innovations.html> |
| Splunk | AI troubleshooting agent | Automatically triggers RCA for selected APM service and Kubernetes infrastructure alerts. | APM services and Kubernetes infrastructure monitoring. | Suspected root-cause display and remediation plan. | Alert-linked RCA flow. | ZeptoDB can start with Kubernetes + OpenTelemetry incident memory as a focused MVP. | <https://help.splunk.com/en/splunk-observability-cloud/create-alerts-detectors-and-service-level-objectives/create-alerts-and-detectors/ai-troubleshooting-agent-and-remediation-plan> |
| PagerDuty | AIOps | Reduces alert noise, improves incident visibility, triage, and removes repetitive work. | Alerts, incident events, change events, historical incidents. | Event grouping, triage, workflow automation. | Incident lifecycle context. | PagerDuty is a strong integration source for incident labels, timelines, and action outcomes. | <https://www.pagerduty.com/platform/aiops/> |
| PagerDuty | Event Intelligence | Claims filtering up to 98% of noise; surfaces relevant incidents, recent changes, and likely origin points. | Alerts, related incidents, recent changes. | Alert grouping and origin suggestions. | "What happened, when" situational awareness. | ZeptoDB should store the richer raw windows behind PagerDuty events. | <https://www.pagerduty.com/platform/aiops/event-intelligence/> |
| PagerDuty | SRE Agent | Spring 2026 release says PagerDuty SRE Agent investigates and resolves complex incidents at enterprise scale. | PagerDuty Operations Cloud context. | Investigation and resolution. | Enterprise incident workflow context. | ZeptoDB can provide the time-series evidence layer that incident platforms can call. | <https://www.pagerduty.com/newsroom/pagerduty-operations-cloud-spring-2026-release/> |
| Grafana | Assistant Investigations | Uses specialized agents over metrics, logs, traces, and profiles; internal case found root cause in 8 minutes vs 28 minutes manually. | Metrics, logs, traces, profiles, query plans, MCP integrations. | Recommendations for mitigation/remediation. | Evidence trail, hypotheses, confidence scores. | This validates a multi-agent evidence-pack design; ZeptoDB can supply the memory backend. | <https://grafana.com/blog/a-tale-of-two-incident-responses-how-our-ai-assist-helped-us-find-the-cause-3-5x-faster/> |
| Grafana | AI-powered Observability | Root-cause analysis, dashboard creation, query suggestions, SRE agent, cost optimization, AI Observability preview. | Grafana Cloud observability data and AI app telemetry. | Troubleshooting and workflow assistance. | Grafana Assistant embedded workflows. | ZeptoDB should integrate through OpenTelemetry/Grafana rather than require UI replacement. | <https://grafana.com/products/cloud/ai-observability/> |
| ServiceNow | ITOM AIOps agentic workflows | Network of AIOps agents gathers real-time data from other tools and presents alert/incident context conversationally. | Alerts, incidents, external tool data. | Agentic workflows for incident/outage response. | Platform governance context. | ServiceNow is a workflow and ticket system integration for action outcomes and postmortems. | <https://www.servicenow.com/community/itom-blog/revolutionizing-it-operations-with-ai-agents/ba-p/3271551> |
| OpenTelemetry | AI agent observability conventions | Standardized traces, metrics, and logs for AI agent frameworks; GenAI semantic conventions. | Agent traces, model calls, token usage, latency, logs, metrics. | Telemetry standard, not an agent product. | Vendor-neutral instrumentation. | ZeptoDB should ingest OTLP and preserve agent traces as first-class incident evidence. | <https://opentelemetry.io/blog/2025/ai-agent-observability/> |

## Benchmark And Paper Data

| Source | Type | Data / Environment | Task | Reported Scale | Why It Matters For ZeptoDB | URL |
| --- | --- | --- | --- | --- | --- | --- |
| AIOpsLab | Benchmark framework | Deploys microservice cloud environments, injects faults, generates workloads, exports telemetry, and exposes agent interfaces. | End-to-end AIOps agent evaluation across incident lifecycle. | Framework, not just static dataset. | Best fit for evaluating agentic workflows with ZeptoDB as memory backend. | <https://arxiv.org/abs/2501.06706> |
| RCAEval | Open-source benchmark | Multiple microservice RCA datasets and baselines. | Root-cause analysis. | 9 datasets, 735 real failure cases, 15 reproducible baselines. | Useful baseline suite for comparing time-series memory retrieval against existing RCA methods. | <https://github.com/phamquiluan/RCAEval> |
| OpenRCA | Benchmark dataset | Real software operating scenarios with telemetry. | LLM root-cause analysis. | 335 failure cases, 3 heterogeneous software systems, over 68 GB telemetry. | Direct benchmark for testing whether LLMs need structured time-series evidence. | <https://netman.aiops.org/wp-content/uploads/2025/05/13411_OpenRCA_Can_Large_Langua.pdf> |
| CCF AIOPS 2025 RCA Challenge dataset | Challenge dataset | Extended from Google HipsterShop microservice system; includes trace, metric, and log data. | Root-cause analysis. | Public challenge dataset referenced by 2026 RCA paper. | Good for controlled experiments over metrics/logs/traces. | <https://arxiv.org/html/2602.08804v1> |
| Survey of AIOps in the Era of Large Language Models | Survey | Literature map for LLM-based incident management, log analysis, RCA, mitigation, postmortems, and QA. | Survey / taxonomy. | Broad paper survey. | Helps position this research against text-only incident reports and LLM-only RCA. | <https://arxiv.org/html/2507.12472v1> |
| Auditable Graph-Guided RCA for Kubernetes | Method paper | Kubernetes incidents, graph traversal agent, graph-guided RCA. | Auditable RCA. | 2026 paper. | Supports combining LLMs with graph/time-series tools instead of free-form diagnosis. | <https://arxiv.org/html/2606.08590v1> |
| AIOps "AI Oops" | Safety/security paper | LLM-driven IT operations and RCA attack surface. | Subversion and trust risks. | 2025 paper. | Reinforces need for scoped tools, evidence packs, audit trails, and safe action gates. | <https://arxiv.org/html/2508.06394v2> |
| TrioXpert | Automated incident management framework | Incident detection, root-cause localization, mitigation, and post-incident stages. | Incident lifecycle automation. | 2025 paper. | Useful lifecycle model for mapping memory objects to actions and outcomes. | <https://arxiv.org/html/2506.10043v1> |
| Cloud-OpsBench | Benchmark | Reproducible benchmark for agentic root-cause analysis; references AIOpsLab. | Agentic RCA. | 2026 paper. | Candidate evaluation target for interactive agent experiments. | <https://arxiv.org/html/2603.00468v1> |
| Uncovering Reasoning Failures in LLMs for Cloud RCA | Analysis paper | Extracts modality-specific alerts from logs, metrics, and traces into unified alerts. | Reasoning failure analysis. | 2026 paper. | Supports measuring false-cause rate, not only answer accuracy. | <https://arxiv.org/html/2601.22208v1> |

## AIOps Data Modalities To Capture

The research data model must include more than incident text.

| Modality | Required Fields | Why It Matters | ZeptoDB Storage Fit |
| --- | --- | --- | --- |
| Metrics | metric name, labels, entity, timestamp, value, unit, aggregation window | Detect anomalies, trends, saturation, and recovery. | Strong fit as columnar time-series. |
| Logs | timestamp, service, host/pod, severity, template id, message hash, raw reference | Capture error modes and rare events. | Store templates/features in ZeptoDB; keep large raw logs by reference if needed. |
| Traces | trace id, span id, parent id, service, operation, latency, error, timestamp | Localize latency and dependency failures. | Store span summaries and time-window references. |
| Topology | service graph, dependency edges, pod/node/container, region/cluster | Distinguish text-similar but topology-different incidents. | Store as temporal graph snapshots or edge tables. |
| Change events | deploy id, commit, config change, feature flag, migration, timestamp | Many incidents are change-triggered. | Strong fit as event stream joined to telemetry windows. |
| Alerts | alert id, condition, severity, threshold, first seen, resolved time | Incident entry point and evaluation query. | Strong fit as event stream. |
| Runbook actions | action id, actor, command/tool, target, timestamp, approval, rollback | Required for action-outcome memory. | Strong fit as append-only action log. |
| Outcomes | success/failure, time to mitigation, time to resolution, side effects | Required for recommendation ranking. | Strong fit as incident metadata. |
| Postmortems | root cause, contributing factors, lessons, owner, links | Human-labeled ground truth. | Store text embedding refs and link to telemetry. |
| Agent traces | prompt, tool calls, retrieved evidence, decision, action, token/cost, latency | Audit and repeated-mistake reduction. | Store summarized traces and raw references. |

## Comparative Experiment Design

The comparison should test whether time-series incident memory adds value over
current lower-cost baselines.

| Experiment | Baseline A | Baseline B | ZeptoDB Variant | Primary Metrics | Required Data |
| --- | --- | --- | --- | --- | --- |
| Similar incident retrieval | Keyword search over postmortems | Vector search over postmortems | Time-series motif + topology + text hybrid retrieval | top-k recall, MRR, retrieval latency | Incident labels, postmortems, telemetry windows, topology |
| RCA ranking | LLM over alert text only | LLM over alert + logs | LLM with ZeptoDB evidence pack and temporal joins | top-1/top-3 RCA accuracy, false-cause rate, evidence precision | Alerts, metrics, logs, traces, changes, ground truth |
| Runbook recommendation | Static runbook lookup | LLM runbook Q&A | Action-outcome memory ranked by similar incidents | safe-action rate, successful recommendation rate, rollback rate | Actions, outcomes, runbooks, incident labels |
| Postmortem generation | LLM over chat transcript | LLM over ticket and logs | LLM with timeline, evidence pack, action log | edit distance, hallucinated claims, evidence coverage | Incident channel, ticket, telemetry, action log |
| Edge retention | Full cloud retention | Random/sampled retention | Edge anomaly segments + compressed signatures | RCA accuracy per GB, retrieval hit rate, edge storage lifetime | High-frequency telemetry, storage budget, labels |
| Agent audit | No agent trace | Text-only agent transcript | Structured tool-call/event-time audit log | reproducibility, reviewer acceptance, policy violations | Agent calls, prompts, tool outputs, approvals |

## MVP Data Contract

The MVP should produce and consume one core record: `incident_memory`.

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `incident_id` | string | yes | Stable id from PagerDuty/Opsgenie/Jira/internal system. |
| `tenant_id` | string | yes | Required for SaaS or multi-team deployments. |
| `environment` | string | yes | prod/staging/site/plant/robot/fleet. |
| `service` | string | yes | Main affected service or component. |
| `entity_refs` | array | yes | Host, pod, node, device, robot, region, cluster. |
| `start_ts` | timestamp | yes | Earliest suspected incident timestamp. |
| `detect_ts` | timestamp | yes | Alert or human detection time. |
| `mitigate_ts` | timestamp | no | Service restored but root cause may remain. |
| `resolve_ts` | timestamp | no | Incident fully resolved. |
| `event_time_range` | interval | yes | Raw telemetry window by event time. |
| `ingest_time_range` | interval | yes | Raw telemetry window by ingestion time. |
| `clock_domain` | enum/string | yes | Needed for edge, robot, replay, and distributed systems. |
| `symptoms` | array/object | yes | Alerts, metric deviations, log templates, trace anomalies. |
| `topology_context_ref` | reference | no | Service graph or entity graph snapshot. |
| `change_context_ref` | reference | no | Deploy/config/feature-flag/migration context. |
| `anomaly_segment_refs` | array | yes | Pointers into ZeptoDB windows. |
| `embedding_refs` | array | no | Segment, log, postmortem, runbook embeddings. |
| `candidate_causes` | array | no | Ranked hypotheses. |
| `confirmed_root_cause` | string/object | no | Ground truth after postmortem. |
| `actions_taken` | array | no | Human and agent actions. |
| `action_outcomes` | array | no | Success/failure, duration, side effects. |
| `rollback_steps` | array | no | Required for guarded remediation. |
| `human_notes` | reference | no | Postmortem, ticket, chat transcript. |
| `agent_trace_ref` | reference | no | Tool calls, evidence, model outputs. |
| `confidence` | float | no | Model or system confidence. |
| `evidence_score` | float | no | How strongly retrieved evidence supports the claim. |
| `safety_score` | float | no | Action safety / reversibility estimate. |

## ZeptoDB Differentiation Matrix

| Capability | Existing AI SRE Vendors | Generic Vector DB | ZeptoDB Time-Series Memory |
| --- | --- | --- | --- |
| Human-facing dashboards | Strong | Weak | Not the primary wedge. |
| Agent-readable evidence packs | Emerging | Weak | Strong opportunity. |
| Full-fidelity event-time windows | Platform-dependent | Weak | Strong fit. |
| Similar incident retrieval | Emerging | Text-biased | Hybrid time-series/text/topology retrieval. |
| Edge/site deployment | Limited for SaaS products | Possible but not telemetry-native | Strong opportunity for gateway-class edge. |
| Action-outcome memory | Emerging | Weak | Strong if modeled explicitly. |
| Temporal joins across metrics/logs/traces/changes | Platform-dependent | Weak | Core ZeptoDB opportunity. |
| Data residency / self-hosting | Varies | Strong | Strong if packaged properly. |
| Regulated audit trail | Emerging | Weak | Strong if agent_trace and evidence provenance are first-class. |
| Vendor lock-in avoidance | Weak for SaaS platforms | Strong | Strong if OTLP and incident-tool integrations exist. |

## Ranked Use Cases

| Rank | Use Case | Buyer | Urgency | Data Availability | Automation Risk | Revenue Potential | Recommendation |
| ---: | --- | --- | --- | --- | --- | --- | --- |
| 1 | Similar incident retrieval | SRE / platform engineering | High | High | Low | High | Build first. |
| 2 | Evidence pack for RCA agents | SRE / incident command | High | Medium-high | Low-medium | High | Build with retrieval MVP. |
| 3 | Postmortem-to-memory pipeline | SRE / reliability leadership | Medium | Medium | Low | Medium | Build early to create labels. |
| 4 | Runbook recommendation | SRE / operations | High | Medium | Medium | High | Build after action-outcome labels. |
| 5 | Edge AIOps recorder | Industrial / robotics / edge ops | Medium-high | Medium | Low | High in verticals | Build after core MVP proves value. |
| 6 | Guarded remediation | SRE / IT ops | High | Medium | High | Very high | Later; requires policy and audit. |
| 7 | Fully autonomous remediation | CIO / platform leadership | High | Low-medium | Very high | Very high but slow adoption | Research only until trust improves. |

## High-Risk / Gamechanger Bets

This section compares ideas that are riskier than the read-only MVP but could
become category-defining if they work.

| Rank | Bet | Field | Gamechanger Thesis | Why Time-Series Memory Matters | Main Risks | ZeptoDB Advantage | Suggested First Proof |
| ---: | --- | --- | --- | --- | --- | --- | --- |
| 1 | Closed-loop incident autopilot | AIOps | Move from "AI explains incidents" to "AI safely fixes recurring incidents with evidence, rollback, and audit." | The agent needs action-outcome memory: what happened, what changed, what it did, whether it worked, and how the system recovered over time. | Production damage, trust, permissions, liability, bad RCA, unsafe automation. | Low-latency incident windows, append-only action logs, temporal joins, evidence packs, and audit traces. | Guarded remediation for 3-5 reversible incident classes such as restart, scale-out, rollback, cache purge, or traffic drain. |
| 2 | Edge embodied incident memory / robot black box | Physical AI | Become the memory and replay layer for robots, drones, smart factories, and physical AI fleets. | Physical incidents are temporal and multi-modal: sensor streams, robot state, control actions, operator overrides, environment, and recovery. | Hardware fragmentation, safety certification, huge payloads, edge packaging, robotics sales cycles. | Existing edge/ROS 2/physical AI docs, aarch64 path, time-series storage, replay orientation. | Robot-local recorder that stores 30-300 seconds of high-fidelity pre/post anomaly telemetry and produces a compact incident signature. |
| 3 | Agentic SOC temporal memory | Security / AIOps adjacent | Security operations move at machine speed; agents need unified, high-fidelity timelines to investigate and contain attacks. | Attack chains are temporal graphs across identity, network, endpoint, cloud, and agent actions. | Very high safety risk, adversarial manipulation, compliance, high bar for integrations. | Append-only time-series evidence, action audit, scoped retrieval, self-hosted deployment. | Read-only attack timeline reconstruction plus analyst-approved containment recommendation. |
| 4 | Causal remediation simulator | AIOps / Physical AI | Before executing an action, simulate likely blast radius using prior incident memory and system topology. | Counterfactuals need historical temporal neighborhoods, topology, action outcomes, and recovery curves. | Hard causal modeling, false confidence, limited ground truth. | ZeptoDB can store aligned action/outcome trajectories and topology snapshots for retrieval. | For recurring incidents, predict whether restart/rollback/scale-out would reduce error rate within N minutes. |
| 5 | Open incident memory protocol | Cross-domain | Define a portable memory object and API that AI agents can use across Datadog, Splunk, Grafana, PagerDuty, ServiceNow, and edge systems. | The protocol must preserve event time, ingest time, evidence links, agent traces, and outcome labels. | Standards are hard, ecosystem adoption is slow, vendors may resist portability. | ZeptoDB can be a reference implementation with OTLP and incident-tool integrations. | Publish `incident_memory` schema plus import/export adapters for OpenTelemetry and PagerDuty. |

### Gamechanger Ranking Rationale

The best single high-risk bet is **Closed-loop incident autopilot**. It is risky,
but the buyer pain is immediate and the ROI is measurable through MTTR, toil
reduction, and repeated-incident elimination. The research should not start with
free-form autonomous action. It should start with a narrow loop:

1. Detect a recurring incident pattern.
2. Retrieve prior similar incidents and action outcomes.
3. Produce an evidence pack and ranked remediation.
4. Execute only reversible, policy-approved actions.
5. Measure recovery and write the outcome back into memory.

The second best bet is **Edge embodied incident memory / robot black box**. This
has a larger long-term platform upside if physical AI adoption accelerates, but
it has a harder route to market because hardware, safety, and robotics deployment
cycles are slower than cloud/SRE sales cycles.

The unifying insight is that both gamechanger bets need the same primitive:

**time-series action-outcome memory**

This primitive records the temporal situation, the action taken, the policy and
approval context, the observed recovery curve, and the long-term outcome. If
ZeptoDB owns this layer, it can support AIOps first and physical AI later.

### Additional High-Risk Signals

| Source | Signal | Relevance | URL |
| --- | --- | --- | --- |
| TechRadar OpenClaw coverage | Agentic systems can become unmanaged privileged actors; security requires inventory, permissions, and audit. | Supports treating agents as governed identities before autonomous remediation. | <https://www.techradar.com/pro/what-the-openclaw-vulnerability-reveals-about-the-future-of-agentic-ai-security> |
| ITPro / Forrester agentic AI operationalization | Many enterprises are still stuck in pilot because of orchestration, governance, security, and data architecture gaps. | Explains why read-only evidence packs should precede full autonomy. | <https://www.itpro.com/technology/artificial-intelligence/most-enterprises-are-still-unprepared-to-operationalize-it-it-leaders-are-bullish-on-agents-but-keeping-falling-at-the-final-hurdle-heres-why> |
| TechRadar agentic SOC | AI-enabled threats increase the need for machine-speed investigation and response. | Makes agentic SOC a high-upside adjacent market for temporal memory. | <https://www.techradar.com/pro/security-at-machine-speed-why-the-soc-must-be-rebuilt-for-the-ai-era> |
| Elastic agentic AI SOC | Agentic SOCs promise autonomous prioritization, closed-loop containment, and traceable reasoning. | Reinforces action audit and policy-aligned automation as product requirements. | <https://www.elastic.co/security-labs/why-2026-is-the-year-to-upgrade-to-an-agentic-ai-soc> |
| AgentSOC | SOC autonomy must be explainable, risk-aware, and policy-aligned. | Supports graph/time-series evidence over free-form LLM decisions. | <https://arxiv.org/html/2604.20134v1> |
| NVIDIA Jetson Thor | Edge robotics compute now reaches 2070 FP4 TFLOPS and 128 GB memory at 40-130 W. | Makes robot-local memory and inference more plausible on edge-class hardware. | <https://www.nvidia.com/en-us/autonomous-machines/embedded-systems/jetson-thor/> |
| Physical Intelligence MEM | Multi-scale embodied memory enables longer robot tasks and in-context correction. | Supports physical AI memory as a real research frontier, not only infrastructure speculation. | <https://www.pi.website/research/memory> |

## Adoption Constraints

| Constraint | Evidence From Research | Product Implication |
| --- | --- | --- |
| Trust and governance slow autonomous adoption | Dynatrace docs emphasize scoped permissions; market commentary says many agent projects remain pilot-stage due to security and governance. | Start read-only. Add approval gates before actions. |
| AI agents need telemetry, not just chat | Datadog, Splunk, Grafana, and OpenTelemetry all center metrics/logs/traces/tool calls. | Build OTLP and incident integrations before UI polish. |
| RCA requires multi-modal evidence | Benchmarks and product pages repeatedly mention metrics, logs, traces, topology, changes, and events. | `incident_memory` must be structured and temporal. |
| Vendor platforms already own dashboards | Datadog, Dynatrace, Splunk, Grafana have mature UIs. | Position ZeptoDB as memory/evidence layer, not dashboard replacement. |
| Edge deployments need trimmed packages | ZeptoDB docs show edge profiles and resource envelopes, but full image includes heavyweight features. | Create edge AIOps profile later. |
| Ground truth is hard | RCA datasets rely on failure cases and postmortems; real labels are limited. | Build postmortem-to-memory ingestion early. |

## Data Gaps To Resolve

| Gap | Why It Matters | Proposed Next Step |
| --- | --- | --- |
| Real customer incident telemetry | Public benchmarks may not reflect real enterprise complexity. | Start with AIOpsLab/RCAEval/OpenRCA, then collect internal replay data. |
| Precise vendor pricing and attach rates | Needed for business model comparison. | Collect pricing pages and sales estimates separately. |
| ZeptoDB OTLP ingest path | Required for easy AIOps adoption. | Audit current ingestion APIs and design OTLP bridge or adapter. |
| Time-series motif embedding implementation | Needed for retrieval beyond text search. | Prototype segment embedding pipeline over benchmark datasets. |
| Agent trace schema | Needed for trust and audit. | Align with OpenTelemetry GenAI semantic conventions. |
| Edge package size/perf | Needed for physical AI and industrial deployments. | Build an edge profile and benchmark on ARM/Jetson-class hardware. |

## Research Decision

The data supports narrowing the research to:

**Event-Time Incident Memory for AI SRE Agents**

Recommended paper/product framing:

- Problem: LLM-based AIOps agents lack durable, high-fidelity, event-time-aware
  memory across incidents.
- System: ZeptoDB stores incident windows, topology snapshots, change events,
  action logs, postmortems, and agent traces as a time-series memory plane.
- Evaluation: Compare text-only RAG, vector-only incident retrieval, and
  time-series incident memory on AIOpsLab, RCAEval, OpenRCA, and CCF AIOPS 2025
  RCA-style data.
- Product: Start with Similar Incident Retrieval plus Evidence Pack API.
- Expansion: Add runbook recommendation, action-outcome memory, and edge AIOps
  recorder after read-only value is proven.

## Source Log

| ID | Source | Category | Data Captured |
| --- | --- | --- | --- |
| S1 | Mordor AIOps market | Market | 2026/2031 size, CAGR, MTTR driver. |
| S2 | Fortune Business Insights AIOps | Market | 2025/2026/2034 size, CAGR. |
| S3 | Future Market Insights AIOps platform | Market | 2025/2026/2036 size, CAGR. |
| S4 | MRFR AIOps platform | Market | 2024/2025/2035 size, CAGR. |
| S5 | Coherent AIOps platform | Market | 2026/2033 size, CAGR. |
| S6 | MarketsandMarkets AIOps platform | Market | 2023/2028 size, CAGR. |
| S7 | Mordor Autonomous IT operations | Market | 2026/2031 size, CAGR. |
| S8 | Mordor Observability | Market | 2026/2031 size, CAGR. |
| S9 | Datadog Bits Investigation | Product | AI SRE positioning, 90% faster claim. |
| S10 | Datadog Bits AI SRE | Product | Autonomous investigation, actions, code-fix handoff. |
| S11 | Datadog Bits deeper reasoning | Product | 2x faster claim, agent trace, expanded data sources. |
| S12 | Dynatrace Intelligence | Product | Deterministic + agentic AI positioning. |
| S13 | Dynatrace agentic AI docs | Product | Permissions, MCP tools, PII handling. |
| S14 | Splunk Observability AI update | Product | Troubleshooting Agent modalities and next steps. |
| S15 | Splunk troubleshooting agent docs | Product | Auto RCA for APM/Kubernetes alerts. |
| S16 | PagerDuty AIOps | Product | Noise reduction, visibility, triage. |
| S17 | PagerDuty Event Intelligence | Product | Up to 98% noise filtering, related incidents, changes. |
| S18 | PagerDuty Spring 2026 release | Product | SRE Agent positioning. |
| S19 | Grafana Assistant Investigations | Product | 3.5x faster internal case, agent swarm, evidence trail. |
| S20 | OpenTelemetry AI agent observability | Standard | Agent traces, metrics, logs, GenAI conventions. |
| S21 | AIOpsLab | Benchmark | Interactive microservice/fault/agent evaluation framework. |
| S22 | RCAEval | Benchmark | 9 datasets, 735 cases, 15 baselines. |
| S23 | OpenRCA | Benchmark | 335 cases, 68 GB telemetry, 3 systems. |
| S24 | CCF AIOPS 2025 RCA paper | Benchmark | HipsterShop-based metrics/logs/traces RCA dataset. |
| S25 | AIOps LLM survey | Research | Taxonomy and literature map. |
| S26 | Auditable graph-guided RCA | Research | Graph/tool-guided auditable RCA. |
| S27 | AIOps "AI Oops" | Risk | LLM-driven AIOps trust/security risks. |
| S28 | TrioXpert | Research | Automated incident management lifecycle. |
| S29 | ZeptoDB repo docs | Internal | Edge profiles, ROS 2, agent memory, time-series architecture. |
| S30 | TechRadar OpenClaw agentic AI security | Risk | Shadow AI, agent identity, permissions, audit concerns. |
| S31 | ITPro / Forrester agentic AI operationalization | Market/Risk | Enterprises remain pilot-heavy due to governance and infrastructure gaps. |
| S32 | TechRadar agentic SOC | Market/Risk | SOC needs machine-speed investigation and response. |
| S33 | Elastic agentic AI SOC | Product/Risk | Closed-loop containment and traceable reasoning. |
| S34 | AgentSOC | Research | Explainable, risk-aware, policy-aligned autonomous SOC. |
| S35 | NVIDIA Jetson Thor | Physical AI | 2070 FP4 TFLOPS, 128 GB memory, 40-130 W edge robotics compute. |
| S36 | Physical Intelligence MEM | Physical AI | Multi-scale embodied memory for long-horizon robot tasks. |
| S37 | AIOpsLab project site | Benchmark | Interactive autonomous AIOps agent benchmark implementation. |
| S38 | Autonomous Incident Resolution at Hyperscale | Research | Multi-agent autonomous incident resolution with rollback mechanisms. |
| S39 | OpenTelemetry GenAI observability 2026 | Standard | Structured capture of LLM messages and tool calls. |
| S40 | Datadog OTel GenAI semantic convention support | Product/Standard | Prompts, model responses, token usage, tool/agent calls, provider metadata. |
| S41 | Reflexion | Agent memory | Episodic reflection over action feedback without weight updates. |
| S42 | Experiential Reflective Learning | Agent memory | Outcome-based reusable heuristics from experience trajectories. |
| S43 | Voyager | Agent memory / embodied agents | Skill library plus environment feedback for lifelong agent learning. |
