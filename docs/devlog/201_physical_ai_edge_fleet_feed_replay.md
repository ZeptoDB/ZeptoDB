# 201: Physical AI Edge/Fleet Feed Replay

Date: 2026-06-23

Status: Complete

Classification: Research-only

## Context

Experiment 015 proved the Physical AI memory split with two live ZeptoDB
endpoints, but the research harness still wrote fleet rows directly. The next
question was whether the same edge-local safety and fleet-global audit shape
survives a more realistic bounded feed path with dropped, duplicated, late,
outage, and restart conditions.

## Changes

- Added `docs/research/tools/physical_ai_edge_fleet_feed_replay.py`.
  - Materializes edge-local incidents, robot state, sensor summaries,
    decisions, suppressions, and a 52-row edge outbox.
  - Seeds fleet-global historical action outcomes and expected safe actions.
  - Transfers only edge-generated `decision`, `retrieval`, and `suppression`
    events through a bounded feed worker.
  - Records fleet inbox attempts, persistent ACK rows, and feed telemetry rows.
  - Injects outage, dropped-event retry, duplicate delivery, late delivery, and
    restart ACK reload phases.
  - Validates native SQL row counts, JOINs, ACK convergence, event-kind
    accounting, duplicate/late/outage/restart telemetry, bounded batch limits,
    and ACK window queries.
- Added Experiment 016 docs and generated replay artifacts:
  - `docs/research/physical_ai_edge_fleet_feed_replay_experiment_016.md`
  - `docs/research/results/physical_ai_edge_fleet_feed_replay_016.md`
  - `docs/research/results/physical_ai_edge_fleet_feed_replay_016_edge.sql`
  - `docs/research/results/physical_ai_edge_fleet_feed_replay_016_fleet.sql`
- Updated research tracking docs:
  - `docs/research/action_outcome_research_process_log.md`
  - `docs/research/physical_ai_edge_fleet_replay_experiment_015.md`
  - `docs/COMPLETED.md`
  - `docs/BACKLOG.md`

## Result

The live two-node replay passed:

- edge-local immediate recovery and risky-action suppression,
- 52 edge outbox events,
- 52 fleet ACK rows,
- 5/15/32 ACK rows for decision/retrieval/suppression,
- duplicate, late, outage, and restart telemetry checks,
- bounded feed pass checks with `batch_limit=12` and `max_inflight=12`,
- fleet recovery JOIN and suppression audit JOIN,
- ACK `ROW_NUMBER`/`LAG` SQL acceptance and sorted stream completeness.

## Verification

```bash
python3 -m py_compile docs/research/tools/physical_ai_edge_fleet_feed_replay.py

python3 -m json.tool docs/research/fixtures/physical_ai_action_outcome_episodes.json >/tmp/physical_ai_action_outcome_episodes.pretty.json

./build/zepto_http_server --port 19441 --node-id 1 --no-auth --storage-mode pure

./build/zepto_http_server --port 19442 --node-id 8 --no-auth --storage-mode pure

python3 docs/research/tools/physical_ai_edge_fleet_feed_replay.py \
  --edge-url http://127.0.0.1:19441/ \
  --fleet-url http://127.0.0.1:19442/ \
  --outage-url http://127.0.0.1:1/ \
  --edge-stats-url http://127.0.0.1:19441/stats \
  --fleet-stats-url http://127.0.0.1:19442/stats \
  --fixture docs/research/fixtures/physical_ai_action_outcome_episodes.json \
  --output docs/research/results/physical_ai_edge_fleet_feed_replay_016.md \
  --edge-sql-output docs/research/results/physical_ai_edge_fleet_feed_replay_016_edge.sql \
  --fleet-sql-output docs/research/results/physical_ai_edge_fleet_feed_replay_016_fleet.sql \
  --timeout 10 \
  --batch-limit 12 \
  --max-inflight 12
```

## Experimental Boundary

This is research-only. It does not add a runtime replication service, control
plane, HTTP API, SQL syntax, or persistent ZeptoDB feed connector.

Validated scope:

- one edge endpoint and one fleet endpoint,
- deterministic research fixture,
- append-only outbox/inbox/ACK/telemetry tables,
- bounded feed passes driven by the Python harness.

Non-goals:

- exactly-once runtime replication,
- transactional final-table plus ACK commit,
- multi-edge fan-in,
- operator security/RBAC for feed administration,
- durable feed cursor owned by ZeptoDB runtime.

## Follow-ups

- Promote the feed semantics into an experimental runtime connector.
- Add persisted cursor/checkpoint behavior.
- Define explicit recovery for final-table insert success followed by ACK
  failure.
- Add runtime tests for duplicate, dropped, late, outage, and restart behavior.
