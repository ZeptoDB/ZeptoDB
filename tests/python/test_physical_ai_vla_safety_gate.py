from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = (
    ROOT / "docs" / "research" / "tools" / "physical_ai_vla_safety_gate.py"
)
SPEC = importlib.util.spec_from_file_location("physical_ai_vla_safety_gate", MODULE_PATH)
assert SPEC and SPEC.loader
safety_gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(safety_gate)


def _memory(command, width):
    return {
        "state": [0.0] * 6 + [width / 2.0, -width / 2.0],
        "action": [0.0] * 6 + [float(command)],
    }


def test_derive_gripper_boundary_uses_both_command_states():
    assert safety_gate.derive_gripper_boundary(
        [_memory(-1, 0.08), _memory(-1, 0.06), _memory(1, 0.02)]
    ) == pytest.approx(0.045)


@pytest.mark.parametrize(
    "memories",
    [[], [_memory(-1, 0.08)], [{"state": [], "action": []}]],
)
def test_derive_gripper_boundary_rejects_incomplete_input(memories):
    with pytest.raises(ValueError):
        safety_gate.derive_gripper_boundary(memories)


def test_candidate_disagreement_rejects_empty_matches():
    with pytest.raises(ValueError, match="must not be empty"):
        safety_gate.candidate_disagreement([], [0.0] * 7, [1.0] * 7)


def _safety(**overrides):
    values = {
        "action": [0.1, 0.1, 0.1, 0.01, 0.01, 0.01, -1.0],
        "state": [0.0] * 6 + [0.04, -0.04],
        "state_means": [0.0] * 8,
        "state_stds": [1.0] * 8,
        "disagreement": 0.1,
        "gripper_consensus": True,
        "robot_contacts": 0,
        "gripper_boundary": 0.05,
        "translation_limit": 0.75,
        "rotation_limit": 0.15,
        "disagreement_limit": 0.3,
        "state_z_limit": 3.0,
    }
    values.update(overrides)
    return safety_gate.safety_index(**values)


def test_safety_index_allows_bounded_gripper_hold():
    result = _safety()
    assert result["level"] == "low"
    assert not result["gripper_transition"]


@pytest.mark.parametrize(
    "overrides",
    [
        {"robot_contacts": 1},
        {"gripper_consensus": False},
        {"action": [0.1] * 6 + [1.0]},
    ],
)
def test_safety_index_marks_hard_risk(overrides):
    assert _safety(**overrides)["level"] == "high"


def test_safety_index_marks_soft_limit_as_medium():
    assert _safety(action=[0.8, 0.0, 0.0, 0.0, 0.0, 0.0, -1.0])[
        "level"
    ] == "medium"


def test_safety_index_rejects_bad_dimensions_and_limits():
    with pytest.raises(ValueError, match="dimensions"):
        _safety(action=[])
    with pytest.raises(ValueError, match="positive"):
        _safety(translation_limit=0.0)


def test_route_dual_gate_keeps_confidence_and_safety_separate():
    assert safety_gate.route_dual_gate(
        confidence=0.8,
        margin=0.02,
        safety_level="low",
        previous_skip=False,
        threshold=0.76,
        min_margin=0.01,
    ) == (True, "accepted")
    assert safety_gate.route_dual_gate(
        confidence=0.8,
        margin=0.02,
        safety_level="high",
        previous_skip=False,
        threshold=0.76,
        min_margin=0.01,
    ) == (False, "safety")
    assert safety_gate.route_dual_gate(
        confidence=0.7,
        margin=0.02,
        safety_level="low",
        previous_skip=False,
        threshold=0.76,
        min_margin=0.01,
    ) == (False, "confidence")


def test_route_dual_gate_forces_cooldown_after_skip():
    assert safety_gate.route_dual_gate(
        confidence=0.8,
        margin=0.02,
        safety_level="low",
        previous_skip=True,
        threshold=0.76,
        min_margin=0.01,
    ) == (False, "cooldown")


def test_policy_seed_is_absolute_step_stable():
    assert safety_gate.policy_seed(300000, 5, 12) == 350012
    assert safety_gate.policy_seed(300000, 5, 12) == 350012
    with pytest.raises(ValueError):
        safety_gate.policy_seed(300000, -1, 0)


def test_write_result_rejects_termination_message_overflow(tmp_path):
    with pytest.raises(RuntimeError, match="termination-message limit"):
        safety_gate._write_result(
            tmp_path / "result.json", {"payload": "x" * 3900}
        )
