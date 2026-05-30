# 129: Agent-Attached Time-Series Demos

Date: 2026-05-28
Status: Complete

## Context

Agent Memory already had provider-cache, LangGraph-style, and AgentOps examples,
but the vertical story was still implicit. The backlog called for demos that
show Agent Memory as an attachment to ZeptoDB's live time-series workloads:
finance, IoT, observability, robotics, and game/live-ops.

## Changes

- Added `examples/agent_memory/agent_attached_timeseries_demo.py`.
- Modeled five runnable vertical scenarios:
  - finance/HFT ticks, spread, volume, and risk scores plus strategy and
    compliance memory.
  - IoT smart-factory sensor readings plus maintenance history and operator
    notes.
  - observability/APM service metrics plus incident summaries and remediation
    outcomes.
  - robotics fleet telemetry plus route decisions and replayed failure
    episodes.
  - game/live-ops cohort telemetry plus experiment interpretation and approved
    actions.
- Each scenario installs a time-series table, inserts representative rows,
  stores scoped `MemoryRecord` entries with metadata, retrieves context under a
  token budget, and builds the prompt an attached agent would send to a provider.
- Updated Agent Memory example docs, the Python reference, README positioning,
  `docs/COMPLETED.md`, and `docs/BACKLOG.md`.
- Extended Python example tests to cover all verticals, scoped metadata,
  empty-row behavior, and timestamp-zero validity.

## Verification

- `python3 -m py_compile examples/agent_memory/agent_attached_timeseries_demo.py tests/python/test_agent_memory_examples.py`
- `python3 -m pytest -q tests/python/test_agent_memory_examples.py`

Verification was run on the current x86_64 instance. Cross-architecture
verification was not run because this change is Python example/docs-only and
does not alter SIMD, storage layout, or C++ execution paths.

## Follow-ups

- Add OpenTelemetry/LLM trace ingest mapping so observability becomes the first
  production-grade agent use case.
- Add context trace/replay that explains why each memory entered the prompt and
  replays the surrounding time-series state.
