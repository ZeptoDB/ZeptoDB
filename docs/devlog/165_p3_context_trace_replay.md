# 165: P3 Context Trace Replay

Date: 2026-06-05
Status: Complete

## Context

Agent Memory context assembly returns ranked memories, but operators and
auditors also need a durable explanation of why memories entered a prompt and
what time-series state surrounded that decision.

## Changes

- Added AgentOps schema tables `context_traces` and
  `context_replay_events`.
- Added `examples/agent_memory/context_trace.py`.
- `build_context_trace_sql()` emits one row per selected memory with run id,
  tenant id, memory id, rank, score, similarity, token count, reason, and
  timestamp.
- `explain_memory_selection()` derives compact reason strings from pinned,
  semantic similarity, importance, prior-use, and token-budget-fit signals.
- `build_context_replay_sql()` records replay metadata for time-series queries
  observed around an agent decision.
- Updated Agent Memory example docs and design docs.

## Verification

- `python3 -m pytest -q tests/python/test_agent_memory_examples.py` —
  14/14 passed.

## Follow-ups

- Devlog 166 completed multi-node Agent Memory cluster stats. Remaining
  multi-node work is automatic TTL/capacity-eviction tombstones, replica
  promotion/degraded-state reporting, and full rollback for capacity-eviction
  side effects.
