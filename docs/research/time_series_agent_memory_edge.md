# Time-Series Agent Memory and Edge Physical AI Research Log

Date: 2026-06-13

Companion data log:
`docs/research/aiops_time_series_memory_research_data.md`

Action-outcome engine plan:
`docs/research/action_outcome_memory_engine_plan.md`

Research execution roadmap:
`docs/research/action_outcome_research_execution_roadmap.md`

Research process log:
`docs/research/action_outcome_research_process_log.md`

First replay experiment:
`docs/research/action_outcome_replay_experiment_001.md`

## Working Thesis

The most commercially promising research direction is **time-series-native
incident memory for agentic AIOps and physical AI edge systems**.

The core idea is to treat operational telemetry, robot telemetry, logs, traces,
sensor streams, agent actions, and remediation outcomes as a temporal memory
substrate. An agent should not only retrieve semantically similar text memories;
it should retrieve temporally similar situations: precursor patterns, anomaly
motifs, action histories, causal hypotheses, and recovery outcomes.

This is especially relevant for ZeptoDB because the project already positions
itself as an in-memory, high-throughput time-series database and includes
agent-memory, ROS 2, physical AI, telemetry, and edge-deployment documentation.

## Commercial Priority

The highest-value wedge is:

**ZeptoDB Edge Incident Memory: an agent-ready time-series memory layer for
observability, industrial telemetry, and robot fleets.**

Why this looks more commercial than a generic "agent memory" layer:

- Enterprise buyers already pay for observability, incident response, predictive
  maintenance, fleet operations, and downtime reduction.
- The ROI is measurable: lower MTTR, fewer repeated incidents, better operator
  handoff, safer remediation, reduced cloud telemetry costs, and better fleet
  replay.
- Existing vector-memory products are not optimized for high-frequency telemetry,
  event-time semantics, replay, sensor fusion, or root-cause timelines.
- Physical AI and robotics need local temporal context before data reaches the
  cloud. A robot or factory gateway cannot rely only on cloud memory during
  network loss, latency spikes, or safety-critical operation.

Best first market:

1. Agentic AIOps and observability incident memory.
2. Industrial predictive maintenance and smart-factory telemetry.
3. Robot fleet / drone / physical AI memory and replay.

## Why Time-Series Agent Memory Matters

Generic agent memory usually stores facts, documents, summaries, or conversation
history. Time-series agent memory stores **how a system changed over time**.
That enables use cases that semantic memory alone handles poorly:

- Similar incident retrieval: "Have we seen this metric/log/trace pattern before?"
- Early-warning memory: "Which precursor pattern usually appears 10 minutes before
  this failure?"
- Action-outcome memory: "What did the agent or operator do last time, and did it
  work?"
- Fleet memory: "Which robots, machines, hosts, or sites show the same temporal
  motif?"
- Local replay: "Reconstruct the last 30 seconds before a robot collision,
  sensor fault, or control anomaly."
- Causal timeline building: "Which event came first in event time, ingestion time,
  and remediation time?"

The research opportunity is to combine:

- Time-series storage and windowed retrieval.
- Temporal knowledge graphs.
- Segment-level embeddings for motifs and anomalies.
- Event-time-aware retrieval augmented generation.
- Agent action logs and outcome scoring.
- Edge-to-cloud memory consolidation.

## ZeptoDB Edge Fit

Short answer: **ZeptoDB can plausibly run on edge gateway and robot-local
devices, but it is not suitable for tiny microcontrollers or very small edge
nodes in its current full-server form.**

Evidence from the repository:

- `docs/operations/ROS2_EDGE_DEPLOYMENT.md` already defines robot-local, lab-edge,
  and factory-edge ZeptoDB deployment profiles.
- The documented robot-local resource envelope is roughly 2 vCPU, 4-8 GiB RAM,
  and local NVMe storage.
- `docs/devlog/023_arm_graviton_verification.md` records successful aarch64
  build and test verification.
