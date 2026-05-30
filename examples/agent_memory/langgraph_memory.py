"""LangGraph-style Agent Memory example for ZeptoDB.

The functions are shaped like graph nodes operating on a shared state dict, but
the file does not require LangGraph at runtime. In a real LangGraph app, wire
these functions into StateGraph nodes and replace ``mock_agent_response`` with
your model call.
"""

from __future__ import annotations

import json
from typing import Callable, Dict, List, Protocol, TypedDict

from zepto_py import ZeptoConnection

from .provider_cache import deterministic_embedding


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


class MemoryConnection(Protocol):
    memory: MemoryClient


class AgentState(TypedDict, total=False):
    tenant_id: str
    namespace: str
    user_id: str
    session_id: str
    agent_id: str
    user_message: str
    context: Dict
    response: str
    stored_memory_id: str


def retrieve_context_node(
    state: AgentState,
    db: MemoryConnection,
    embed: Callable[[str], List[float]] = deterministic_embedding,
) -> AgentState:
    """Fetch ranked memories for the current user/session turn."""
    query = state["user_message"]
    context = db.memory.get_context(
        query_embedding=embed(query),
        token_budget=256,
        namespace=state.get("namespace", "agent"),
        tenant_id=state.get("tenant_id", "demo"),
        user_id=state.get("user_id", ""),
        session_id=state.get("session_id", ""),
        agent_id=state.get("agent_id", "assistant"),
        limit=8,
    )
    return {**state, "context": context}


def mock_agent_response(state: AgentState) -> str:
    memories = state.get("context", {}).get("memories", [])
    facts = [m.get("content", "") for m in memories if m.get("content")]
    if facts:
        return f"Using remembered context: {facts[0]}"
    return f"No prior context found. Answering: {state['user_message']}"


def call_model_node(
    state: AgentState,
    model: Callable[[AgentState], str] = mock_agent_response,
) -> AgentState:
    """Run the model step. The default model is a deterministic stub."""
    return {**state, "response": model(state)}


def remember_turn_node(
    state: AgentState,
    db: MemoryConnection,
    embed: Callable[[str], List[float]] = deterministic_embedding,
) -> AgentState:
    """Store a compact turn memory after the model response."""
    content = f"User asked: {state['user_message']} | Assistant answered: {state['response']}"
    memory_id = db.memory.put(
        content,
        embedding=embed(content),
        namespace=state.get("namespace", "agent"),
        tenant_id=state.get("tenant_id", "demo"),
        user_id=state.get("user_id", ""),
        session_id=state.get("session_id", ""),
        agent_id=state.get("agent_id", "assistant"),
        type="turn",
        metadata_json=json.dumps({"source": "langgraph_memory_example"}),
        importance=0.5,
    )
    return {**state, "stored_memory_id": memory_id}


def run_turn(
    db: MemoryConnection,
    user_message: str,
    tenant_id: str = "demo",
    namespace: str = "agent",
    user_id: str = "user_1",
    session_id: str = "session_1",
    agent_id: str = "assistant",
) -> AgentState:
    """Run one graph-like turn: retrieve context, call model, store memory."""
    state: AgentState = {
        "tenant_id": tenant_id,
        "namespace": namespace,
        "user_id": user_id,
        "session_id": session_id,
        "agent_id": agent_id,
        "user_message": user_message,
    }
    state = retrieve_context_node(state, db)
    state = call_model_node(state)
    state = remember_turn_node(state, db)
    return state


def main() -> None:
    db = ZeptoConnection("localhost", 8123)
    state = run_turn(db, "Remember that I prefer short implementation plans.")
    print(state["response"])
    print(f"stored_memory_id={state['stored_memory_id']}")


if __name__ == "__main__":
    main()
