from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
from types import ModuleType
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = (
    ROOT / "docs" / "research" / "tools" / "physical_ai_vla_risk_router.py"
)
SPEC = importlib.util.spec_from_file_location(
    "physical_ai_vla_risk_router", MODULE_PATH
)
assert SPEC and SPEC.loader
risk_router = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(risk_router)
DIAGNOSTIC_WRAPPER_PATH = (
    ROOT
    / "docs"
    / "research"
    / "tools"
    / "physical_ai_vla_calibration_failure_attribution.py"
)


def _state(width=0.08):
    return [0.0] * 6 + [width / 2.0, -width / 2.0]


def _action(command=-1.0, translation=0.1):
    return [translation, 0.0, 0.0, 0.0, 0.0, 0.0, command]


def test_classify_memory_admits_only_bounded_hold_actions():
    common = {
        "gripper_boundary": 0.05,
        "gripper_guard_band": 0.005,
        "translation_limit": 0.75,
        "rotation_limit": 0.15,
    }
    assert (
        risk_router.classify_memory(
            state=_state(0.08), action=_action(-1.0), **common
        )
        == "open_hold"
    )
    assert (
        risk_router.classify_memory(
            state=_state(0.02), action=_action(1.0), **common
        )
        == "closed_hold"
    )
    assert (
        risk_router.classify_memory(
            state=_state(0.08), action=_action(1.0), **common
        )
        == "suppression"
    )
    assert (
        risk_router.classify_memory(
            state=_state(0.08), action=_action(-1.0, 0.8), **common
        )
        == "suppression"
    )


def test_classify_memory_rejects_malformed_input():
    with pytest.raises(ValueError):
        risk_router.classify_memory(
            state=[],
            action=_action(),
            gripper_boundary=0.05,
            gripper_guard_band=0.005,
            translation_limit=0.75,
            rotation_limit=0.15,
        )


def test_contact_classification_separates_finger_and_arm_contact():
    assert risk_router.classify_contact_names(
        [
            ("robot0_right_finger", "target_object"),
            ("robot0_wrist", "table"),
            ("target_object", "table"),
        ]
    ) == {"total": 2, "finger": 1, "arm": 1}
    assert risk_router.classify_contact_names(
        [("robot0_left_finger", "robot0_right_finger")]
    ) == {"total": 1, "finger": 0, "arm": 1}


def _precheck(**overrides):
    values = {
        "state": _state(0.08),
        "previous_width": 0.08,
        "contacts": {"total": 0, "finger": 0, "arm": 0},
        "available": {"open_hold": 4, "closed_hold": 2},
        "gripper_boundary": 0.05,
        "gripper_guard_band": 0.005,
        "max_gripper_delta": 0.003,
        "state_z": 1.0,
        "state_z_limit": 3.0,
    }
    values.update(overrides)
    return risk_router.free_space_precheck(**values)


def test_free_space_precheck_allows_stable_contact_free_hold():
    assert _precheck() == ("open_hold", "eligible")


@pytest.mark.parametrize(
    ("overrides", "reason"),
    [
        ({"previous_width": None}, "first_observation"),
        ({"previous_width": 0.07}, "gripper_motion"),
        (
            {"contacts": {"total": 1, "finger": 1, "arm": 0}},
            "finger_contact",
        ),
        (
            {"contacts": {"total": 1, "finger": 0, "arm": 1}},
            "arm_contact",
        ),
        ({"state_z": 4.0}, "state_outlier"),
        ({"available": {"open_hold": 0}}, "no_executable_memory"),
    ],
)
def test_free_space_precheck_fails_closed(overrides, reason):
    assert _precheck(**overrides) == (None, reason)


def _candidate(**overrides):
    values = {
        "confidence": 0.8,
        "margin": 0.02,
        "safe": True,
        "safety_reason": "safe",
        "positive_similarity": 0.85,
        "negative_similarity": 0.70,
    }
    values.update(overrides)
    return values


def _route(candidate=None, **overrides):
    values = {
        "threshold": 0.76,
        "min_margin": 0.01,
        "veto_margin": 0.01,
        "previous_reuse": False,
    }
    values.update(overrides)
    return risk_router.route_candidate(
        _candidate() if candidate is None else candidate, **values
    )


def test_route_candidate_accepts_only_separate_confidence_and_safety_passes():
    assert _route() == (True, "accepted")
    assert _route(_candidate(confidence=0.7)) == (False, "confidence")
    assert _route(_candidate(safe=False, safety_reason="candidate_rotation")) == (
        False,
        "candidate_rotation",
    )


