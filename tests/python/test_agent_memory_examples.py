"""Tests for Agent Memory examples without external LLM calls."""

from types import SimpleNamespace

from examples.agent_memory.adapters import (
    AnthropicMessagesAdapter,
    OpenAIResponsesAdapter,
    anthropic_state_model,
    openai_state_model,
)
from examples.agent_memory.agentops_schema import AGENTOPS_DDL, install_agentops_schema
from examples.agent_memory.context_trace import (
    build_context_replay_sql,
    build_context_trace_sql,
    explain_memory_selection,
)
from examples.agent_memory.otel_mapping import (
    map_span_to_agentops_sql,
    map_spans_to_agentops_sql,
)
from examples.agent_memory.agent_attached_timeseries_demo import (
    USE_CASES,
    run_all_use_cases,
    run_use_case,
)
from examples.agent_memory.langgraph_memory import run_turn
from examples.agent_memory.provider_cache import answer_with_cache
from examples.agent_memory.production_agent_demo import run_production_turn


class FakeCache:
    def __init__(self):
        self.entries = {}
        self.lookup_calls = 0
        self.store_calls = 0

    def lookup(self, prompt, embedding, namespace, tenant_id, semantic_threshold):
        self.lookup_calls += 1
        entry = self.entries.get((tenant_id, namespace, prompt))
        if entry is None:
            return {"hit": False}
        return {"hit": True, "kind": "exact", "score": 1.0, "entry": entry}

    def store(
        self,
        prompt,
        response,
        embedding,
        namespace,
        tenant_id,
        metadata_json="{}",
        token_count=0,
    ):
        self.store_calls += 1
        self.entries[(tenant_id, namespace, prompt)] = {
            "prompt": prompt,
            "response": response,
            "metadata_json": metadata_json,
        }
        return "cache_1"


class FakeQueryResult:
    def __init__(self, sql):
        self.sql = sql
        self.columns = ["scope", "value"]
        self.rows = [["demo", 1]]
        self.row_count = 1


