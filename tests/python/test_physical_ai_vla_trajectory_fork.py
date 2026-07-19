from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = (
    ROOT / "docs" / "research" / "tools" / "physical_ai_vla_trajectory_fork.py"
)
SPEC = importlib.util.spec_from_file_location(
    "physical_ai_vla_trajectory_fork", MODULE_PATH
)
assert SPEC and SPEC.loader
trajectory_fork = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(trajectory_fork)


def test_parse_regions_accepts_named_thresholds():
    assert trajectory_fork.parse_regions("broad:0.68:0,selected:0.76:0.01") == [
        {"name": "broad", "threshold": 0.68, "min_margin": 0.0},
        {"name": "selected", "threshold": 0.76, "min_margin": 0.01},
    ]


@pytest.mark.parametrize(
    "value",
    ["", "bad", "a:0.7", "a:0.7:0,a:0.8:0", "a:nan:0"],
)
def test_parse_regions_rejects_invalid_or_duplicate_input(value):
    with pytest.raises(ValueError):
        trajectory_fork.parse_regions(value)


def test_update_region_streaks_resets_regions_independently():
    regions = trajectory_fork.parse_regions("broad:0.68:0,strict:0.76:0.01")
    assert trajectory_fork.update_region_streaks(
        {"broad": 2, "strict": 2},
        regions,
        confidence=0.72,
        margin=0.005,
    ) == {"broad": 3, "strict": 0}


def test_update_region_streaks_rejects_mismatched_keys():
    with pytest.raises(ValueError, match="must match"):
        trajectory_fork.update_region_streaks(
            {},
            trajectory_fork.parse_regions("broad:0.68:0"),
            confidence=0.7,
            margin=0.0,
        )


def test_normalized_state_mae_and_dimension_checks():
    assert trajectory_fork.normalized_state_mae(
        [1.0, 2.0], [2.0, 0.0], [2.0, 4.0]
    ) == pytest.approx(0.5)
    with pytest.raises(ValueError, match="dimensions"):
        trajectory_fork.normalized_state_mae([], [], [])
    with pytest.raises(ValueError, match="positive"):
        trajectory_fork.normalized_state_mae([1.0], [2.0], [0.0])


def test_pearson_correlation_handles_positive_and_degenerate_inputs():
    assert trajectory_fork.pearson_correlation(
        [1.0, 2.0, 3.0], [2.0, 4.0, 6.0]
    ) == pytest.approx(1.0)
    assert trajectory_fork.pearson_correlation([1.0], [2.0]) is None
    assert trajectory_fork.pearson_correlation([1.0, 1.0], [2.0, 3.0]) is None


def _fork(region, action, state, confidence_delta, control=True, branch=False):
    return {
        "task_id": 0 if region == "broad" else 5,
        "phase": "early" if action < 0.3 else "middle",
        "region": region,
        "action_mae": action,
        "restore_state_mae": 0.0,
        "restore_pixel_mae": 0.0,
        "points": [
            {
                "state_mae": state,
                "pixel_mae": state / 10.0,
                "control_confidence": 0.8,
                "branch_confidence": 0.8 + confidence_delta,
                "control_eligible": control,
                "branch_eligible": branch,
            }
        ],
    }


def test_summarize_forks_and_assess_causal_effect():
    forks = [
        _fork("broad", 0.1, 0.02, -0.02),
        _fork("broad", 0.2, 0.04, -0.02),
        _fork("selected", 0.3, 0.06, -0.02),
        _fork("selected", 0.4, 0.08, -0.02),
        _fork("selected", 0.5, 0.10, -0.02),
        _fork("selected", 0.6, 0.12, -0.02),
    ]
    summary = trajectory_fork.summarize_forks(forks)
    assert summary["overall"]["eligibility_drop"] == 1.0
    assert summary["overall"]["action_state_correlation"] == pytest.approx(1.0)
    checks = trajectory_fork.assess_cause(
        summary,
        min_forks=6,
        min_state_divergence=0.01,
        min_eligibility_drop=0.1,
        min_confidence_drop=0.01,
        min_correlation=0.3,
    )
    assert all(checks.values())


def test_summarize_forks_rejects_empty_input():
    with pytest.raises(ValueError, match="must not be empty"):
        trajectory_fork.summarize_forks([])


def test_write_result_rejects_termination_message_overflow(tmp_path):
    with pytest.raises(RuntimeError, match="termination-message limit"):
        trajectory_fork._write_result(
            tmp_path / "result.json", {"payload": "x" * 3900}
        )
