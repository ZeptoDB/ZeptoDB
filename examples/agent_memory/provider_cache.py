"""Application-level provider cache example for ZeptoDB Agent Memory.

This example intentionally does not call OpenAI, Anthropic, or any other model
provider. It shows the control flow an application would run before a provider
call:

1. Embed the prompt in the application.
2. Lookup ZeptoDB exact/semantic cache.
3. Call the provider only on a miss.
4. Store the response for future turns.
"""

from __future__ import annotations

import hashlib
import math
from typing import Callable, Dict, List, Protocol

from zepto_py import ZeptoConnection


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


class CacheConnection(Protocol):
    cache: CacheClient


def deterministic_embedding(text: str, dim: int = 16) -> List[float]:
    """Small deterministic embedding stub for examples and tests."""
    digest = hashlib.sha256(text.encode("utf-8")).digest()
    values = []
    for i in range(dim):
        byte = digest[i % len(digest)]
        values.append((float(byte) / 127.5) - 1.0)
    norm = math.sqrt(sum(v * v for v in values))
    if norm == 0.0:
        return values
    return [v / norm for v in values]


def mock_provider(prompt: str) -> str:
    """Provider stand-in. Replace this with an OpenAI/Anthropic call."""
    return f"Mock provider answer for: {prompt}"


def answer_with_cache(
    db: CacheConnection,
    prompt: str,
    tenant_id: str = "demo",
    namespace: str = "agent",
    semantic_threshold: float = 0.92,
    provider: Callable[[str], str] = mock_provider,
    embed: Callable[[str], List[float]] = deterministic_embedding,
) -> Dict:
    """Answer a prompt using ZeptoDB as an app-level exact/semantic cache."""
    embedding = embed(prompt)
    hit = db.cache.lookup(
        prompt,
        embedding=embedding,
        namespace=namespace,
        tenant_id=tenant_id,
        semantic_threshold=semantic_threshold,
    )
    if hit.get("hit"):
        entry = hit.get("entry", {})
        return {
            "source": "cache",
            "kind": hit.get("kind", "semantic"),
            "response": entry.get("response", ""),
            "score": hit.get("score", 0.0),
        }

    response = provider(prompt)
    cache_id = db.cache.store(
        prompt,
        response,
        embedding=embedding,
        namespace=namespace,
        tenant_id=tenant_id,
        metadata_json='{"source":"provider_cache_example"}',
    )
    return {
        "source": "provider",
        "cache_id": cache_id,
        "response": response,
    }


def main() -> None:
    db = ZeptoConnection("localhost", 8123)
    prompt = "Summarize the user's ZeptoDB Agent Memory preferences."
    first = answer_with_cache(db, prompt)
    second = answer_with_cache(db, prompt)
    print(first)
    print(second)


if __name__ == "__main__":
    main()
