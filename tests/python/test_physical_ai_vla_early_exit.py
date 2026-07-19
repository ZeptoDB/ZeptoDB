from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "docs" / "research" / "tools" / "physical_ai_vla_early_exit.py"
SPEC = importlib.util.spec_from_file_location("physical_ai_vla_early_exit", MODULE_PATH)
assert SPEC and SPEC.loader
vla = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(vla)


def test_split_calibration_evaluation_is_balanced_and_disjoint():
    queries = [
        {"task_index": task, "episode_index": task * 10 + index}
        for task in range(2)
        for index in range(4)
    ]

    calibration, evaluation = vla.split_calibration_evaluation(
        queries, calibration_per_task=2
    )

    assert len(calibration) == len(evaluation) == 4
    assert {row["episode_index"] for row in calibration}.isdisjoint(
        row["episode_index"] for row in evaluation
    )
    assert [row["task_index"] for row in calibration].count(0) == 2
    assert [row["task_index"] for row in evaluation].count(1) == 2


def test_split_calibration_evaluation_rejects_empty_evaluation_partition():
    with pytest.raises(ValueError, match="needs more"):
        vla.split_calibration_evaluation(
            [{"task_index": 0, "episode_index": 1}], calibration_per_task=1
        )


def test_normalized_action_mae_and_dimension_validation():
    assert vla.normalized_action_mae([1.0, 2.0], [0.0, 0.0], [2.0, 4.0]) == 0.5
    with pytest.raises(ValueError, match="dimensions"):
        vla.normalized_action_mae([], [], [])
    with pytest.raises(ValueError, match="positive"):
        vla.normalized_action_mae([1.0], [0.0], [0.0])


def test_weighted_action_prior_prefers_closer_match_and_penalizes_disagreement():
    scale = [2.0, 2.0]
    close, close_confidence = vla.weighted_action_prior(
        [
            {"similarity": 0.90, "action": [1.0, 1.0]},
            {"similarity": 0.80, "action": [0.8, 0.8]},
        ],
        scale,
    )
    _, disagreeing_confidence = vla.weighted_action_prior(
        [
            {"similarity": 0.90, "action": [1.0, 1.0]},
            {"similarity": 0.80, "action": [-1.0, -1.0]},
        ],
        scale,
    )

    assert close[0] > 0.9
    assert close_confidence > disagreeing_confidence


def test_weighted_action_prior_rejects_empty_matches():
    with pytest.raises(ValueError, match="at least one"):
        vla.weighted_action_prior([], [1.0])


def test_calibrate_threshold_maximizes_skips_with_quality_bound():
    result = vla.calibrate_threshold(
        [
            {"confidence": 0.9, "baseline_mae": 0.10, "prior_mae": 0.09},
            {"confidence": 0.8, "baseline_mae": 0.10, "prior_mae": 0.10},
            {"confidence": 0.2, "baseline_mae": 0.10, "prior_mae": 0.50},
        ]
    )

    assert result["skipped"] == 2
    assert result["threshold"] == pytest.approx(0.8)
    assert result["routed_mae"] <= result["quality_limit"]


def test_calibrate_threshold_allows_zero_skip_boundary():
    result = vla.calibrate_threshold(
        [{"confidence": 0.5, "baseline_mae": 0.01, "prior_mae": 1.0}]
    )

    assert result["skipped"] == 0
    assert result["skip_rate"] == 0.0


def test_load_manifest_rejects_missing_vla_input(tmp_path):
    rows = [
        {
            "task_index": task,
            "front_image_url": "https://example.test/front",
            "wrist_image_url": "https://example.test/wrist",
            "state": [0.0] * 8,
            "action": [0.0] * 7,
        }
        for task in range(10)
    ]
    rows[3].pop("action")
    path = tmp_path / "manifest.json"
    path.write_text(vla.json.dumps({"memories": rows, "queries": rows}))

    with pytest.raises(ValueError, match="missing VLA"):
        vla.load_sample_manifest(path, memory_per_task=1, query_per_task=1)


def test_resolve_samples_resumes_fresh_checkpoint(tmp_path, monkeypatch):
    checkpoint = tmp_path / "checkpoint.json"
    checkpoint.write_text(
        vla.json.dumps(
            {
                "created_at": vla.time.time(),
                "samples": [{"row_index": 1, "value": "cached"}],
            }
        )
    )
    calls = []

    def resolve(sample):
        calls.append(sample["row_index"])
        return {**sample, "value": "fetched"}

    monkeypatch.setattr(vla, "_resolve_sample", resolve)
    result = vla.resolve_samples_with_checkpoint(
        [{"row_index": 1}, {"row_index": 2}],
        checkpoint=checkpoint,
        workers=1,
    )

    assert calls == [2]
    assert [row["value"] for row in result] == ["cached", "fetched"]
    assert len(vla.json.loads(checkpoint.read_text())["samples"]) == 2


def test_resolve_samples_rejects_zero_workers(tmp_path):
    with pytest.raises(ValueError, match="workers must be positive"):
        vla.resolve_samples_with_checkpoint(
            [{"row_index": 1}],
            checkpoint=tmp_path / "checkpoint.json",
            workers=0,
        )


def test_write_result_rejects_termination_message_overflow(tmp_path):
    with pytest.raises(RuntimeError, match="termination-message limit"):
        vla._write_result(tmp_path / "result.json", {"payload": "x" * 3900})