- `docs/operations/ROS2_SETUP.md` shows an edge-oriented build configuration with
  Python and DuckDB disabled for a smaller validation surface.
- `docs/operations/TELEGRAF_OUTPUT.md` shows edge telemetry ingestion through an
  external Telegraf output plugin.
- `docs/design/ros2_physical_ai_roadmap.md` positions ZeptoDB beside ROS 2 as a
  time-series layer for telemetry, replay, feature calculation, observability,
  and physical AI workflows.

Practical edge tiers:

| Tier | Fit | Notes |
| --- | --- | --- |
| Factory edge gateway | Strong fit | Local NVMe, several CPU cores, enough RAM, stable power/network. |
| Robot-local computer | Plausible fit | Works for Jetson/industrial PC class devices if build features are trimmed. |
| Drone companion computer | Possible but constrained | Needs careful retention, compression, and CPU budgeting. |
| Tiny edge sensor / MCU | Poor fit | Use a collector/forwarder and sync to ZeptoDB on a gateway. |

Important constraints:

- The full Docker image is documented as roughly 300 MB and includes heavyweight
  capabilities such as LLVM/JIT, OpenSSL, Arrow Flight, Parquet, S3, LZ4,
  HugePages, and the Web UI.
- Several useful features are optional or already default-off, including Kafka,
  MQTT, Kinesis, Pulsar, OPC UA, ROS 2, and HNSW.
- Some defaults are more server-oriented than edge-oriented, including Flight and
  DuckDB being enabled by default.
- Release builds use `-march=native`, which is good for local performance but
  means edge packaging should build on the target architecture or use explicit
  portable CPU flags.
- For physical AI, large payloads such as images, point clouds, and bag files
  should usually be stored externally, with ZeptoDB storing temporal metadata,
  features, hashes, and payload references.

Recommended product profiles:

1. **Edge gateway profile**
   - Enable core server, HTTP ingest/query, WAL/HDB, LZ4, metrics, and selected
     industrial connectors.
   - Disable Flight, DuckDB, S3, JIT, Python, and unused connectors unless needed.

2. **Robot-local recorder profile**
   - Enable ROS 2 typed profiles, local WAL/HDB, bounded retention, and replay
     indexes.
   - Keep large media as object or file references.

3. **Tiny-edge collector profile**
   - Do not run full ZeptoDB.
   - Run a small Telegraf, ROS 2, or custom collector and forward to a nearby
     ZeptoDB gateway.

## Why Edge Is Central to This Research

Edge deployment is not just an implementation detail. It is part of the research
value proposition for time-series agent memory.

Reasons:

- Physical AI incidents happen before cloud ingestion completes.
- Robots and factories need local context during network outages.
- High-frequency sensor streams are expensive to send to cloud in full fidelity.
- Safety investigations often require local replay with event-time precision.
- Privacy and IP constraints may prevent raw sensor/log data from leaving a site.
- Local memories can be summarized and promoted into fleet/global memory.

This suggests a two-level memory architecture:

1. **Local temporal memory at the edge**
   - High-fidelity recent telemetry.
   - Fast replay and root-cause timeline construction.
   - Local agent decisions and operator actions.

2. **Fleet/global memory in cloud or datacenter**
   - Compressed incident signatures.
   - Cross-site similarity search.
   - Long-term action-outcome learning.
   - Model and policy updates distributed back to edge nodes.

## Research Questions Worth Pursuing

1. **Temporal Similarity Retrieval**
   - Can a memory system retrieve similar incidents from metric/log/trace/sensor
     windows better than text-only retrieval?
   - Compare raw time-series distance, learned segment embeddings, and hybrid
     symbolic-temporal retrieval.

2. **Event-Time-Aware Agent Memory**
   - How much do event time, ingestion time, action time, and replay time improve
     root-cause accuracy?
   - Build temporal memory records with explicit time provenance.

