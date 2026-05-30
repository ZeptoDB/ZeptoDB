"""Production-shaped Agent Memory demo.

This demo combines three ZeptoDB surfaces:

- Agent Memory context retrieval
- exact/semantic provider cache
- AgentOps telemetry tables stored as ordinary ZeptoDB time-series data

The default provider is deterministic and local. To use a real provider, pass an
adapter from ``examples.agent_memory.adapters``.
"""

from __future__ import annotations

import json
import time
import uuid
from dataclasses import dataclass
from typing import Callable, Dict, List, Protocol

from zepto_py import ZeptoConnection

from .agentops_schema import install_agentops_schema, insert_sql
from .provider_cache import deterministic_embedding, mock_provider


class MemoryClient(Protocol):
    def get_context(
        self,
        query_embedding: List[float],
        token_budget: int,
        namespace: str,
        tenant_id: str,
        user_id: str,
        session_id: str,
        agent_id: str,
        limit: int,
    ) -> Dict:
        ...

    def put(
        self,
        content: str,
        embedding: List[float],
        namespace: str,
        tenant_id: str,
        user_id: str,
        session_id: str,
        agent_id: str,
        type: str = "memory",
        metadata_json: str = "{}",
        importance: float = 0.0,
    ) -> str:
        ...


class CacheClient(Protocol):
    def lookup(
        self,
        prompt: str,
        embedding: List[float],
        namespace: str,
        tenant_id: str,
        semantic_threshold: float,
    ) -> Dict:
        ...

    def store(
        self,
        prompt: str,
        response: str,
        embedding: List[float],
        namespace: str,
        tenant_id: str,
        metadata_json: str = "{}",
        token_count: int = 0,
    ) -> str:
        ...


class ProductionDb(Protocol):
    memory: MemoryClient
    cache: CacheClient

    def execute(self, sql: str) -> int:
        ...


@dataclass
class AgentTurnResult:
    run_id: str
    response: str
    response_source: str
    memory_id: str
    cache_id: str
    context_rows: int
    context_tokens: int


