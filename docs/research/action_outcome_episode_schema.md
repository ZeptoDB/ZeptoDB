# Action-Outcome Episode Schema

Date: 2026-06-13
Branch: `codex/aiops-time-series-memory-research`

## Purpose

This schema defines the first research fixture format for Action-Outcome Memory.
It is intentionally product-shaped but still simple enough for replay
experiments.

An episode represents:

```text
situation -> hypothesis -> policy decision -> action -> observation -> outcome -> reflection
```

## Required Top-Level Fields

| Field | Type | Required | Description |
| --- | --- | --- | --- |
| `episode_id` | string | yes | Stable fixture id. |
| `incident_id` | string | yes | Incident or synthetic incident id. |
| `incident_type` | string | yes | Normalized incident family. |
| `service` | string | yes | Primary affected service. |
| `environment` | string | yes | Environment or site. |
| `entity_refs` | array of strings | yes | Affected pods, hosts, queues, regions, devices, or dependencies. |
| `pre_action_window` | object | yes | Event-time window before the action. |
| `action_ts` | string | yes | Event-time timestamp for the action. |
| `post_action_window` | object | yes | Event-time window used to evaluate recovery. |
| `symptoms` | object | yes | Alerts, metric deviations, log signatures, trace symptoms. |
| `topology_context` | object | yes | Relevant service/dependency topology. |
| `change_context` | object | yes | Deploy, config, feature flag, migration, or other change context. |
| `candidate_root_causes` | array of objects | yes | Root-cause hypotheses considered before or during action. |
| `action` | object | yes | Action class, parameters, actor, and target. |
| `policy_decision` | object | yes | Allow/deny/escalate decision, risk tier, and approval. |
| `rollback_plan` | object | yes | Reversal plan and whether it was executed. |
| `machine_outcome_label` | object | yes | Outcome inferred from post-action telemetry. |
| `human_outcome_label` | object | yes | Human or postmortem label. |
| `recovery_curve` | object | yes | Compact pre/post metric deltas. |
| `evidence_refs` | array of objects | yes | References to telemetry windows, logs, traces, tickets, or memory records. |
| `reflection` | string | yes | Reusable lesson for future agents. |
| `tags` | array of strings | yes | Search and ablation tags. |

## Window Object

```json
{
  "start": "2026-06-13T10:00:00Z",
  "end": "2026-06-13T10:05:00Z"
}
```

Rules:

- Use UTC ISO-8601 timestamps.
- `pre_action_window.end` must be less than or equal to `action_ts`.
- `post_action_window.start` must be greater than or equal to `action_ts`.

## Symptoms Object

Recommended fields:

| Field | Type | Description |
| --- | --- | --- |
| `alerts` | array of strings | Alert names or monitor ids. |
| `metrics` | object | Key metric values or deltas. |
| `logs` | array of strings | Log templates or normalized signatures. |
| `traces` | array of strings | Trace/span symptoms. |

## Candidate Root Cause Object

```json
{
  "cause": "payment_client_pool_exhaustion",
  "confidence": 0.72,
  "evidence": ["checkout p99 latency rose after deploy", "payment spans saturated"]
}
```

Rules:

- Confidence is a fixture label between `0.0` and `1.0`.
- Evidence should reference observed signals, not unsupported explanation.

## Action Object

```json
{
  "action_class": "rollback",
  "target": "checkout",
  "parameters": {"deploy_id": "deploy_42", "rollback_to": "deploy_41"},
  "actor": "human_oncall",
  "tool": "deployment_controller"
}
```

Initial allowed action classes:

- `restart`
- `scale_out`
- `rollback`
- `traffic_drain`
- `cache_purge`
- `config_revert`
- `queue_reset`
- `no_action`

## Policy Decision Object

```json
{
  "decision": "allow",
  "risk_tier": "medium",
  "approval": "human_required",
  "approver": "oncall_lead",
  "reason": "rollback is reversible and affects one service"
}
```

Allowed decisions:

- `allow`
- `deny`
- `escalate`
- `shadow_only`

Allowed risk tiers:

- `low`
- `medium`
- `high`
- `critical`

## Outcome Label Object

```json
{
  "label": "success",
  "assigned_by": "post_action_metric_rule",
  "confidence": 0.9,
  "notes": "p95 latency returned below SLO within 8 minutes"
}
```

Allowed labels:

- `success`
- `partial_success`
- `failure`
- `rollback_required`
- `unsafe`
- `insufficient_evidence`

## Recovery Curve Object

Recommended fields:

| Field | Type | Description |
| --- | --- | --- |
| `primary_metric` | string | Main metric used to judge recovery. |
| `before` | number | Value near action time. |
| `after_5m` | number | Value 5 minutes after action. |
| `after_15m` | number | Value 15 minutes after action. |
| `unit` | string | Metric unit. |
| `slo_restored` | boolean | Whether the service returned to target. |
| `side_effects` | array of strings | New alerts or regressions after action. |

## Evidence Reference Object

```json
{
  "type": "metric_window",
  "ref": "zeptodb://observability_metrics?service=checkout&start=...",
  "description": "checkout latency and error window"
}
```

Recommended types:

- `metric_window`
- `log_window`
- `trace_window`
- `topology_snapshot`
- `deploy_event`
- `ticket`
- `postmortem`
- `agent_trace`
- `memory_record`

## Fixture Quality Requirements

The first fixture must include:

- At least 5 incident types.
- At least 4 episodes per incident type.
- At least one success and one failure/rollback per incident type.
- Multiple action classes.
- Both machine and human outcome labels.
- Event-time windows for pre-action and post-action context.
- Negative outcomes preserved as first-class examples.

## Relationship To ZeptoDB

For the first prototype:

- Store this fixture as JSON for easy replay.
- Later map episodes into ZeptoDB tables:
  - `incident_events`
  - `incident_windows`
  - `change_events`
  - `remediation_actions`
  - `recovery_observations`
  - `action_outcomes`
- Store `reflection` fields as Agent Memory records of type
  `remediation_outcome` or `failure_signature`.
