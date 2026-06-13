# ZeptoDB Agent Memory Examples

These examples show ZeptoDB as an application-level memory and cache layer for
LLM/agent workloads. They use client-supplied deterministic embeddings and mock
model/provider functions, so they do not require API keys.

## Provider Cache

`provider_cache.py` demonstrates the provider-call path:

1. Embed the prompt in the application.
2. Lookup ZeptoDB exact/semantic cache.
3. Call the provider only on a miss.
4. Store the response for future turns.

```bash
python examples/agent_memory/provider_cache.py
```

## LangGraph-Style Memory

`langgraph_memory.py` exposes graph-shaped node functions:

- `retrieve_context_node`
- `call_model_node`
- `remember_turn_node`

The file does not import LangGraph, but the functions can be wired into a
LangGraph `StateGraph` as ordinary nodes.

```bash
python examples/agent_memory/langgraph_memory.py
```

## Optional Adapters

`adapters.py` contains lazy optional wrappers:

- `OpenAIResponsesAdapter(model=...)`
- `AnthropicMessagesAdapter(model=...)`
- `build_langgraph_app(db, model, embed)`

They do not add hard dependencies to ZeptoDB. Install the provider/framework
library in your application and pass explicit model names from your config.

```python
from examples.agent_memory.adapters import OpenAIResponsesAdapter
from examples.agent_memory.provider_cache import answer_with_cache

provider = OpenAIResponsesAdapter(model="your-openai-model")
answer = answer_with_cache(db, "What should the agent remember?", provider=provider)
```

## Production Agent Demo

`production_agent_demo.py` combines Agent Memory, provider cache, and AgentOps
time-series telemetry tables in one flow. The default provider is still a local
mock; pass an adapter from `adapters.py` to call a real provider from your
application process.

```bash
python -m examples.agent_memory.production_agent_demo
```

The demo installs these tables with `CREATE TABLE IF NOT EXISTS`:

- `agent_runs`
- `retrieval_events`
- `cache_events`
- `context_traces`
- `context_replay_events`
- `llm_calls`
- `llm_errors`
- `tool_calls`

## Context Trace And Replay

`context_trace.py` builds AgentOps INSERT statements that explain why each
memory entered a prompt and record the time-series query snapshots used around
that decision.

```python
from examples.agent_memory.context_trace import (
    build_context_replay_sql,
    build_context_trace_sql,
)

for sql in build_context_trace_sql(context, run_id="run_1", tenant_id="tenant_a", timestamp_ns=now_ns):
    db.execute(sql)

for sql in build_context_replay_sql(snapshots, run_id="run_1", tenant_id="tenant_a", timestamp_ns=now_ns):
    db.execute(sql)
```

## OpenTelemetry GenAI Mapping

`otel_mapping.py` converts OTLP JSON span dictionaries into INSERT statements
for the AgentOps tables. It maps GenAI provider/model spans, prompt/completion
token counts, cache-hit attributes, tool-call spans, latency, and model errors
without requiring an OpenTelemetry SDK dependency.

```python
from examples.agent_memory.otel_mapping import map_spans_to_agentops_sql

sql_rows = map_spans_to_agentops_sql(otlp_spans, default_tenant_id="tenant_a")
for sql in sql_rows:
    db.execute(sql)
```

## Agent-Attached Time-Series Demos

`agent_attached_timeseries_demo.py` shows five vertical scenarios where live
time-series tables and Agent Memory records are used together:

- finance / HFT: ticks, spread, volume, and risk scores plus strategy and
  compliance memory.
- IoT / smart factory: sensor readings plus maintenance history and operator
  notes.
- observability / APM: service metrics plus incident summaries and remediation
  outcomes.
- robotics / fleet operations: route telemetry plus failure episodes and route
  decisions.
- game / live ops: cohort telemetry plus experiment interpretation and approved
  actions.

Start ZeptoDB first:

```bash
./build/zepto_http_server --no-auth \
  --agent-memory-dir /tmp/zeptodb_agent_memory \
  --agent-memory-max-memories 100000 \
  --agent-memory-max-cache-entries 10000
```

Then run:

```bash
python -m examples.agent_memory.agent_attached_timeseries_demo
```
