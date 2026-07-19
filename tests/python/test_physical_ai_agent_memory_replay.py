"""Focused tests for the Physical AI Agent Memory replay harness."""

from __future__ import annotations

import json
import math
import sys
from copy import deepcopy
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parent.parent.parent
TOOLS = ROOT / "docs" / "research" / "tools"
sys.path.insert(0, str(TOOLS))

import physical_ai_agent_memory_replay as replay  # noqa: E402


def fixture_episodes():
    return replay.load_fixture(replay.DEFAULT_FIXTURE)


def test_observation_embedding_is_normalized_and_excludes_action_outcome():
    episode = fixture_episodes()[0]
    changed = deepcopy(episode)
    changed["action"]["action_class"] = "different_action"
    changed["machine_outcome_label"]["label"] = "failure"
    changed["reflection"] = "different retrospective text"

    original_embedding = replay.observation_embedding(episode)
    changed_embedding = replay.observation_embedding(changed)

    assert original_embedding == changed_embedding
    assert len(original_embedding) == replay.EMBEDDING_DIM
    assert math.isclose(sum(value * value for value in original_embedding), 1.0)


def test_observation_embedding_rejects_zero_dimensions():
    with pytest.raises(ValueError, match="dimensions"):
        replay.observation_embedding(fixture_episodes()[0], dimensions=0)


def test_load_fixture_rejects_empty_episode_array(tmp_path):
    path = tmp_path / "empty.json"
    path.write_text(json.dumps({"episodes": []}))

    with pytest.raises(ValueError, match="non-empty"):
        replay.load_fixture(path)


def test_percentile_uses_nearest_rank_and_rejects_invalid_quantile():
    assert replay.percentile([4.0, 1.0, 3.0, 2.0], 0.50) == 2.0
    assert replay.percentile([4.0, 1.0, 3.0, 2.0], 0.95) == 4.0
    with pytest.raises(ValueError, match="quantile"):
        replay.percentile([1.0], 1.1)


def test_agent_memory_client_surfaces_connection_error(monkeypatch):
    def fail(*_args, **_kwargs):
        raise replay.urllib.error.URLError("connection refused")

    monkeypatch.setattr(replay.urllib.request, "urlopen", fail)
    client = replay.AgentMemoryClient("http://127.0.0.1:1", timeout=0.01)

    with pytest.raises(RuntimeError, match="connection refused"):
        client.request("GET", "/health")