3. **Edge Memory Compression**
   - What should be retained locally under a fixed storage budget: raw windows,
     sketches, anomalies, embeddings, summaries, or causal graphs?
   - Measure MTTR/RCA accuracy versus retention cost.

4. **Action-Outcome Memory**
   - Store remediation actions, confidence, operator overrides, and outcomes.
   - Retrieve "what worked last time" for similar incidents.

5. **Fleet Memory Consolidation**
   - Promote local incident signatures into a global memory.
   - Detect repeated patterns across robots, factories, hosts, or customer sites.

6. **Safety and Auditability**
   - Agents should produce a timeline, evidence pack, and rollback record for
     every recommendation or remediation.
   - This is a differentiator for enterprise and physical AI adoption.

## Suggested Prototype

Build a prototype called **Temporal Incident Memory**:

- Ingest high-frequency telemetry into ZeptoDB.
- Segment streams into windows around anomalies, operator actions, and system
  state transitions.
- Generate compact memory records:
  - metric/log/trace/sensor window references,
  - temporal features,
  - anomaly type,
  - causal hypothesis,
  - action taken,
  - outcome,
  - confidence,
  - source timestamps and replay timestamps.
- Support retrieval by:
  - time range,
  - entity/fleet/site,
  - anomaly motif,
  - vector similarity,
  - temporal graph neighborhood,
  - outcome success/failure.
- Let an agent answer:
  - "What is happening now?"
  - "Have we seen this before?"
  - "What usually happens next?"
  - "What action worked last time?"
  - "What evidence supports that recommendation?"

## AIOps Expansion: How This Gets Used

The strongest adoption path is to make ZeptoDB a **machine-readable memory plane
for SRE and operations agents**, not a replacement for existing observability
dashboards on day one.

Most enterprises already have Datadog, Dynatrace, Splunk, Grafana, Prometheus,
Elastic, PagerDuty, Jira, Slack, Kubernetes, and OpenTelemetry somewhere in the
stack. A commercially viable ZeptoDB AIOps product should attach to those systems
and add the thing they do not naturally provide: long-lived, time-series-native,
agent-queryable incident memory.

### Product Entry Points

1. **Incident memory backend**
   - Ingest metrics, logs, traces, deploy events, alerts, runbook actions, and
     postmortems.
   - Store incident-centered windows with event-time fidelity.
   - Expose retrieval APIs for "similar previous incidents", "likely precursor",
     and "successful prior remediation".

2. **AI SRE copilot data layer**
   - Let an existing LLM/agent call ZeptoDB tools instead of asking the model to
     reason from a dashboard screenshot or raw log dump.
   - Return compact evidence packs: timeline, correlated signals, prior cases,
     candidate causes, and action history.

3. **Postmortem-to-memory pipeline**
   - Convert every closed incident into a reusable memory object.
   - Link the human postmortem to the exact telemetry windows, topology, deploys,
     alerts, and remediation steps that occurred during the incident.

4. **Edge AIOps recorder**
   - Run ZeptoDB near plants, robots, devices, or customer sites.
   - Preserve recent high-fidelity telemetry locally and ship compressed incident
     signatures to cloud memory.

5. **Evaluation and benchmark harness**
   - Use AIOpsLab, RCAEval, OpenRCA, and internal incident replays to evaluate
     root-cause ranking, action recommendations, and explanation faithfulness.

### Adoption Ladder

Start with low-risk read-only value, then move toward guarded automation.

| Stage | User Value | Risk Level | Product Behavior |
| --- | --- | --- | --- |
| 1. Read-only investigation | Faster triage and postmortem search | Low | Agent retrieves similar incidents and evidence packs. |
| 2. Root-cause ranking | Better prioritization during incident response | Medium | Agent ranks candidate causes with citations to telemetry. |
| 3. Runbook recommendation | Less operator toil | Medium | Agent recommends known-safe actions and shows prior outcomes. |
| 4. Guarded remediation | Faster MTTR | Higher | Agent executes approved actions with human confirmation. |
| 5. Autonomous remediation | Self-healing operations | Highest | Agent acts within strict policy, rollback, and audit constraints. |

