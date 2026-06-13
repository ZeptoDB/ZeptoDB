# 164: P3 OpenTelemetry LLM Mapping

Date: 2026-06-05
Status: Complete

## Context

P3 positions observability as a production Agent Memory use case. The existing
AgentOps example tables captured agent runs, retrievals, cache events, LLM
calls, and tool calls, but there was no concrete mapping from OpenTelemetry
GenAI spans into those tables.

## Changes

- Added `examples/agent_memory/otel_mapping.py`.
- The mapper accepts OTLP JSON-style spans with either attribute arrays or flat
  attribute dictionaries.
- GenAI spans map to `llm_calls` with provider, model, prompt/completion token
  counts, latency, cache-hit state, run id, and tenant id.
- Cache-hit attributes also emit `cache_events`.
- Tool spans emit `tool_calls` with tool name, status, latency, run id, and
  tenant id.
- Error spans emit `llm_errors`; `agentops_schema.py` now installs that table.
- Updated Agent Memory example docs and design docs.

## Verification

- `python3 -m pytest -q tests/python/test_agent_memory_examples.py` —
  13/13 passed.

## Follow-ups

- Context trace/replay was completed in devlog 165.