def test_route_candidate_applies_negative_veto_and_cooldown():
    assert _route(_candidate(negative_similarity=0.845)) == (
        False,
        "negative_veto",
    )
    assert _route(previous_reuse=True) == (False, "cooldown")


def test_simulate_reuse_mask_resets_cooldown_between_tasks():
    rows = [
        {"task_id": 0, "candidate": _candidate()},
        {"task_id": 0, "candidate": _candidate()},
        {"task_id": 1, "candidate": _candidate()},
    ]
    assert risk_router.simulate_reuse_mask(
        rows, threshold=0.76, min_margin=0.01, veto_margin=0.01
    ) == [True, False, True]


def test_simulate_route_outcomes_reports_one_reason_and_resets_cooldown():
    rows = [
        {"task_id": 0, "candidate": _candidate()},
        {"task_id": 0, "candidate": _candidate()},
        {"task_id": 0, "candidate": _candidate(confidence=0.5)},
        {"task_id": 0, "candidate": None},
        {"task_id": 1, "candidate": _candidate()},
    ]
    mask, reasons = risk_router.simulate_route_outcomes(
        rows, threshold=0.76, min_margin=0.01, veto_margin=0.01
    )
    assert mask == [True, False, False, False, True]
    assert reasons == [
        "accepted",
        "cooldown",
        "confidence",
        "no_candidate",
        "accepted",
    ]
    assert len(reasons) == len(rows)


def _shadow_row(task_id=0, candidate=None, mae=0.05, hold_class="open_hold"):
    return {
        "task_id": task_id,
        "precheck_reason": "eligible" if candidate is not None else "finger_contact",
        "hold_class": hold_class if candidate is not None else None,
        "policy_wall": 100.0,
        "memory_wall": 5.0 if candidate is not None else 0.0,
        "candidate": candidate,
        "candidate_mae": mae if candidate is not None else None,
    }


def test_evaluate_region_counts_end_to_end_compute():
    rows = [
        _shadow_row(candidate=_candidate()),
        _shadow_row(candidate=_candidate()),
        _shadow_row(candidate=None),
        _shadow_row(task_id=1, candidate=_candidate()),
    ]
    result = risk_router.evaluate_region(
        rows, threshold=0.76, min_margin=0.01, veto_margin=0.01
    )
    assert result["reuses"] == 2
    assert result["reuse_rate"] == pytest.approx(0.5)
    assert result["projected_latency_reduction"] == pytest.approx(0.4625)
    assert sum(result["route_reasons"].values()) == len(rows)
    assert [
        (row["task_id"], row["steps"], row["candidates"], row["reuses"])
        for row in result["tasks"]
    ] == [(0, 3, 2, 1), (1, 1, 1, 1)]
    assert result["tasks"][0]["mean_action_mae"] == pytest.approx(0.05)
    assert result["tasks"][1]["projected_latency_reduction"] == pytest.approx(
        0.95
    )


def _gate_row(**overrides):
    row = {
        "reuse_rate": 0.20,
        "mean_action_mae": 0.10,
        "p95_action_mae": 0.15,
        "projected_latency_reduction": 0.15,
    }
    row.update(overrides)
    return row


def _gate_mask(row):
    return risk_router._region_gate_mask(
        row,
        target_reuse_min=0.20,
        target_reuse_max=0.35,
        max_mean_action_mae=0.10,
        max_p95_action_mae=0.15,
        min_projected_latency_reduction=0.15,
    )


def test_region_gate_boundaries_are_inclusive():
    assert _gate_mask(_gate_row()) == 0
    assert _gate_mask(_gate_row(reuse_rate=0.35)) == 0


@pytest.mark.parametrize(
    ("overrides", "expected"),
    [
        ({"reuse_rate": 0.199}, 1),
        ({"reuse_rate": 0.351}, 2),
        ({"mean_action_mae": 0.101}, 4),
        ({"p95_action_mae": 0.151}, 8),
        ({"projected_latency_reduction": 0.149}, 16),
        (
            {
                "reuse_rate": 0.1,
                "mean_action_mae": 0.2,
                "p95_action_mae": 0.3,
                "projected_latency_reduction": 0.0,
            },
            29,
        ),
    ],
)
def test_region_gate_mask_attributes_independent_failures(overrides, expected):
    assert _gate_mask(_gate_row(**overrides)) == expected