For 2026-era enterprise adoption, stages 1-3 are the practical wedge. Full
autonomous remediation is marketable, but many organizations will restrict it to
basic or reversible tasks until governance, security, and trust improve.

### Core AIOps Memory Object

The key abstraction should be an `incident_memory` record, not a generic vector
document.

Suggested fields:

- `incident_id`
- `tenant_id`, `environment`, `service`, `resource`, `region`, `cluster`
- `start_ts`, `detect_ts`, `mitigate_ts`, `resolve_ts`
- `clock_domain`, `event_time_range`, `ingest_time_range`
- `symptoms`: alert names, metric deviations, log templates, trace anomalies
- `topology_context`: service graph, pod/node/container, dependency edges
- `change_context`: deploys, config changes, feature flags, migrations
- `anomaly_segments`: references to raw time-series windows
- `embedding_refs`: segment embeddings, log embeddings, postmortem embeddings
- `candidate_causes`
- `confirmed_root_cause`
- `actions_taken`
- `action_outcomes`
- `rollback_steps`
- `human_notes`
- `agent_trace`
- `confidence`, `evidence_score`, `safety_score`

This object should point to raw ZeptoDB time windows rather than duplicating all
raw telemetry into the memory record.

### AIOps Retrieval Modes

A useful system should combine several retrieval modes:

- **Time-range retrieval**: find all signals around incident start, detection,
  mitigation, and resolution.
- **Motif retrieval**: find past windows with similar metric/log/trace patterns.
- **Topology retrieval**: find incidents near the same service, dependency, pod,
  node, device, or customer site.
- **Change-aware retrieval**: include deploys, config changes, scaling events,
  schema changes, and feature flags.
- **Outcome retrieval**: prioritize incidents where remediation outcome is known.
- **Policy retrieval**: retrieve approved runbooks, blocked actions, escalation
  rules, and rollback requirements.

The differentiator is the combination. Vector similarity alone is not enough for
AIOps because two incidents can sound similar in text but differ in topology,
time order, blast radius, or remediation safety.

### Integrations That Make It Usable

To be used in real AIOps workflows, ZeptoDB should integrate where operators and
agents already work:

- OpenTelemetry / OTLP for traces, metrics, logs, and GenAI agent telemetry.
- Prometheus remote write or scrape-compatible metrics.
- Kubernetes events, pod metadata, node metadata, and service topology.
- GitHub/GitLab deployment events and commit metadata.
- PagerDuty/Opsgenie incident lifecycle events.
- Jira/Linear tickets and postmortem documents.
- Slack/Teams incident channel transcripts.
- Datadog/Dynatrace/Splunk/Grafana export or webhook paths.
- Runbook tools and internal automation systems.

The product should not require a team to migrate all observability data first.
It should start by attaching to the incident lifecycle and gradually become the
long-term incident memory layer.

### Research Experiments

The most useful research experiments are measurable against SRE outcomes:

1. **Similar Incident Retrieval**
   - Given the first N minutes of an incident, retrieve prior incidents with the
     same confirmed root cause.
   - Metrics: top-k recall, time-to-first-useful-case, retrieval latency.

2. **RCA Candidate Ranking**
   - Given metrics, logs, traces, topology, and deploy events, rank likely root
     causes.
   - Metrics: top-1/top-3 accuracy, evidence precision, false-cause rate.

3. **Action Recommendation**
   - Given a candidate incident, recommend a safe next action based on previous
     outcomes.
   - Metrics: action success rate, unsafe recommendation rate, rollback need.

4. **Postmortem Grounding**
   - Generate a postmortem draft linked to telemetry evidence and action history.
   - Metrics: human edit distance, evidence coverage, hallucinated claim rate.

5. **Retention Policy Optimization**
   - Compare raw retention, sketch retention, anomaly-only retention, and
     segment-embedding retention.
   - Metrics: RCA accuracy per GB, retrieval latency, edge storage lifetime.

