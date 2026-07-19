from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np
import pytest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "docs" / "research" / "tools" / "physical_ai_vla_closed_loop.py"
SPEC = importlib.util.spec_from_file_location("physical_ai_vla_closed_loop", MODULE_PATH)
assert SPEC and SPEC.loader
closed_loop = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(closed_loop)


def test_route_action_requires_stable_confidence_and_bounds_consecutive_skips():
    skip, streak = closed_loop.route_action(
        confidence=0.95,
        threshold=0.90,
        task_match=True,
        high_confidence_streak=0,
        consecutive_skips=0,
        required_streak=2,
        max_consecutive_skips=4,
    )
    assert not skip
    assert streak == 1

    skip, streak = closed_loop.route_action(
        confidence=0.95,
        threshold=0.90,
        task_match=True,
        high_confidence_streak=streak,
        consecutive_skips=0,
        required_streak=2,
        max_consecutive_skips=4,
    )
    assert skip
    assert streak == 2

    skip, _ = closed_loop.route_action(
        confidence=0.95,
        threshold=0.90,
        task_match=True,
        high_confidence_streak=streak,
        consecutive_skips=4,
        required_streak=2,
        max_consecutive_skips=4,
    )
    assert not skip


def test_route_action_falls_back_on_task_mismatch_or_low_confidence():
    for confidence, task_match in ((0.95, False), (0.89, True)):
        skip, streak = closed_loop.route_action(
            confidence=confidence,
            threshold=0.90,
            task_match=task_match,
            high_confidence_streak=3,
            consecutive_skips=0,
            required_streak=2,
            max_consecutive_skips=4,
        )
        assert not skip
        assert streak == 0


def test_route_action_rejects_invalid_limits():
    with pytest.raises(ValueError, match="must be positive"):
        closed_loop.route_action(
            confidence=1.0,
            threshold=0.9,
            task_match=True,
            high_confidence_streak=0,
            consecutive_skips=0,
            required_streak=0,
            max_consecutive_skips=4,
        )


def test_extract_success_handles_vector_final_info_and_empty_values():
    assert closed_loop.extract_success(
        {"final_info": {"is_success": np.array([True])}}
    )
    assert not closed_loop.extract_success({"is_success": []})
    assert not closed_loop.extract_success({})


def test_paired_quality_counts_regressions_and_improvements():
    quality = closed_loop.paired_quality(
        [
            {"baseline_success": True, "routed_success": True},
            {"baseline_success": True, "routed_success": False},
            {"baseline_success": False, "routed_success": True},
            {"baseline_success": False, "routed_success": False},
        ]
    )
    assert quality["baseline_success_rate"] == 0.5
    assert quality["routed_success_rate"] == 0.5
    assert quality["success_rate_delta"] == 0.0
    assert quality["paired_regressions"] == 1
    assert quality["paired_improvements"] == 1


def test_paired_quality_rejects_empty_input():
    with pytest.raises(ValueError, match="must not be empty"):
        closed_loop.paired_quality([])


def test_resolve_memory_task_index_uses_text_not_suite_order():
    assert (
        closed_loop.resolve_memory_task_index(
            "put both items in the basket",
            {"put both items in the basket": 5},
        )
        == 5
    )

    with pytest.raises(RuntimeError, match="absent from memory bank"):
        closed_loop.resolve_memory_task_index("unknown task", {})


def test_seed_policy_rng_seeds_cpu_and_cuda():
    calls = []

    class FakeCuda:
        @staticmethod
        def manual_seed_all(seed):
            calls.append(("cuda", seed))

    class FakeTorch:
        cuda = FakeCuda()

        @staticmethod
        def manual_seed(seed):
            calls.append(("cpu", seed))

    closed_loop.seed_policy_rng(27, FakeTorch())
    assert calls == [("cpu", 27), ("cuda", 27)]


def test_write_result_rejects_termination_message_overflow(tmp_path):
    with pytest.raises(RuntimeError, match="termination-message limit"):
        closed_loop._write_result(tmp_path / "result.json", {"payload": "x" * 3900})