def run_production_turn(
    db: ProductionDb,
    user_message: str,
    tenant_id: str = "demo",
    namespace: str = "agent",
    user_id: str = "user_1",
    session_id: str = "session_1",
    agent_id: str = "assistant",
    provider_name: str = "mock",
    model_name: str = "mock-local",
    provider: Callable[[str], str] = mock_provider,
    embed: Callable[[str], List[float]] = deterministic_embedding,
    install_schema: bool = True,
) -> AgentTurnResult:
    """Run one production-shaped agent turn and record telemetry."""
    if install_schema:
        install_agentops_schema(db)

    run_id = _id("run")
    started_at = _now_ns()
    _execute(db, insert_sql(
        "agent_runs",
        [
            "run_id",
            "tenant_id",
            "user_id",
            "session_id",
            "agent_id",
            "started_at_ns",
            "ended_at_ns",
            "status",
        ],
        [run_id, tenant_id, user_id, session_id, agent_id, started_at, 0, "started"],
    ))

    query_embedding = embed(user_message)

    retrieval_start = time.perf_counter()
    context = db.memory.get_context(
        query_embedding=query_embedding,
        token_budget=512,
        namespace=namespace,
        tenant_id=tenant_id,
        user_id=user_id,
        session_id=session_id,
        agent_id=agent_id,
        limit=8,
    )
    retrieval_ms = _elapsed_ms(retrieval_start)
    context_rows = len(context.get("memories", []))
    context_tokens = int(context.get("token_count", 0))
    _execute(db, insert_sql(
        "retrieval_events",
        ["event_id", "run_id", "tenant_id", "memory_count", "token_count", "latency_ms", "timestamp_ns"],
        [_id("retrieval"), run_id, tenant_id, context_rows, context_tokens, retrieval_ms, _now_ns()],
    ))

    prompt = _assemble_prompt(user_message, context)
    cache_start = time.perf_counter()
    cache_hit = db.cache.lookup(
        prompt,
        embedding=embed(prompt),
        namespace=namespace,
        tenant_id=tenant_id,
        semantic_threshold=0.92,
    )
    cache_ms = _elapsed_ms(cache_start)
    hit = bool(cache_hit.get("hit"))
    cache_kind = str(cache_hit.get("kind", "miss") if hit else "miss")
    _execute(db, insert_sql(
        "cache_events",
        ["event_id", "run_id", "tenant_id", "cache_hit", "cache_kind", "latency_ms", "timestamp_ns"],
        [_id("cache"), run_id, tenant_id, hit, cache_kind, cache_ms, _now_ns()],
    ))

    cache_id = ""
    if hit:
        response = str(cache_hit.get("entry", {}).get("response", ""))
        response_source = "cache"
        llm_ms = 0
    else:
        llm_start = time.perf_counter()
        response = provider(prompt)
        llm_ms = _elapsed_ms(llm_start)
        cache_id = db.cache.store(
            prompt,
            response,
            embedding=embed(prompt),
            namespace=namespace,
            tenant_id=tenant_id,
            metadata_json=json.dumps({"source": "production_agent_demo", "run_id": run_id}),
        )
        response_source = "provider"

    _execute(db, insert_sql(
        "llm_calls",
        [
            "call_id",
            "run_id",
            "tenant_id",
            "provider",
            "model",
            "prompt_tokens",
            "completion_tokens",
            "latency_ms",
            "cache_hit",
            "timestamp_ns",
        ],
        [
            _id("llm"),
            run_id,
            tenant_id,
            provider_name,
            model_name,
            _estimate_tokens(prompt),
            _estimate_tokens(response),
            llm_ms,
            hit,
            _now_ns(),
        ],
    ))

    memory_content = f"User asked: {user_message} | Assistant answered: {response}"
    memory_id = db.memory.put(
        memory_content,
        embedding=embed(memory_content),
        namespace=namespace,
        tenant_id=tenant_id,
        user_id=user_id,
        session_id=session_id,
        agent_id=agent_id,
        type="turn",
        metadata_json=json.dumps({"source": "production_agent_demo", "run_id": run_id}),
        importance=0.7,
    )
    _execute(db, insert_sql(
        "tool_calls",
        ["call_id", "run_id", "tenant_id", "tool_name", "status", "latency_ms", "timestamp_ns"],
        [_id("tool"), run_id, tenant_id, "memory.put", "ok", 0, _now_ns()],
    ))

    _execute(db, insert_sql(
        "agent_runs",
        [
            "run_id",
            "tenant_id",
            "user_id",
            "session_id",
            "agent_id",
            "started_at_ns",
            "ended_at_ns",
            "status",
        ],
        [run_id, tenant_id, user_id, session_id, agent_id, started_at, _now_ns(), "completed"],
    ))

    return AgentTurnResult(
        run_id=run_id,
        response=response,
        response_source=response_source,
        memory_id=memory_id,
        cache_id=cache_id,
        context_rows=context_rows,
        context_tokens=context_tokens,
    )


def _assemble_prompt(user_message: str, context: Dict) -> str:
    memories = context.get("memories", [])
    lines = []
    for memory in memories:
        content = memory.get("content") if isinstance(memory, dict) else None
        if content:
            lines.append(f"- {content}")
    memory_block = "\n".join(lines) if lines else "(none)"
    return (
        "Answer using relevant retrieved memories and current telemetry context.\n\n"
        f"Retrieved memories:\n{memory_block}\n\n"
        f"User message:\n{user_message}"
    )


def _execute(db: ProductionDb, sql: str) -> None:
    db.execute(sql)


def _estimate_tokens(text: str) -> int:
    return max(1, (len(text) + 3) // 4)


def _elapsed_ms(start: float) -> int:
    return max(0, int((time.perf_counter() - start) * 1000))


def _now_ns() -> int:
    return time.time_ns()


def _id(prefix: str) -> str:
    return f"{prefix}_{uuid.uuid4().hex[:16]}"


def main() -> None:
    db = ZeptoConnection("localhost", 8123)
    result = run_production_turn(
        db,
        "Summarize the current Agent Memory rollout risk.",
    )
    print(result)


if __name__ == "__main__":
    main()
