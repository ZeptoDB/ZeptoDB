"""Optional provider/framework adapters for ZeptoDB Agent Memory examples.

These adapters are deliberately thin and lazy-import optional dependencies.
ZeptoDB remains responsible for memory/cache storage; providers remain
application-owned.
"""

from __future__ import annotations

from typing import Callable, Dict, List, Optional

from .langgraph_memory import (
    AgentState,
    call_model_node,
    remember_turn_node,
    retrieve_context_node,
)


class OpenAIResponsesAdapter:
    """Callable provider adapter for the OpenAI Responses API.

    Pass an explicit model string from your application config. The adapter uses
    ``response.output_text`` when available, matching the Python SDK convenience
    property for Responses.
    """

    def __init__(
        self,
        model: str,
        instructions: str = "",
        max_output_tokens: Optional[int] = None,
        client: object = None,
    ):
        if not model:
            raise ValueError("model is required")
        if client is None:
            try:
                from openai import OpenAI
            except ImportError as exc:
                raise ImportError("Install openai to use OpenAIResponsesAdapter") from exc
            client = OpenAI()
        self.client = client
        self.model = model
        self.instructions = instructions
        self.max_output_tokens = max_output_tokens

    def __call__(self, prompt: str) -> str:
        kwargs = {
            "model": self.model,
            "input": prompt,
        }
        if self.instructions:
            kwargs["instructions"] = self.instructions
        if self.max_output_tokens is not None:
            kwargs["max_output_tokens"] = self.max_output_tokens
        response = self.client.responses.create(**kwargs)
        output_text = getattr(response, "output_text", "")
        if output_text:
            return output_text
        return str(response)


class AnthropicMessagesAdapter:
    """Callable provider adapter for Anthropic's Messages API."""

    def __init__(
        self,
        model: str,
        system: str = "",
        max_tokens: int = 1024,
        client: object = None,
    ):
        if not model:
            raise ValueError("model is required")
        if client is None:
            try:
                from anthropic import Anthropic
            except ImportError as exc:
                raise ImportError("Install anthropic to use AnthropicMessagesAdapter") from exc
            client = Anthropic()
        self.client = client
        self.model = model
        self.system = system
        self.max_tokens = max_tokens

    def __call__(self, prompt: str) -> str:
        kwargs = {
            "model": self.model,
            "max_tokens": self.max_tokens,
            "messages": [{"role": "user", "content": prompt}],
        }
        if self.system:
            kwargs["system"] = self.system
        message = self.client.messages.create(**kwargs)
        parts = []
        for block in getattr(message, "content", []):
            text = getattr(block, "text", None)
            if text is None and isinstance(block, dict):
                text = block.get("text")
            if text:
                parts.append(text)
        if parts:
            return "\n".join(parts)
        return str(message)


def openai_state_model(provider: OpenAIResponsesAdapter) -> Callable[[AgentState], str]:
    """Adapt a prompt-only OpenAI provider into a LangGraph-style state model."""

    def model(state: AgentState) -> str:
        return provider(_prompt_from_state(state))

    return model


def anthropic_state_model(provider: AnthropicMessagesAdapter) -> Callable[[AgentState], str]:
    """Adapt a prompt-only Anthropic provider into a LangGraph-style state model."""

    def model(state: AgentState) -> str:
        return provider(_prompt_from_state(state))

    return model


def build_langgraph_app(
    db,
    model: Callable[[AgentState], str],
    embed: Callable[[str], List[float]],
):
    """Build a LangGraph app around the example retrieve/call/remember nodes.

    Requires ``langgraph`` to be installed. The returned object is a compiled
    graph and can be invoked with the same state dict accepted by ``run_turn``.
    """
    try:
        from langgraph.graph import END, START, StateGraph
    except ImportError as exc:
        raise ImportError("Install langgraph to use build_langgraph_app") from exc

    graph = StateGraph(AgentState)
    graph.add_node("retrieve_context", lambda state: retrieve_context_node(state, db, embed))
    graph.add_node("call_model", lambda state: call_model_node(state, model))
    graph.add_node("remember_turn", lambda state: remember_turn_node(state, db, embed))
    graph.add_edge(START, "retrieve_context")
    graph.add_edge("retrieve_context", "call_model")
    graph.add_edge("call_model", "remember_turn")
    graph.add_edge("remember_turn", END)
    return graph.compile()


def _prompt_from_state(state: AgentState) -> str:
    memories = state.get("context", {}).get("memories", [])
    context_lines = []
    for memory in memories:
        content = memory.get("content") if isinstance(memory, dict) else None
        if content:
            context_lines.append(f"- {content}")
    context = "\n".join(context_lines) if context_lines else "(none)"
    return (
        "Use the retrieved memories when they are relevant.\n\n"
        f"Retrieved memories:\n{context}\n\n"
        f"User message:\n{state['user_message']}"
    )
