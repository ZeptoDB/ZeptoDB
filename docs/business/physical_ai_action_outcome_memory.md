# Action-Outcome Memory for Physical AI

Date: 2026-06-23
Status: Research narrative, not a promoted product feature

## The Problem

Physical AI systems are starting to act in the real world: robots navigate
warehouses, arms recover from torque spikes, drones route around degraded GPS,
and cold-chain systems decide when intervention is worth the operational cost.

These systems do not only need more perception. They need memory of action and
outcome:

- what the agent decided,
- what context surrounded the decision,
- what happened afterward,
- whether a similar action should be repeated, suppressed, or recovered from.

Generic logs answer "what happened." Action-Outcome Memory answers "what should
we do differently next time?"

## Why Time-Series Matters

Action and outcome are temporal by nature. A useful memory system has to keep
the sequence intact:

- robot state before the decision,
- sensor summaries near the decision,
- retrieved historical incidents,
- selected or suppressed actions,
- recovery outcome,
- fleet-level audit after delayed consolidation.

That makes the problem a time-series database problem as much as an AI problem.
The value is not just storing vectors or text. The value is replaying evidence
with SQL, JOINs, windows, ASOF alignment, and bounded edge-to-fleet transfer.

## The ZeptoDB Research Direction

The current ZeptoDB research track validates a context-gated
Action-Outcome Memory architecture for Physical AI:

1. Similar-incident retrieval and runbook priors are useful but can repeat the
   wrong action when context changes.
2. Reflection-only memory explains failures after the fact but does not reliably
   suppress unsafe repeats.
3. Context-gated Action-Outcome Memory combines retrieval with current
   state/sensor evidence and suppresses actions whose historical outcomes were
   unsafe in the current context.
4. Edge-local memory makes the immediate safety decision.
5. Fleet-global memory converges later for audit, replay, and policy learning.

The key product idea is a "robot operations memory plane": local enough to
protect the edge, structured enough to audit at fleet scale, and replayable
enough to improve future behavior.

## What Has Been Validated

The research track has progressed from offline comparison to live ZeptoDB
runtime paths:

- Experiment 013 compares similar incident retrieval, runbook/action-prior,
  reflection-only memory, and context-gated Physical AI Action-Outcome Memory.
- Experiment 014 validates the Physical AI fixture through native ZeptoDB SQL.
- Experiment 015 separates edge-local immediate suppression from fleet-global
  delayed audit.
- Experiment 016 adds bounded edge-to-fleet feed replay with duplicate,
  dropped, late, outage, and restart cases.
- Experiments 017-018 promote those semantics into a C++ runtime connector and
  validate a live two-node SQL/HTTP replay adapter.
- Experiments 019-020 add server-managed lifecycle control and a bounded worker
  runtime foundation.

This does not yet make the feature production-ready. It does show that the
architecture can be made operational: the evidence is stored as tables,
validated through native SQL, transferred through bounded passes, checkpointed,
and exposed through admin status plus Prometheus metrics.

## Commercial Wedge

The strongest near-term commercial wedge is not "robot memory" as an abstract
AI feature. It is operational safety and recovery analytics for deployed
robotic fleets:

- avoid repeating risky actions in similar contexts,
- explain why an action was suppressed,
- compare recovery options against historical outcomes,
- audit edge decisions after fleet consolidation,
- measure policy improvement over time.

This sits between observability, robotics safety, and fleet operations. A buyer
does not need to believe in a new AI category first; they already need fewer
unsafe repeats, faster incident analysis, and better evidence for why the robot
did what it did.

## Production Promotion Gates

Before this can be described as a promoted ZeptoDB feature, the remaining work
is explicit:

- build the built-in SQL/HTTP adapter into the server runtime,
- persist connector configuration across restart,
- document idempotent sink and ACK-boundary requirements,
- add backpressure, retry pacing, and failure-budget limits,
- add live restart/outage/duplicate/late/fault-injection tests,
- add admin audit events for connector lifecycle changes,
- run focused x86_64 and aarch64 verification,
- document supported scope, limits, non-goals, and rollback procedure.

The current status is best described as an experimental runtime path with a
clear production roadmap.

## Positioning

ZeptoDB can become the time-series memory substrate for physical AI operations:

> Every action becomes evidence. Every outcome becomes memory. Every future
> decision can be checked against what actually happened before.

