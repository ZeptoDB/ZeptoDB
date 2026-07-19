from __future__ import annotations

import importlib.util
import urllib.error
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "docs" / "research" / "tools" / "physical_ai_vision_retrieval.py"
SPEC = importlib.util.spec_from_file_location("physical_ai_vision_retrieval", MODULE_PATH)
assert SPEC and SPEC.loader
vision = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(vision)


def _fixtures(task_count: int = 2, episodes_per_task: int = 4):
    tasks = [
        {"task_index": task_index, "task": f"task {task_index}"}
        for task_index in range(task_count)
    ]
    episodes = []
    episode_index = 0
    for task in tasks:
        for local_index in range(episodes_per_task):
            episodes.append(
                {
                    "episode_index": episode_index,
                    "dataset_from_index": episode_index * 10,
                    "length": 9 + local_index,
                    "tasks": [task["task"]],
                }
            )
            episode_index += 1
    return episodes, tasks


def test_select_balanced_episodes_is_deterministic_and_disjoint():
    episodes, tasks = _fixtures()
    first = vision.select_balanced_episodes(
        episodes, tasks, memory_per_task=2, query_per_task=2, seed=25
    )
    second = vision.select_balanced_episodes(
        episodes, tasks, memory_per_task=2, query_per_task=2, seed=25
    )

    assert first == second
    memories, queries = first
    assert len(memories) == len(queries) == 4
    assert {row["episode_index"] for row in memories}.isdisjoint(
        row["episode_index"] for row in queries
    )
    assert all(row["row_index"] >= row["episode_index"] * 10 for row in memories + queries)


def test_select_balanced_episodes_rejects_insufficient_task_data():
    episodes, tasks = _fixtures(episodes_per_task=3)

    with pytest.raises(ValueError, match="but 4 are required"):
        vision.select_balanced_episodes(
            episodes, tasks, memory_per_task=2, query_per_task=2, seed=25
        )


def test_select_balanced_episodes_rejects_empty_partition():
    episodes, tasks = _fixtures()

    with pytest.raises(ValueError, match="must be positive"):
        vision.select_balanced_episodes(
            episodes, tasks, memory_per_task=0, query_per_task=2, seed=25
        )


def test_retrieval_metrics_handles_hits_and_misses():
    metrics = vision.retrieval_metrics(
        [[1, 0, 2], [0, 2, 1], [2, 1, 0]],
        [1, 1, 3],
    )

    assert metrics["recall_at_1"] == pytest.approx(1 / 3)
    assert metrics["recall_at_5"] == pytest.approx(2 / 3)
    assert metrics["mrr"] == pytest.approx((1.0 + 1 / 3 + 0.0) / 3)


def test_retrieval_metrics_handles_empty_input():
    assert vision.retrieval_metrics([], []) == {
        "recall_at_1": 0.0,
        "recall_at_5": 0.0,
        "mrr": 0.0,
    }


def test_retrieval_metrics_rejects_mismatched_lengths():
    with pytest.raises(ValueError, match="equal length"):
        vision.retrieval_metrics([[1]], [])


def test_write_result_rejects_termination_message_overflow(tmp_path):
    with pytest.raises(RuntimeError, match="termination-message limit"):
        vision._write_result(tmp_path / "result.json", {"payload": "x" * 3900})


def test_request_bytes_retries_transient_network_error(monkeypatch):
    calls = 0

    class Response:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def read(self):
            return b"ok"

    def urlopen(_request, timeout):
        nonlocal calls
        calls += 1
        assert timeout == 3.0
        if calls == 1:
            raise urllib.error.URLError("temporary")
        return Response()

    monkeypatch.setattr(vision.urllib.request, "urlopen", urlopen)
    monkeypatch.setattr(vision.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(vision.random, "uniform", lambda _low, _high: 0.0)

    assert vision._request_bytes("https://example.test", attempts=2, timeout=3.0) == b"ok"
    assert calls == 2


def test_load_sample_manifest_validates_balanced_counts(tmp_path):
    memories = [
        {"task_index": task, "image_url": f"https://example.test/m-{task}"}
        for task in range(10)
    ]
    queries = [
        {"task_index": task, "image_url": f"https://example.test/q-{task}"}
        for task in range(10)
    ]
    path = tmp_path / "samples.json"
    path.write_text(
        vision.json.dumps({"memories": memories, "queries": queries})
    )

    loaded_memories, loaded_queries = vision.load_sample_manifest(
        path, memory_per_task=1, query_per_task=1
    )

    assert loaded_memories == memories
    assert loaded_queries == queries


def test_load_sample_manifest_rejects_missing_image_url(tmp_path):
    rows = [{"task_index": task, "image_url": "https://example.test"} for task in range(10)]
    rows[3].pop("image_url")
    path = tmp_path / "samples.json"
    path.write_text(vision.json.dumps({"memories": rows, "queries": rows}))

    with pytest.raises(ValueError, match="without image_url"):
        vision.load_sample_manifest(path, memory_per_task=1, query_per_task=1)
