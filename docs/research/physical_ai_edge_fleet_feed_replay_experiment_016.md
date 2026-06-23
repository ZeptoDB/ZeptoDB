# Physical AI Action-Outcome Experiment 016 Edge/Fleet Feed Replay

Date: 2026-06-23

Status: Research complete

Classification: Research-only

## Goal

Validate a bounded, explicit edge-to-fleet feed shape for Physical AI
Action-Outcome Memory:

- edge-local memory still suppresses unsafe robot actions immediately,
- edge-generated evidence is written to an edge outbox,
- a bounded feed worker transfers outbox events to fleet-global memory,
- fleet-global memory eventually converges for audit after dropped, duplicate,
  late, outage, and restart conditions.

Experiment 015 proved a two-endpoint replay, but the harness directly wrote the
fleet rows. Experiment 016 replaces that direct copy with outbox, inbox, ACK,
telemetry, retry, and restart-reload semantics.

## Hypothesis

For the current Physical AI fixture, a bounded feed worker can preserve the
commercially important split:

- robot safety decisions remain edge-local and immediate,
- fleet learning/audit can be delayed and eventually consistent,
- duplicate and late feed events do not corrupt fleet audit tables,
- restart can reload fleet ACK rows and continue from the remaining edge outbox.

## Workload Scope

This experiment uses the existing noisy Physical AI fixture with five query
incidents:

- AGV dock slip,
- LiDAR occlusion,
- robot arm torque spike,
- cold-chain temperature excursion,
- drone GPS drift.

The feed transfers three edge-generated event kinds:

- `decision`,
- `retrieval`,
- `suppression`.

Fleet setup tables for historical action outcomes and expected safe actions are
seeded directly because they model fleet-global memory/control context rather
than edge-generated evidence.

## Procedure

1. Start an edge-local ZeptoDB server on port 19441.
2. Start a fleet-global ZeptoDB server on port 19442.
3. Reset and create Experiment 016 edge/fleet tables.
4. Materialize edge-local incident, state, sensor, decision, suppression, and
   outbox rows.
5. Materialize fleet-global base memory rows.
6. Run a bounded feed worker with `batch_limit=12` and `max_inflight=12`.
7. Inject these phases:
   - outage probe to `http://127.0.0.1:1/`,
   - bounded recovery with one dropped event and one duplicate attempt,
   - worker restart with ACK reload and late delivery,
   - bounded final drains until all outbox events are ACKed.
8. Record feed telemetry rows into the fleet node.
9. Validate native SQL row counts, JOINs, ACK convergence, event-kind
   accounting, duplicate/late/outage/restart telemetry, and ACK window queries.

## Acceptance Criteria

The run passes only if all of these hold:

- edge-local immediate recovery JOIN passes,
- edge-local risky-action suppression remains `5/5`,
- edge outbox contains `52` events,
- fleet ACK table converges to `52` events,
- fleet ACK event-kind counts are `decision=5`, `retrieval=15`,
  `suppression=32`,
- duplicate inbox attempts are observed,
- late inbox attempts are observed,
- outage failure telemetry is observed,
- restart reload telemetry is observed with prior ACK state,
- every feed pass stays within `batch_limit` and `max_inflight`,
- fleet final decision/retrieval/suppression tables converge to
  `5/15/32` rows,
- fleet recovery JOIN returns all five expected recovery actions,
- fleet suppression audit JOIN exposes all five misleading hard distractors,
- ACK `ROW_NUMBER`/`LAG` SQL runs over all ACK rows and sorted ACK stream
  completeness is preserved.

## Artifacts

- Harness:
  `docs/research/tools/physical_ai_edge_fleet_feed_replay.py`
- Result report:
  `docs/research/results/physical_ai_edge_fleet_feed_replay_016.md`
- Edge SQL replay:
  `docs/research/results/physical_ai_edge_fleet_feed_replay_016_edge.sql`
- Fleet SQL replay:
  `docs/research/results/physical_ai_edge_fleet_feed_replay_016_fleet.sql`

## Result

See `docs/research/results/physical_ai_edge_fleet_feed_replay_016.md`.

Summary:

- Overall bounded feed replay status: pass.
- Edge-local node stored 134 research rows.
- Fleet-global node stored 198 research rows.
- Edge outbox events: 52.
- Fleet ACK rows: 52.
- Duplicate inbox attempts: 1.
- Late inbox attempts: 2.
- Outage telemetry rows: 1.
- Restart reload telemetry rows: 1.
- Fleet final decision/retrieval/suppression rows: 5/15/32.

## Interpretation

The experiment validates the intended Physical AI memory split more strongly
than Experiment 015. Immediate safety remains local to the edge node, while
fleet-global memory can tolerate bounded delay, duplicate attempts, late
delivery, a temporary outage, and a feed-worker restart before audit converges.

This is still research-only. The feed worker is a deterministic research tool,
not a ZeptoDB runtime replication service. The next product step must define
operator-visible telemetry, persisted cursor state, security boundaries, and
the non-transactional final-table-plus-ACK failure behavior.

## Next Product Or Research Step

Promote the feed semantics into an experimental runtime connector with:

- persisted edge cursor or ACK checkpoint state,
- explicit retry/backoff policy,
- operator-visible feed metrics,
- documented behavior for final-table insert success followed by ACK failure,
- restart and outage tests that use the runtime connector rather than the
  research harness.

Status update: Experiment 017 added the experimental C++ runtime connector
state machine with bounded passes, ACK checkpoint reload, duplicate/late
handling, outage-style retry, and ACK-boundary tests. The remaining step is a
SQL/HTTP source/sink adapter and live two-node replay through that connector.