6. **Agent Telemetry Memory**
   - Store the agent's tool calls, prompts, retrieved evidence, actions, and
     outcomes.
   - Metrics: reproducibility, audit completeness, repeated-mistake reduction.

### Why ZeptoDB Can Win a Niche

Large observability vendors are already adding AI SRE agents, so a new system
should not compete head-on as another dashboard. The better niche is:

- Self-hosted or edge-deployable memory for regulated, industrial, robotics, and
  data-residency-sensitive customers.
- High-frequency time-series storage where full-fidelity windows matter.
- Agent APIs that return structured evidence instead of only charts.
- Incident memory that combines raw telemetry, temporal features, topology,
  postmortems, and action outcomes.
- Edge-to-cloud consolidation for fleets and physical AI systems.

This makes ZeptoDB useful even when a customer keeps Datadog, Dynatrace, Splunk,
or Grafana as the human-facing UI.

### AIOps MVP Recommendation

The first build should be intentionally narrow:

**MVP: Similar Incident Retrieval + Evidence Pack API**

Inputs:

- service/resource identifier,
- current alert,
- recent time window,
- optional topology/deploy context.

Outputs:

- top similar incidents,
- aligned timelines,
- matching signals,
- known root causes,
- prior remediation actions,
- action outcomes,
- links to raw ZeptoDB windows and postmortems.

This MVP is commercially useful because it can run in read-only mode, improves
on-call workflows immediately, and creates the data foundation for later RCA and
remediation agents.

## High-Risk Gamechanger Bet

If the goal is not only a realistic MVP but a category-defining bet, the strongest
direction is:

**Closed-loop Incident Autopilot powered by Time-Series Action-Outcome Memory**

This means moving beyond "the agent explains the incident" toward "the agent can
safely fix recurring incidents, prove why, roll back if needed, and learn from
the outcome."

The core memory primitive is not just an incident summary. It is an
action-outcome timeline:

- what the system looked like before the incident,
- which signals changed first,
- what root-cause hypotheses were considered,
- what action was taken,
- who or what approved it,
- how the metrics/logs/traces recovered,
- whether rollback was needed,
- whether the same action worked again later.

This is risky because an agent can damage production if it acts on a wrong cause.
But it is also the most game-changing AIOps direction because it turns memory
into operational leverage. The first safe version should only handle reversible,
policy-approved actions such as restart, rollback, scale-out, traffic drain, or
cache purge.

The second high-risk bet is:

**Edge Embodied Incident Memory / Robot Black Box**

This applies the same time-series action-outcome memory idea to robots, drones,
smart factories, and physical AI fleets. The upside may be larger over a long
time horizon, but deployment is harder because hardware, safety, sensors, and
robotics sales cycles add friction.

The unifying bet across AIOps and physical AI is:

**Own the time-series memory of actions, outcomes, and recovery.**

## Relevance to ZeptoDB Roadmap

This research aligns with existing ZeptoDB directions:

- Agent memory examples already exist under `examples/agent_memory/`.
- Physical AI use cases are documented under `docs/usecases/physical_ai.md`.
- ROS 2 and physical AI roadmap docs already define time provenance and typed
  profile concepts.
- Edge deployment docs already define robot-local, lab-edge, and factory-edge
  modes.
- Storage design already supports in-memory columnar time-series data,
  timestamp-range indexing, snapshots, WAL/HDB, and high-throughput ingestion.

The missing commercially valuable layer is the **incident-memory abstraction**
above raw telemetry: segmenting, summarizing, indexing, and retrieving past
temporal situations for agents.

## External Signals and Sources

Market and industry:

- Mordor Intelligence estimates the AIOps market at USD 18.95B in 2026 and USD
  37.79B by 2031.
  <https://www.mordorintelligence.com/industry-reports/aiops-market>
- MarketsandMarkets estimates predictive maintenance at USD 13.89B in 2026 and
  USD 23.79B by 2031.
  <https://www.marketsandmarkets.com/Market-Reports/operational-predictive-maintenance-market-8656856.html>