def test_metric_scaling_preserves_just_over_boundary_failure():
    value = 0.100001
    encoded = risk_router._scaled_metric(value)
    assert encoded / risk_router.DIAGNOSTIC_SCALE == pytest.approx(value)
    assert _gate_mask(_gate_row(mean_action_mae=value)) == 4


def _select(rows, **overrides):
    values = {
        "threshold_min": 0.60,
        "threshold_max": 0.90,
        "threshold_step": 0.02,
        "margin_candidates": [0.0, 0.005, 0.01],
        "veto_margin": 0.01,
        "target_reuse_min": 0.20,
        "target_reuse_max": 0.60,
        "max_mean_action_mae": 0.10,
        "max_p95_action_mae": 0.15,
        "min_projected_latency_reduction": 0.15,
    }
    values.update(overrides)
    return risk_router.select_region(rows, **values)


def test_select_region_emits_consistent_canonical_48_point_grid():
    rows = [
        _shadow_row(task_id=index // 5, candidate=_candidate(confidence=0.82))
        for index in range(10)
    ]
    result = _select(rows, margin_candidates=[0.01, 0.0, 0.005, 0.005])
    diagnostics = result["diagnostics"]
    grid = diagnostics["grid"]
    assert result["candidates"] == len(grid) == 48
    assert grid[0][:2] == [600, 0]
    assert grid[1][:2] == [600, 5]
    assert grid[2][:2] == [600, 10]
    assert grid[3][:2] == [620, 0]
    assert grid[47][:2] == [900, 10]
    assert sum(diagnostics["signatures"].values()) == 48
    assert diagnostics["gate_counts"] == [
        sum(bool(row[-1] & (1 << bit)) for row in grid) for bit in range(5)
    ]
    assert result["viable"] == any(row[-1] == 0 for row in grid)
    assert (result["selected"] is not None) == result["viable"]


@pytest.mark.parametrize(
    ("overrides", "expected_bit"),
    [
        ({"reuse_rate": 0.19}, 1),
        ({"reuse_rate": 0.36}, 2),
        ({"mean_action_mae": 0.101}, 4),
        ({"p95_action_mae": 0.151}, 8),
        ({"projected_latency_reduction": 0.149}, 16),
    ],
)
def test_select_region_attributes_all_48_points_to_one_gate(
    monkeypatch, overrides, expected_bit
):
    metrics = {
        "reuse_rate": 0.20,
        "mean_action_mae": 0.10,
        "p95_action_mae": 0.15,
        "projected_latency_reduction": 0.15,
    }
    metrics.update(overrides)

    def fake_evaluate(_rows, *, threshold, min_margin, veto_margin):
        del veto_margin
        reuses = round(metrics["reuse_rate"] * 10)
        return {
            "threshold": threshold,
            "min_margin": min_margin,
            "reuses": reuses,
            **metrics,
            "route_reasons": {"accepted": 10},
            "tasks": [
                {
                    "task_id": 0,
                    "steps": 10,
                    "candidates": 10,
                    "reuses": reuses,
                    "mean_action_mae": metrics["mean_action_mae"],
                    "p95_action_mae": metrics["p95_action_mae"],
                    "projected_latency_reduction": metrics[
                        "projected_latency_reduction"
                    ],
                }
            ],
            "holds": [
                {
                    "task_id": 0,
                    "hold_class": "open_hold",
                    "candidates": 10,
                    "reuses": reuses,
                    "mean_action_mae": metrics["mean_action_mae"],
                    "p95_action_mae": metrics["p95_action_mae"],
                }
            ],
        }

    monkeypatch.setattr(risk_router, "evaluate_region", fake_evaluate)
    rows = [_shadow_row(candidate=_candidate()) for _ in range(10)]
    result = _select(rows, target_reuse_max=0.35)
    diagnostics = result["diagnostics"]
    assert diagnostics["gate_counts"] == [
        48 if bit == expected_bit.bit_length() - 1 else 0 for bit in range(5)
    ]
    assert diagnostics["signatures"] == {str(expected_bit): 48}
    assert not result["viable"]


def test_pareto_frontier_deduplicates_ties_deterministically():
    common = {
        "threshold": 0.6,
        "margin": 0.0,
        "reuse_rate": 0.2,
        "failure_mask": 0,
    }
    rows = [
        {
            **common,
            "index": 0,
            "reuses": 10,
            "mean_action_mae": 0.10,
            "p95_action_mae": 0.15,
            "latency_reduction": 0.20,
        },
        {
            **common,
            "index": 1,
            "reuses": 10,
            "mean_action_mae": 0.10,
            "p95_action_mae": 0.15,
            "latency_reduction": 0.20,
        },
        {
            **common,
            "index": 2,
            "reuses": 8,
            "mean_action_mae": 0.05,
            "p95_action_mae": 0.10,
            "latency_reduction": 0.15,
        },
        {
            **common,
            "index": 3,
            "reuses": 7,
            "mean_action_mae": 0.20,
            "p95_action_mae": 0.30,
            "latency_reduction": 0.10,
        },
    ]
    assert risk_router._pareto_indexes(rows) == [0, 2]


def test_select_region_preserves_zero_reuse_as_finite_json_nulls():
    result = _select([_shadow_row(candidate=None) for _ in range(10)])
    diagnostics = result["diagnostics"]
    assert diagnostics["no_reuse_points"] == 48
    assert all(row[2] == 0 and row[3] is None and row[4] is None for row in diagnostics["grid"])
    json.dumps(result, allow_nan=False)


def test_select_region_rejects_nonfinite_grid_parameters():
    rows = [_shadow_row(candidate=_candidate())]
    with pytest.raises(ValueError, match="finite"):
        _select(rows, threshold_min=float("nan"))
    with pytest.raises(ValueError, match="finite"):
        _select(rows, margin_candidates=[0.0, float("inf")])


def test_experiment_tenant_identity_is_parameterized():
    assert risk_router._tenant_id(31, "run") == "physical-ai-031-run"
    assert risk_router._tenant_id(32, "run") == "physical-ai-032-run"


def test_semantic_manifest_hash_ignores_refreshed_image_urls(tmp_path):
    row = {
        "episode_index": 1,
        "task_index": 0,
        "task": "task",
        "row_index": 10,
        "front_image_url": "https://example.test/old-front",
        "wrist_image_url": "https://example.test/old-wrist",
        "state": [0.0] * 8,
        "action": [0.0] * 7,
    }
    manifest = {
        "dataset": "dataset",
        "dataset_revision": "revision",
        "seed": 25,
        "memories": [row],
        "queries": [row],
    }
    first = tmp_path / "first.json"
    second = tmp_path / "second.json"
    first.write_text(json.dumps(manifest))
    refreshed = json.loads(json.dumps(manifest))
    refreshed["memories"][0]["front_image_url"] = "https://example.test/new-front"
    refreshed["queries"][0]["wrist_image_url"] = "https://example.test/new-wrist"
    second.write_text(json.dumps(refreshed))
    assert risk_router._semantic_manifest_sha256(
        first
    ) == risk_router._semantic_manifest_sha256(second)


def test_experiment_032_wrapper_forces_shadow_only(monkeypatch):
    wrapper_spec = importlib.util.spec_from_file_location(
        "physical_ai_vla_calibration_failure_attribution", DIAGNOSTIC_WRAPPER_PATH
    )
    assert wrapper_spec and wrapper_spec.loader
    wrapper = importlib.util.module_from_spec(wrapper_spec)
    wrapper_spec.loader.exec_module(wrapper)
    calls = []
    monkeypatch.setattr(
        wrapper.risk_router,
        "main",
        lambda **kwargs: calls.append(kwargs) or 0,
    )
    assert wrapper.main() == 0
    assert calls == [
        {
            "default_experiment_id": 32,
            "force_experiment_id": 32,
            "force_diagnostic_only": True,
        }
    ]


def test_diagnostics_report_closest_task_hold_and_structural_ceilings():
    rows = [
        _shadow_row(task_id=0, candidate=_candidate(), hold_class="open_hold"),
        _shadow_row(task_id=0, candidate=_candidate(), hold_class="open_hold"),
        _shadow_row(task_id=0, candidate=None),
        _shadow_row(task_id=1, candidate=_candidate(), hold_class="closed_hold"),
    ]
    diagnostics = _select(rows)["diagnostics"]
    assert diagnostics["ceilings"][:4] == [4, 3, 2, 2]
    assert sum(diagnostics["closest_rejections"]) == len(rows)
    assert sum(row[1] for row in diagnostics["closest_tasks"]) == len(rows)
    assert sum(row[2] for row in diagnostics["closest_holds"]) == 3


def test_diagnostic_only_run_never_starts_routed_execution(monkeypatch):
    fake_torch = ModuleType("torch")
    fake_huggingface = ModuleType("huggingface_hub")
    fake_huggingface.model_info = lambda _model, *, revision: SimpleNamespace(
        sha=revision
    )
    monkeypatch.setitem(sys.modules, "torch", fake_torch)
    monkeypatch.setitem(sys.modules, "huggingface_hub", fake_huggingface)
    monkeypatch.setattr(risk_router.exp28, "parse_task_ids", lambda _value: [0, 5])
    policy = SimpleNamespace(config=SimpleNamespace(n_action_steps=1))
    monkeypatch.setattr(
        risk_router.exp26,
        "_load_models",
        lambda _args: (object(), object(), policy, object(), object()),
    )
    monkeypatch.setattr(risk_router.exp27, "AgentMemoryClient", lambda *_a, **_k: object())
    memory = {
        "memories": [{}, {}],
        "availability": {
            task_id: {"open_hold": 1, "closed_hold": 0, "suppression": 0}
            for task_id in (0, 5)
        },
    }
    monkeypatch.setattr(
        risk_router,
        "_prepare_memory_bank",
        lambda *_args: (memory, {}),
    )
    variants = []

    def fake_episode(*, variant, task_id, **_kwargs):
        variants.append(variant)
        candidate = _candidate(
            confidence=0.8,
            margin=0.02,
            encoder_wall=7.0,
            positive_search_wall=4.0,
            suppression_search_wall=1.0,
            search_wall=5.0,
        )
        traces = [
            _shadow_row(
                task_id=task_id,
                candidate=candidate if index == 0 else None,
                mae=0.05,
            )
            for index in range(5)
        ]
        return {
            "task_id": task_id,
            "memory_task_index": task_id,
            "task_description": f"task-{task_id}",
            "steps": 5,
            "traces": traces,
            "search_wall": [5.0],
            "precheck_counts": {"eligible": 1, "finger_contact": 4},
        }

    monkeypatch.setattr(risk_router, "_run_episode", fake_episode)
    args = SimpleNamespace(
        task_ids="0,5",
        vla_model=risk_router.VLA_MODEL,
        agent_url="http://unused",
        http_timeout=1.0,
        base_policy_seed=1,
        calibration_seed=28,
        evaluation_seed=1028,
        threshold_min=0.60,
        threshold_max=0.90,
        threshold_step=0.02,
        margin_candidates=[0.0, 0.005, 0.01],
        veto_margin=0.01,
        min_reuse_rate=0.20,
        max_reuse_rate=0.35,
        max_mean_action_mae=0.10,
        max_p95_action_mae=0.15,
        min_latency_reduction=0.15,
        diagnostic_only=True,
        experiment_id=32,
        run_id="unit-test",
        sample_manifest=None,
        siglip_model=risk_router.SIGLIP_MODEL,
        episode_length=5,
        gripper_guard_band=0.005,
        max_gripper_delta=0.003,
        translation_limit=0.75,
        rotation_limit=0.15,
        disagreement_limit=0.30,
        state_z_limit=3.0,
        hazard_state_z=4.0,
    )
    result = risk_router.run_experiment(args)
    assert variants == ["shadow", "shadow"]
    assert result["status"] == "pass"
    assert result["experiment"] == 32
    assert result["finding"]["calibration_viable"] is True
    assert result["finding"]["replication_031"] == [False, False, False, False, True]
    assert result["execution"] == {
        "diagnostic_only": True,
        "routed_started": False,
    }
    assert all(result["acceptance"].values())
    assert len(
        json.dumps(
            result, allow_nan=False, separators=(",", ":"), sort_keys=True
        ).encode("utf-8")
    ) <= 3900
    assert "Attribution 032 Results" in risk_router.render_report(result)


def test_select_region_handles_empty_and_invalid_ranges():
    with pytest.raises(ValueError, match="must not be empty"):
        risk_router.evaluate_region(
            [], threshold=0.7, min_margin=0.0, veto_margin=0.0
        )
    with pytest.raises(ValueError, match="target reuse"):
        risk_router.select_region(
            [_shadow_row(candidate=_candidate())],
            threshold_min=0.6,
            threshold_max=0.9,
            threshold_step=0.1,
            margin_candidates=[0.0],
            veto_margin=0.0,
            target_reuse_min=0.8,
            target_reuse_max=0.2,
            max_mean_action_mae=0.1,
            max_p95_action_mae=0.15,
            min_projected_latency_reduction=0.1,
        )


def _realistic_compact_failure():
    traces = []
    for index in range(595):
        task_id = 0 if index < 300 else 5
        candidate = None
        if index % 4 == 0:
            candidate = _candidate(
                confidence=0.60 + (index % 31) / 100.0,
                margin=(index % 5) / 1000.0,
                encoder_wall=7.0 + (index % 3) / 10.0,
            )
        traces.append(
            _shadow_row(
                task_id=task_id,
                candidate=candidate,
                mae=0.04 + (index % 20) / 1000.0,
                hold_class="open_hold" if index % 8 else "closed_hold",
            )
        )
    calibration = _select(
        traces,
        target_reuse_min=0.20,
        target_reuse_max=0.35,
    )
    episode_rows = []
    for task_id in (0, 5):
        task_traces = [row for row in traces if row["task_id"] == task_id]
        candidate_count = sum(row["candidate"] is not None for row in task_traces)
        episode_rows.append(
            {
                "task_id": task_id,
                "steps": len(task_traces),
                "traces": task_traces,
                "search_wall": [5.0] * candidate_count,
                "precheck_counts": {
                    "eligible": candidate_count,
                    "finger_contact": len(task_traces) - candidate_count,
                },
            }
        )
    memory = {
        "memories": [{}] * 190,
        "availability": {
            task_id: {"open_hold": 9, "closed_hold": 1, "suppression": 9}
            for task_id in range(10)
        },
    }
    result = risk_router._compact_failure(
        SimpleNamespace(diagnostic_only=True),
        resolved_sha="a" * 40,
        reason="no_viable_free_space_region",
        memory=memory,
        calibration=calibration,
        rows=episode_rows,
    )
    return result


def test_compact_failure_with_48_points_fits_termination_message_limit():
    result = _realistic_compact_failure()
    encoded = json.dumps(
        result, allow_nan=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    assert len(result["calibration"]["diagnostics"]["grid"]) == 48
    assert set(result["memory"]["availability"]) == {0, 5}
    assert len(encoded) <= 3900


def test_compact_failure_fits_with_all_bounded_reason_names_and_signatures():
    result = _realistic_compact_failure()
    result["manifest_semantic_sha256"] = "b" * 64
    result["run_id"] = "20260718123456-123456"
    result["completion_reason"] = result.pop("stop_reason")
    result["execution"] = {"diagnostic_only": True, "routed_started": False}
    result["finding"] = {"calibration_viable": False}
    result["acceptance"] = {
        "grid_48_complete": True,
        "counts_consistent": True,
        "route_attribution_complete": True,
        "routed_actions_zero": True,
    }
    diagnostics = result["calibration"]["diagnostics"]
    diagnostics["closest_rejections"] = [54] * len(
        risk_router.ROUTE_REASON_NAMES
    )
    diagnostics["closest_rejections"][0] += 1
    diagnostics["signatures"] = {str(index): 1 for index in range(32)}
    result["shadow"]["task_precheck"] = [
        [0, 300, [37] * len(risk_router.PRECHECK_REASON_NAMES)],
        [5, 295, [36] * len(risk_router.PRECHECK_REASON_NAMES)],
    ]
    encoded = json.dumps(
        result, allow_nan=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    assert len(encoded) <= 3900


def test_render_report_attributes_every_calibration_point():
    result = _realistic_compact_failure()
    result["acceptance"]["aws_resources_deleted"] = True
    report = risk_router.render_report(result)
    assert "## All 48 Calibration Points" in report
    assert "## Structural Funnel" in report
    assert "## Calibration Gate Attribution" in report
    assert "| 0 | 0.600 |" in report
    assert "| 47 | 0.900 |" in report
    assert "No memory action was executed" in report
    assert "AWS temporary-resource cleanup: pass" in report
    assert "## Decision And Next Experiment" in report
    assert "veto separability as the next uncertainty" in report
    assert "abort before a grid unless per-task" in report


def test_render_report_lists_all_tied_most_common_gate_flags():
    result = _realistic_compact_failure()
    result["calibration"]["diagnostics"]["gate_counts"] = [48] * 5
    report = risk_router.render_report(result)
    for name in risk_router.DIAGNOSTIC_GATE_NAMES:
        assert f"`{name}`" in report
    assert "not independent failures or causal dominance" in report


def test_write_result_rejects_termination_message_overflow(tmp_path):
    empty_size = len(
        json.dumps(
            {"payload": ""},
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    )
    exact = {"payload": "x" * (3900 - empty_size)}
    exact_path = tmp_path / "exact.json"
    risk_router._write_result(exact_path, exact)
    assert len(exact_path.read_bytes()) == 3900
    with pytest.raises(RuntimeError, match="termination-message limit"):
        risk_router._write_result(
            tmp_path / "result.json", {"payload": exact["payload"] + "x"}
        )