class FakeMemory:
    def __init__(self):
        self.stored = []

    def get_context(
        self,
        query_embedding,
        token_budget,
        namespace,
        tenant_id,
        user_id,
        session_id,
        agent_id,
        limit,
    ):
        matches = [
            {
                "content": memory["content"],
                "score": 1.0,
            }
            for memory in self.stored
            if memory["tenant_id"] == tenant_id
            and memory["namespace"] == namespace
            and memory["agent_id"] == agent_id
        ][:limit]
        if matches:
            return {
                "memories": matches,
                "token_count": min(token_budget, sum(max(1, len(m["content"]) // 4) for m in matches)),
            }
        return {
            "memories": [
                {
                    "content": "User prefers concise implementation plans.",
                    "score": 1.0,
                }
            ],
            "token_count": 6,
        }

    def put(
        self,
        content,
        embedding,
        namespace,
        tenant_id,
        user_id,
        session_id,
        agent_id,
        type="memory",
        metadata_json="{}",
        token_count=0,
        importance=0.0,
        pinned=False,
    ):
        self.stored.append(
            {
                "content": content,
                "namespace": namespace,
                "tenant_id": tenant_id,
                "user_id": user_id,
                "session_id": session_id,
                "agent_id": agent_id,
                "type": type,
                "metadata_json": metadata_json,
                "token_count": token_count,
                "importance": importance,
                "pinned": pinned,
            }
        )
        return f"mem_{len(self.stored)}"


class FakeDb:
    def __init__(self):
        self.cache = FakeCache()
        self.memory = FakeMemory()
        self.sql = []
        self.queries = []

    def execute(self, sql):
        self.sql.append(sql)
        return 1

    def query(self, sql):
        self.queries.append(sql)
        return FakeQueryResult(sql)


def test_provider_cache_calls_provider_once_then_hits_cache():
    db = FakeDb()
    provider_calls = []

    def provider(prompt):
        provider_calls.append(prompt)
        return "provider response"

    first = answer_with_cache(db, "hello", provider=provider)
    second = answer_with_cache(db, "hello", provider=provider)

    assert first["source"] == "provider"
    assert second["source"] == "cache"
    assert second["response"] == "provider response"
    assert provider_calls == ["hello"]
    assert db.cache.store_calls == 1
    assert db.cache.lookup_calls == 2


def test_langgraph_style_turn_retrieves_context_and_stores_memory():
    db = FakeDb()
    state = run_turn(db, "How should we implement this?")

    assert "Using remembered context" in state["response"]
    assert state["stored_memory_id"] == "mem_1"
    assert len(db.memory.stored) == 1
    assert db.memory.stored[0]["type"] == "turn"
    assert "How should we implement this?" in db.memory.stored[0]["content"]


def test_openai_responses_adapter_uses_responses_api():
    calls = []

    class FakeResponses:
        def create(self, **kwargs):
            calls.append(kwargs)
            return SimpleNamespace(output_text="openai answer")

    client = SimpleNamespace(responses=FakeResponses())
    adapter = OpenAIResponsesAdapter(
        model="test-openai-model",
        instructions="Be concise.",
        max_output_tokens=64,
        client=client,
    )

    assert adapter("hello") == "openai answer"
    assert calls == [
        {
            "model": "test-openai-model",
            "input": "hello",
            "instructions": "Be concise.",
            "max_output_tokens": 64,
        }
    ]


def test_anthropic_messages_adapter_uses_messages_api():
    calls = []

    class FakeMessages:
        def create(self, **kwargs):
            calls.append(kwargs)
            return SimpleNamespace(content=[SimpleNamespace(text="anthropic answer")])

    client = SimpleNamespace(messages=FakeMessages())
    adapter = AnthropicMessagesAdapter(
        model="test-anthropic-model",
        system="Be concise.",
        max_tokens=64,
        client=client,
    )

    assert adapter("hello") == "anthropic answer"
    assert calls == [
        {
            "model": "test-anthropic-model",
            "max_tokens": 64,
            "messages": [{"role": "user", "content": "hello"}],
            "system": "Be concise.",
        }
    ]


def test_state_model_adapters_include_memory_context():
    openai_prompts = []
    anthropic_prompts = []

    openai = lambda prompt: openai_prompts.append(prompt) or "openai"
    anthropic = lambda prompt: anthropic_prompts.append(prompt) or "anthropic"
    state = {
        "user_message": "What now?",
        "context": {"memories": [{"content": "Use short plans."}]},
    }

    assert openai_state_model(openai)(state) == "openai"
    assert anthropic_state_model(anthropic)(state) == "anthropic"
    assert "Use short plans." in openai_prompts[0]
    assert "What now?" in anthropic_prompts[0]


def test_agentops_schema_installs_all_tables():
    db = FakeDb()
    install_agentops_schema(db)

    assert len(db.sql) == len(AGENTOPS_DDL)
    assert all(sql.startswith("CREATE TABLE IF NOT EXISTS") for sql in db.sql)
    assert any("agent_runs" in sql for sql in db.sql)
    assert any("llm_calls" in sql for sql in db.sql)
    assert any("llm_errors" in sql for sql in db.sql)
    assert any("context_traces" in sql for sql in db.sql)
    assert any("context_replay_events" in sql for sql in db.sql)


def test_context_trace_explains_selected_memories_and_replay_snapshot():
    context = {
        "memories": [
            {
                "record": {
                    "memory_id": "mem_1",
                    "token_count": 8,
                    "importance": 2.0,
                    "access_count": 3,
                    "pinned": True,
                },
                "score": 8.25,
                "similarity": 0.91,
            }
        ]
    }
    replay = [
        {
            "source_table": "service_metrics",
            "query": "SELECT count(*) FROM service_metrics",
            "row_count": 42,
        }
    ]

    trace_sql = build_context_trace_sql(
        context,
        run_id="run_1",
        tenant_id="tenant_a",
        timestamp_ns=123,
    )
    replay_sql = build_context_replay_sql(
        replay,
        run_id="run_1",
        tenant_id="tenant_a",
        timestamp_ns=123,
    )

    assert explain_memory_selection(context["memories"][0]) == (
        "pinned+semantic_match+importance+prior_use+token_budget_fit"
    )
    assert len(trace_sql) == 1
    assert "INSERT INTO context_traces" in trace_sql[0]
    assert "'mem_1'" in trace_sql[0]
    assert "8250000" in trace_sql[0]
    assert len(replay_sql) == 1
    assert "INSERT INTO context_replay_events" in replay_sql[0]
    assert "'service_metrics'" in replay_sql[0]


def test_otel_mapping_maps_genai_tokens_cache_and_latency():
    span = {
        "traceId": "trace_1",
        "spanId": "span_llm",
        "name": "gen_ai.chat",
        "startTimeUnixNano": "1000000000",
        "endTimeUnixNano": "1123000000",
        "attributes": [
            {"key": "tenant.id", "value": {"stringValue": "tenant_a"}},
            {"key": "agent.run_id", "value": {"stringValue": "run_1"}},
            {"key": "gen_ai.system", "value": {"stringValue": "openai"}},
            {"key": "gen_ai.request.model", "value": {"stringValue": "gpt-4.1"}},
            {"key": "gen_ai.usage.input_tokens", "value": {"intValue": "12"}},
            {"key": "gen_ai.usage.output_tokens", "value": {"intValue": "7"}},
            {"key": "gen_ai.cache.hit", "value": {"boolValue": True}},
            {"key": "gen_ai.cache.type", "value": {"stringValue": "semantic"}},
        ],
    }

    sql = map_span_to_agentops_sql(span)

    assert any("INSERT INTO llm_calls" in row for row in sql)
    assert any("'openai'" in row and "'gpt-4.1'" in row for row in sql)
    assert any("12" in row and "7" in row and "123" in row for row in sql)
    assert any("INSERT INTO cache_events" in row and "'semantic'" in row for row in sql)


def test_otel_mapping_maps_tool_calls_and_model_errors():
    spans = [
        {
            "traceId": "trace_2",
            "spanId": "span_tool",
            "startTimeUnixNano": "2000000000",
            "endTimeUnixNano": "2010000000",
            "attributes": {
                "agent.run_id": "run_2",
                "tenant.id": "tenant_a",
                "gen_ai.tool.name": "inventory_lookup",
                "gen_ai.tool.call.id": "tool_1",
                "tool.status": "ok",
            },
        },
        {
            "traceId": "trace_2",
            "spanId": "span_error",
            "startTimeUnixNano": "3000000000",
            "endTimeUnixNano": "3005000000",
            "status": {"code": "STATUS_CODE_ERROR", "message": "rate_limit"},
            "attributes": {
                "agent.run_id": "run_2",
                "tenant.id": "tenant_a",
                "gen_ai.system": "openai",
                "gen_ai.response.model": "gpt-4.1",
                "error.type": "rate_limit",
            },
        },
    ]

    sql = map_spans_to_agentops_sql(spans)

    assert any("INSERT INTO tool_calls" in row and "'inventory_lookup'" in row for row in sql)
    assert any("INSERT INTO llm_errors" in row and "'rate_limit'" in row for row in sql)
    assert any("INSERT INTO llm_calls" in row and "'span_error'" in row for row in sql)


def test_production_agent_demo_records_memory_cache_and_telemetry():
    db = FakeDb()
    provider_calls = []

    def provider(prompt):
        provider_calls.append(prompt)
        return "production answer"

    result = run_production_turn(
        db,
        "What changed in Agent Memory?",
        provider=provider,
        provider_name="mock",
        model_name="mock-model",
    )

    assert result.response == "production answer"
    assert result.response_source == "provider"
    assert result.memory_id == "mem_1"
    assert result.cache_id == "cache_1"
    assert result.context_rows == 1
    assert provider_calls
    assert any("INSERT INTO agent_runs" in sql for sql in db.sql)
    assert any("INSERT INTO retrieval_events" in sql for sql in db.sql)
    assert any("INSERT INTO cache_events" in sql for sql in db.sql)
    assert any("INSERT INTO llm_calls" in sql for sql in db.sql)
    assert any("INSERT INTO tool_calls" in sql for sql in db.sql)
    assert db.memory.stored[0]["type"] == "turn"


def test_agent_attached_timeseries_demo_runs_all_verticals():
    db = FakeDb()
    results = run_all_use_cases(db)

    assert {result.domain for result in results} == {
        "finance",
        "iot",
        "observability",
        "robotics",
        "game_liveops",
    }
    assert len(results) == len(USE_CASES)
    assert len(db.queries) == len(USE_CASES)
    assert all(result.inserted_rows == len(use_case.rows) for result, use_case in zip(results, USE_CASES))
    assert all(result.context_count >= 1 for result in results)
    assert all("Current time-series query" in result.prompt for result in results)
    assert any("INSERT INTO finance_ticks" in sql for sql in db.sql)


def test_agent_attached_timeseries_demo_stores_scoped_memory_metadata():
    db = FakeDb()
    result = run_use_case(db, USE_CASES[0])

    assert result.memory_ids == ["mem_1", "mem_2"]
    assert db.memory.stored[0]["namespace"] == "finance_hft"
    assert db.memory.stored[0]["agent_id"] == "execution_agent"
    assert db.memory.stored[0]["pinned"] is True
    metadata = db.memory.stored[0]["metadata_json"]
    assert '"demo": "agent_attached_timeseries"' in metadata
    assert '"domain": "finance"' in metadata
    assert '"table": "finance_ticks"' in metadata


def test_agent_attached_timeseries_demo_handles_empty_rows():
    db = FakeDb()
    use_case = USE_CASES[0].__class__(
        **{
            **USE_CASES[0].__dict__,
            "name": "empty_finance_hft",
            "table_name": "empty_finance_ticks",
            "ddl": USE_CASES[0].ddl.replace("finance_ticks", "empty_finance_ticks"),
            "rows": (),
        }
    )

    result = run_use_case(db, use_case, execute_query=False)

    assert result.inserted_rows == 0
    assert result.query_row_count is None
    assert db.queries == []
    assert not any(sql.startswith("INSERT INTO empty_finance_ticks") for sql in db.sql)


def test_agent_attached_timeseries_demo_keeps_timestamp_zero_valid():
    assert USE_CASES[0].rows[0]["timestamp"] == 0