- RootsAnalysis estimates the edge AI market at USD 25.84B in 2026 and USD
  245.18B by 2040.
  <https://www.rootsanalysis.com/edge-ai-market>
- Fortune Business Insights estimates edge computing at USD 25.63B in 2026 and
  USD 267.42B by 2034.
  <https://www.fortunebusinessinsights.com/edge-computing-market-103760>

Agentic observability and physical AI:

- Datadog Bits Investigation is positioned as an AI SRE agent for autonomous
  troubleshooting and root-cause identification.
  <https://www.datadoghq.com/product/ai/bits-investigation/>
- Datadog Bits AI SRE describes autonomous alert investigation, recommended
  actions, synthetic test investigation, and code-fix handoff.
  <https://www.datadoghq.com/blog/bits-ai-sre/>
- Dynatrace Intelligence positions deterministic insights plus agentic action for
  autonomous prevention, remediation, and optimization.
  <https://www.dynatrace.com/platform/artificial-intelligence/>
- Dynatrace agentic AI documentation shows enterprise controls such as scoped
  permissions, MCP tool access, PII blocking, and user-based data access.
  <https://docs.dynatrace.com/docs/dynatrace-intelligence/agentic-and-generative-ai/agentic-and-generative-ai-getting-started>
- OpenTelemetry describes evolving standards for AI agent observability using
  standardized traces, metrics, and logs.
  <https://opentelemetry.io/blog/2025/ai-agent-observability/>
- TechRadar argues that AI agents need long-term, full-fidelity telemetry rather
  than human-oriented dashboards alone.
  <https://www.techradar.com/pro/observability-was-built-for-humans-ai-agents-need-something-different>
- Splunk describes agentic observability as autonomous agents integrated with
  telemetry pipelines for diagnosis and remediation.
  <https://www.splunk.com/en_us/blog/learn/agentic-observability.html>
- IBM describes AI agent observability as telemetry for both traditional system
  metrics and AI-specific agent behavior.
  <https://www.ibm.com/think/insights/ai-agent-observability>
- TechRadar notes that physical AI scaling depends on edge inference, simulation,
  staged deployment, and operational integration.
  <https://www.techradar.com/pro/the-key-steps-that-will-enable-organizations-to-scale-physical-ai>

Research directions:

- Temporal Semantic Memory:
  <https://arxiv.org/html/2601.07468v1>
- Zep temporal knowledge graph memory:
  <https://arxiv.org/html/2501.13956v1>
- Retrieval Augmented Time Series Forecasting:
  <https://arxiv.org/abs/2411.08249>
- TimeRAG:
  <https://arxiv.org/abs/2412.16643>
- AnomSeer anomaly explanation:
  <https://arxiv.org/html/2602.08868v2>
- OpenRCA:
  <https://netman.aiops.org/wp-content/uploads/2025/05/13411_OpenRCA_Can_Large_Langua.pdf>
- AIOpsLab:
  <https://arxiv.org/abs/2501.06706>
- RCAEval:
  <https://github.com/phamquiluan/RCAEval>
- Survey of AIOps in the Era of Large Language Models:
  <https://arxiv.org/html/2507.12472v1>
- Root Cause Analysis Method Based on Large Language Models:
  <https://arxiv.org/html/2602.08804v1>
- Auditable Graph-Guided Root Cause Analysis for Kubernetes:
  <https://arxiv.org/html/2606.08590v1>

## Bottom Line

The strongest commercial research bet is not broad agent memory. It is
**time-series incident memory for agents that operate over real systems**.

For ZeptoDB, edge relevance is high. The database already has enough design and
deployment support to target gateway-class and robot-local environments. The
research should focus on the layer that turns raw time-series telemetry into
retrievable operational memory: temporal segments, causal timelines, similar
incident retrieval, action-outcome records, and edge-to-cloud fleet memory.
