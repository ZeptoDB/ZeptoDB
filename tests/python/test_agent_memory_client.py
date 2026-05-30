"""Python client coverage for agent memory/cache helpers."""

from zepto_py.connection import ZeptoConnection


def test_memory_client_put_search_and_context(monkeypatch):
    db = ZeptoConnection("localhost", 8123)
    calls = []

    def fake_post_json(path, payload):
        calls.append((path, payload))
        if path == "/api/ai/memories":
            return {"memory_id": "mem_1"}
        if path == "/api/ai/memories/search":
            return {"matches": [{"memory_id": "mem_1"}]}
        if path == "/api/ai/context":
            return {"memories": [{"memory_id": "mem_1"}], "token_count": 4}
        raise AssertionError(path)

    monkeypatch.setattr(db, "_post_json", fake_post_json)

    assert db.memory.put(
        "remember this",
        embedding=[1.0, 0.0],
        tenant_id="t1",
        namespace="agent",
        user_id="u1",
    ) == "mem_1"
    assert db.memory.search([1.0, 0.0], tenant_id="t1")[0]["memory_id"] == "mem_1"
    assert db.memory.get_context([1.0, 0.0], token_budget=4)["token_count"] == 4

    assert calls[0][0] == "/api/ai/memories"
    assert calls[0][1]["content"] == "remember this"
    assert calls[1][1]["query_embedding"] == [1.0, 0.0]


def test_cache_client_store_and_lookup(monkeypatch):
    db = ZeptoConnection("localhost", 8123)

    def fake_post_json(path, payload):
        if path == "/api/ai/cache/store":
            assert payload["prompt"] == "hi"
            assert payload["response"] == "hello"
            return {"cache_id": "cache_1"}
        if path == "/api/ai/cache/lookup":
            assert payload["semantic_threshold"] == 0.8
            return {"hit": True, "kind": "semantic"}
        raise AssertionError(path)

    monkeypatch.setattr(db, "_post_json", fake_post_json)

    assert db.cache.store("hi", "hello", embedding=[1.0, 0.0]) == "cache_1"
    assert db.cache.lookup("hey", embedding=[0.9, 0.1], semantic_threshold=0.8)["hit"]
