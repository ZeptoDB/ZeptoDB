from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = (
    ROOT / "docs" / "research" / "tools" / "physical_ai_vla_skip_region.py"
)
SPEC = importlib.util.spec_from_file_location("physical_ai_vla_skip_region", MODULE_PATH)
assert SPEC and SPEC.loader
skip_region = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(skip_region)


def _row(
    *,
    task_id=0,
    confidence=0.75,
    margin=0.02,
    error=0.1,
    policy=100.0,
    encoder=10.0,
    search=1.0,
    phase="early",
):
    return {
        "task_id": task_id,
        "confidence": confidence,
        "margin": margin,
        "candidate_mae": error,
        "policy_wall": policy,
        "encoder_wall": encoder,
        "search_wall": search,
        "phase": phase,
    }


def test_trajectory_phase_covers_boundaries():
    assert skip_region.trajectory_phase(0, 6) == "early"
    assert skip_region.trajectory_phase(2, 6) == "middle"
    assert skip_region.trajectory_phase(4, 6) == "late"
    with pytest.raises(ValueError, match="within"):
        skip_region.trajectory_phase(6, 6)


def test_simulate_skip_mask_resets_by_task_and_bounds_bursts():
    rows = [_row() for _ in range(5)] + [_row(task_id=1) for _ in range(2)]
    assert skip_region.simulate_skip_mask(
        rows,
        threshold=0.7,
        min_margin=0.01,
        required_streak=2,
        max_consecutive_skips=2,
    ) == [False, True, True, False, True, False, True]


def test_simulate_skip_mask_rejects_invalid_limits():
    with pytest.raises(ValueError, match="positive"):
        skip_region.simulate_skip_mask(
            [_row()],
            threshold=0.7,
            min_margin=0.0,
            required_streak=0,
            max_consecutive_skips=2,
        )


def test_evaluate_skip_region_projects_real_router_cost():
    result = skip_region.evaluate_skip_region(
        [_row() for _ in range(4)],
        threshold=0.7,
        min_margin=0.01,
        required_streak=2,
        max_consecutive_skips=2,
    )
    assert result["skips"] == 2
    assert result["skip_rate"] == 0.5
    assert result["mean_action_mae"] == pytest.approx(0.1)
    assert result["projected_latency_reduction"] == pytest.approx(0.39)


def test_evaluate_skip_region_rejects_empty_input():
    with pytest.raises(ValueError, match="must not be empty"):
        skip_region.evaluate_skip_region(
            [],
            threshold=0.7,
            min_margin=0.0,
            required_streak=2,
            max_consecutive_skips=2,
        )


def test_select_skip_region_finds_target_rate_with_bounded_error():
    rows = [
        _row(confidence=0.75 if index < 4 else 0.65)
        for index in range(10)
    ]
    result = skip_region.select_skip_region(
        rows,
        threshold_min=0.7,
        threshold_max=0.8,
        threshold_step=0.05,
        margin_candidates=[0.0],
        target_skip_min=0.2,
        target_skip_max=0.3,
        max_mean_action_mae=0.2,
        max_p95_action_mae=0.2,
        min_projected_latency_reduction=0.05,
        required_streak=2,
        max_consecutive_skips=2,
    )
    assert result["viable"]
    assert result["selected"]["skip_rate"] == pytest.approx(0.2)


def test_select_skip_region_reports_no_viable_candidate():
    rows = [_row(error=1.0) for _ in range(10)]
    result = skip_region.select_skip_region(
        rows,
        threshold_min=0.7,
        threshold_max=0.7,
        threshold_step=0.01,
        margin_candidates=[0.0],
        target_skip_min=0.2,
        target_skip_max=0.3,
        max_mean_action_mae=0.2,
        max_p95_action_mae=0.2,
        min_projected_latency_reduction=0.1,
        required_streak=2,
        max_consecutive_skips=2,
    )
    assert not result["viable"]
    assert result["best_safe"] is None


def test_summarize_phases_handles_empty_phase():
    result = skip_region.summarize_phases(
        [_row(phase="early"), _row(phase="middle")],
        [True, False],
    )
    assert result[0]["sk"] == 1
    assert result[2]["n"] == 0
    assert result[2]["e"] is None


def test_parse_task_ids_validates_empty_duplicate_and_range():
    assert skip_region.parse_task_ids("0, 5") == [0, 5]
    for value in ("", "0,0", "10", "x"):
        with pytest.raises(ValueError):
            skip_region.parse_task_ids(value)


def test_write_result_rejects_termination_message_overflow(tmp_path):
    with pytest.raises(RuntimeError, match="termination-message limit"):
        skip_region._write_result(tmp_path / "result.json", {"payload": "x" * 3900})


def test_render_legacy_pilot_marks_mismatched_timer_unusable():
    report = skip_region.render_report(
        {
            "status": "fail",
            "suite": "libero_10",
            "vla_model": "vla",
            "vla_sha": "sha",
            "siglip_model": "siglip",
            "stop_reason": "closed_loop_pilot_gate_failed",
            "scope": {"tasks": 2},
            "shadow": {
                "steps": 10,
                "successes": 1,
                "search": {"p95_ms": 2.0},
            },
            "calibration": {"selected": None},
            "pilot": {
                "regressions": 0,
                "skip_rate": 0.1,
                "latency_reduction": 0.2,
            },
        }
    )
    assert "not comparable" in report
    assert "simulator step" in report
