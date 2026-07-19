from __future__ import annotations

import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys
from types import ModuleType
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "docs" / "research" / "tools"
MODULE_PATH = TOOLS / "physical_ai_vla_risk_router.py"
WRAPPER_PATH = TOOLS / "physical_ai_vla_veto_separability.py"
CORRECTION_WRAPPER_PATH = TOOLS / "physical_ai_vla_task_mapping_correction.py"
LEGACY_COMPACT_PATH = (
    ROOT
    / "docs"
    / "research"
    / "results"
    / "physical_ai_vla_veto_separability_compact_033_attempt1_invalidated.json"
)
LEGACY_DETAIL_PATH = (
    ROOT
    / "docs"
    / "research"
    / "results"
    / "physical_ai_vla_veto_separability_candidates_033.json"
)
SPEC = importlib.util.spec_from_file_location(
    "physical_ai_vla_risk_router_exp033_test", MODULE_PATH
)
assert SPEC and SPEC.loader
risk_router = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(risk_router)


def _state(width=0.08):
    return [0.0] * 6 + [width / 2.0, -width / 2.0]


def _action(command=-1.0, translation=0.1, rotation=0.0):
    return [translation, 0.0, 0.0, rotation, 0.0, 0.0, command]


def _candidate(*, group="separated", episode=10, mae=0.05, **overrides):
    positive = 0.85
    negative = {
        "no_negative_support": None,
        "separated": 0.82,
        "vetoed": 0.845,
    }[group]
    values = {
        "prior": [0.0] * 7,
        "confidence": 0.80,
        "margin": 0.02,
        "safe": True,
        "safety_reason": "safe",
        "positive_similarity": positive,
        "positive_second_similarity": 0.83,
        "negative_similarity": negative,
        "positive_neighbors": 5,
        "negative_neighbors": 0 if negative is None else 5,
        "positive_episode_index": episode,
        "negative_episode_index": None if negative is None else episode + 100,
        "negative_admission_mask": None if negative is None else 2,
        "disagreement": 0.05,
        "encoder_wall": 10.0,
        "encoder_gpu": 5.0,
        "positive_search_wall": 4.0,
        "suppression_search_wall": 3.0 if negative is not None else 0.0,
        "search_wall": 7.0 if negative is not None else 4.0,
    }
    values.update(overrides)
    return values


def _trace(
    *,
    task=0,
    step=0,
    phase="early",
    hold="open_hold",
    group="separated",
    candidate=True,
    mae=0.05,
    precheck_reason=None,
    memory_task=None,
):
    candidate_value = (
        _candidate(group=group, episode=10 + step, mae=mae) if candidate else None
    )
    return {
        "task_id": task,
        "memory_task_index": task if memory_task is None else memory_task,
        "episode_id": 28 + task,
        "step": step,
        "query_phase": phase,
        "precheck_reason": precheck_reason
        or ("eligible" if candidate else "finger_contact"),
        "hold_class": hold if candidate else None,
        "observed_hold_class": hold,
        "policy_wall": 440.0,
        "memory_wall": 17.0 if candidate else 0.0,
        "candidate": candidate_value,
        "candidate_mae": mae if candidate else None,
        "candidate_dimension_errors": [mae] * 7 if candidate else None,
    }


def _manifest(path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    row = {
        "episode_index": 1,
        "task_index": 0,
        "task": "task",
        "row_index": 10,
        "state": [0.0] * 8,
        "action": [0.0] * 7,
    }
    path.write_text(
        json.dumps(
            {
                "dataset": "dataset",
                "dataset_revision": "revision",
                "seed": 25,
                "memories": [row],
                "queries": [row],
            }
        )
    )
    return path


def _args(tmp_path: Path, **overrides):
    values = {
        "experiment_id": 33,
        "run_id": "unit-033",
        "sample_manifest": _manifest(tmp_path / "manifest.json"),
        "siglip_model": risk_router.SIGLIP_MODEL,
        "resolved_siglip_sha": "s" * 40,
        "entrypoint_path": str(WRAPPER_PATH),
        "calibration_seed": 28,
        "veto_margin": 0.01,
        "max_p95_action_mae": 0.15,
        "veto_min_group_samples": 20,
        "min_reuse_rate": 0.20,
        "min_latency_reduction": 0.15,
    }
    values.update(overrides)
    return SimpleNamespace(**values)


def _memory():
    availability = {
        0: {"open_hold": 13, "closed_hold": 1, "suppression": 5},
        5: {"open_hold": 9, "closed_hold": 0, "suppression": 10},
    }
    return {
        "memories": [{}] * 190,
        "availability": availability,
        "admission_reasons": {
            0: {"open_hold": 13, "closed_hold": 1, "gripper_command_mismatch": 5},
            5: {"open_hold": 9, "gripper_command_mismatch": 10},
        },
        "admission_mask_counts": {0: {0: 14, 2: 5}, 5: {0: 9, 2: 10}},
        "task_texts": {0: "task-0", 5: "task-5"},
        "text_to_task": {"task-0": 0, "task-5": 5},
    }


def _episode_rows(traces):
    rows = []
    for task in sorted({trace["task_id"] for trace in traces}):
        task_traces = [trace for trace in traces if trace["task_id"] == task]
        memory_tasks = {trace["memory_task_index"] for trace in task_traces}
        assert len(memory_tasks) == 1
        memory_task = memory_tasks.pop()
        counts = {}
        searches = []
        for trace in task_traces:
            reason = trace["precheck_reason"]
            counts[reason] = counts.get(reason, 0) + 1
            if trace["candidate"] is not None:
                searches.append(trace["candidate"]["search_wall"])
        rows.append(
            {
                "task_id": task,
                "memory_task_index": memory_task,
                "task_description": f"task-{memory_task}",
                "steps": len(task_traces),
                "vla_calls": len(task_traces),
                "reuses": 0,
                "traces": task_traces,
                "search_wall": searches,
                "precheck_counts": counts,
            }
        )
    return rows


def test_memory_admission_reason_and_overlap_mask_preserve_boundaries():
    common = {
        "gripper_boundary": 0.05,
        "gripper_guard_band": 0.005,
        "translation_limit": 0.75,
        "rotation_limit": 0.15,
    }
    assert risk_router.classify_memory_with_reason(
        state=_state(0.08), action=_action(), **common
    ) == ("open_hold", "open_hold")
    assert risk_router.classify_memory_with_reason(
        state=_state(0.055), action=_action(), **common
    ) == ("suppression", "gripper_ambiguous")
    assert risk_router.classify_memory_with_reason(
        state=_state(0.08), action=_action(1.0), **common
    ) == ("suppression", "gripper_command_mismatch")
    assert risk_router.classify_memory_with_reason(
        state=_state(0.08), action=_action(translation=0.75), **common
    )[0] == "open_hold"
    assert risk_router.classify_memory_with_reason(
        state=_state(0.08),
        action=_action(translation=math.nextafter(0.75, math.inf)),
        **common,
    ) == ("suppression", "translation_limit")
    mask = risk_router.memory_admission_mask(
        state=_state(0.08),
        action=_action(1.0, translation=0.8, rotation=0.2),
        **common,
    )
    assert mask == 0b1110


def test_veto_group_has_three_states_and_inclusive_boundary():
    candidate = _candidate(group="separated")
    candidate["negative_similarity"] = None
    assert risk_router.veto_group(candidate, 0.01) == "no_negative_support"
    candidate["negative_similarity"] = 0.84
    assert risk_router.veto_group(candidate, 0.01) == "vetoed"
    candidate["negative_similarity"] = math.nextafter(0.84, -math.inf)
    assert risk_router.veto_group(candidate, 0.01) == "separated"
    for invalid in (-0.01, float("nan"), float("inf")):
        with pytest.raises(ValueError):
            risk_router.veto_group(candidate, invalid)
    candidate["negative_similarity"] = float("nan")
    with pytest.raises(ValueError):
        risk_router.veto_group(candidate, 0.01)


def test_candidate_attribution_conserves_three_groups_and_dimension_error():
    traces = [
        _trace(step=0, group="no_negative_support"),
        _trace(step=1, group="separated"),
        _trace(step=2, group="vetoed", mae=0.20),
        _trace(step=3, candidate=False),
    ]
    rows = risk_router._veto_candidate_rows(
        traces, veto_margin=0.01, high_error_threshold=0.15
    )
    assert [row["group"] for row in rows] == list(risk_router.VETO_GROUP_NAMES)
    assert sum(row["high_error"] for row in rows) == 1
    summary = risk_router._veto_group_summary(rows)
    assert sum(row[1] for row in summary) == len(rows)
    assert all(row[3] is not None for row in summary)

    broken = dict(traces[0])
    broken["candidate_dimension_errors"] = [0.05] * 6 + [0.06]
    with pytest.raises(ValueError, match="action-error"):
        risk_router._veto_candidate_rows(
            [broken], veto_margin=0.01, high_error_threshold=0.15
        )


def test_empty_veto_groups_use_standard_json_null_metrics():
    rows = risk_router._veto_candidate_rows(
        [_trace(group="vetoed")], veto_margin=0.01, high_error_threshold=0.15
    )
    summary = risk_router._veto_group_summary(rows)
    assert summary[0][1] == 0
    assert summary[0][3] is None
    assert summary[1][1] == 0
    assert summary[2][1] == 1
    json.dumps(summary, allow_nan=False)


def test_veto_counterfactuals_are_monotonic_and_keep_missing_negative_separate():
    rows = risk_router._veto_candidate_rows(
        [
            _trace(step=0, group="no_negative_support"),
            _trace(step=1, group="separated"),
            _trace(step=2, group="vetoed"),
        ],
        veto_margin=0.01,
        high_error_threshold=0.15,
    )
    result = risk_router._veto_counterfactuals(
        rows, risk_router.VETO_COUNTERFACTUAL_MARGINS
    )
    assert [row[1] for row in result] == [1, 1, 1, 1]
    assert [row[3] for row in result] == sorted(row[3] for row in result)
    assert all(sum(row[1:4]) == len(rows) for row in result)


def test_query_phase_and_hold_support_account_for_every_trace():
    traces = [
        _trace(step=0, phase="early", hold="open_hold"),
        _trace(step=1, phase="middle", hold="closed_hold", candidate=False),
        _trace(
            step=2,
            phase="late",
            hold="closed_hold",
            candidate=False,
            precheck_reason="no_executable_memory",
        ),
    ]
    support = risk_router._query_support_summary(traces)
    assert sum(row[3] for row in support) == len(traces)
    assert sum(row[4] for row in support) == 1
    assert sum(row[5] for row in support) == 1
    with pytest.raises(ValueError):
        risk_router.exp28.trajectory_phase(3, 3)
    assert risk_router.exp28.trajectory_phase(0, 3) == "early"
    assert risk_router.exp28.trajectory_phase(1, 3) == "middle"
    assert risk_router.exp28.trajectory_phase(2, 3) == "late"


def test_compact_uses_text_resolved_memory_task_not_suite_order(tmp_path):
    memory = _memory()
    memory["availability"][9] = {
        "open_hold": 0,
        "closed_hold": 8,
        "suppression": 11,
    }
    memory["admission_reasons"][9] = {
        "closed_hold": 8,
        "gripper_ambiguous": 1,
        "gripper_command_mismatch": 8,
        "translation_limit": 2,
    }
    memory["admission_mask_counts"][9] = {0: 8, 1: 1, 2: 8, 4: 2}
    memory["task_texts"] = {
        5: "put both items in the basket",
        9: "put the book in the caddy",
    }
    memory["text_to_task"] = {
        text: task for task, text in memory["task_texts"].items()
    }
    rows = _episode_rows(
        [
            _trace(task=0, memory_task=5, group="vetoed"),
            _trace(task=5, memory_task=9, candidate=False),
        ]
    )
    rows[0]["task_description"] = memory["task_texts"][5]
    rows[1]["task_description"] = memory["task_texts"][9]
    result = risk_router._compact_veto_separability(
        _args(tmp_path),
        resolved_sha=risk_router.VLA_MODEL_SHA,
        memory=memory,
        rows=rows,
    )
    assert result["scope"]["suite_tasks"] == [0, 5]
    assert result["scope"]["task_map"] == [
        [0, 5, memory["task_texts"][5]],
        [5, 9, memory["task_texts"][9]],
    ]
    assert set(result["memory"]["availability"]) == {5, 9}
    assert [row[0] for row in result["memory"]["admission"]] == [5, 9]
    assert result["acceptance"]["task_mapping_consistent"] is True
    assert result["diagnostics"]["schema"] == 2
    assert "replication_032_legacy_reported" in result["finding"]
    report = risk_router.render_report(result)
    assert "LIBERO suite task IDs and manifest task indexes" in report
    assert "| 0 | 5 | put both items in the basket |" in report
    assert "Manifest task index" in report
    assert "suite task 5 / manifest task 9 open-hold coverage" in report
    duplicate_rows = _episode_rows(
        [
            _trace(task=0, memory_task=5, group="vetoed"),
            _trace(task=5, memory_task=5, candidate=False),
        ]
    )
    duplicate_rows[0]["task_description"] = memory["task_texts"][5]
    duplicate_rows[1]["task_description"] = memory["task_texts"][5]
    with pytest.raises(ValueError, match="memory task appears more than once"):
        risk_router._resolved_task_map(duplicate_rows, memory)


def test_experiment_034_acceptance_requires_exact_frozen_mapping(
    monkeypatch, tmp_path
):
    memory = _memory()
    memory["availability"][9] = {
        "open_hold": 0,
        "closed_hold": 8,
        "suppression": 11,
    }
    memory["admission_reasons"][9] = {
        "closed_hold": 8,
        "gripper_ambiguous": 1,
        "gripper_command_mismatch": 8,
        "translation_limit": 2,
    }
    memory["admission_mask_counts"][9] = {0: 8, 1: 1, 2: 8, 4: 2}
    memory["task_texts"] = {
        task_index: task_description
        for _, task_index, task_description in (
            risk_router.TASK_MAPPING_CORRECTION_EXPECTED_MAP
        )
    }
    memory["text_to_task"] = {
        text: task for task, text in memory["task_texts"].items()
    }
    rows = _episode_rows(
        [
            _trace(task=0, memory_task=5, group="vetoed"),
            _trace(task=5, memory_task=9, candidate=False),
        ]
    )
    rows[0]["task_description"] = memory["task_texts"][5]
    rows[1]["task_description"] = memory["task_texts"][9]
    monkeypatch.setattr(
        risk_router,
        "_semantic_manifest_sha256",
        lambda _path: risk_router.EXPERIMENT_031_SEMANTIC_MANIFEST_SHA256,
    )
    args = _args(
        tmp_path,
        experiment_id=34,
        run_id="unit-034",
        entrypoint_path=str(CORRECTION_WRAPPER_PATH),
    )
    result = risk_router._compact_veto_separability(
        args,
        resolved_sha=risk_router.VLA_MODEL_SHA,
        memory=memory,
        rows=rows,
    )
    assert result["acceptance"]["task_mapping_consistent"] is True
    memory["availability"][9]["open_hold"] = 1
    result = risk_router._compact_veto_separability(
        args,
        resolved_sha=risk_router.VLA_MODEL_SHA,
        memory=memory,
        rows=rows,
    )
    assert result["acceptance"]["task_mapping_consistent"] is False


def test_compact_veto_diagnostic_fits_and_preserves_detail_sha(tmp_path):
    traces = []
    for index in range(595):
        task = 0 if index < 377 else 5
        local_step = index if task == 0 else index - 377
        phase = ("early", "middle", "late")[min(2, local_step * 3 // 520)]
        if index < 127:
            group = (
                "no_negative_support"
                if index == 0
                else "separated"
                if index < 4
                else "vetoed"
            )
            traces.append(
                _trace(
                    task=task,
                    step=local_step,
                    phase=phase,
                    group=group,
                    mae=0.05 + (index % 20) / 100.0,
                )
            )
        else:
            reason = "no_executable_memory" if 500 <= index < 558 else "finger_contact"
            traces.append(
                _trace(
                    task=task,
                    step=local_step,
                    phase=phase,
                    hold="closed_hold" if task == 5 else "open_hold",
                    candidate=False,
                    precheck_reason=reason,
                )
            )
    args = _args(tmp_path)
    result = risk_router._compact_veto_separability(
        args,
        resolved_sha=risk_router.VLA_MODEL_SHA,
        memory=_memory(),
        rows=_episode_rows(traces),
    )
    encoded = json.dumps(
        result, allow_nan=False, separators=(",", ":"), sort_keys=True
    ).encode()
    assert result["status"] == "pass"
    assert result["detail"]["rows"] == 127
    assert result["detail"]["sha256"] == hashlib.sha256(
        json.dumps(
            args.veto_detail_payload,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode()
    ).hexdigest()
    assert len(encoded) <= 3700, len(encoded)
    assert len(args.veto_detail_payload["rows"]) == 127


def test_worst_bounded_exp034_payload_fits_termination_limit(monkeypatch, tmp_path):
    traces = []
    step_by_task = {0: 0, 5: 0}
    for task in (0, 5):
        for phase in risk_router.QUERY_PHASE_NAMES:
            for hold in risk_router.HOLD_CLASS_NAMES[:2]:
                for group in risk_router.VETO_GROUP_NAMES:
                    step = step_by_task[task]
                    traces.append(
                        _trace(
                            task=task,
                            step=step,
                            phase=phase,
                            hold=hold,
                            group=group,
                            mae=0.05 + (step % 20) / 100.0,
                        )
                    )
                    step_by_task[task] += 1
            step = step_by_task[task]
            traces.append(
                _trace(
                    task=task,
                    step=step,
                    phase=phase,
                    hold="ambiguous",
                    candidate=False,
                    precheck_reason="gripper_ambiguous",
                )
            )
            step_by_task[task] += 1
    while len(traces) < 595:
        task = 0 if len(traces) % 2 == 0 else 5
        step = step_by_task[task]
        phase = risk_router.QUERY_PHASE_NAMES[step % 3]
        hold = risk_router.HOLD_CLASS_NAMES[step % 3]
        traces.append(
            _trace(
                task=task,
                step=step,
                phase=phase,
                hold=hold,
                candidate=False,
                precheck_reason=(
                    "no_executable_memory"
                    if hold == "closed_hold"
                    else "finger_contact"
                ),
            )
        )
        step_by_task[task] += 1
    for trace in traces:
        trace["memory_task_index"] = 5 if trace["task_id"] == 0 else 9
    memory = _memory()
    memory["admission_reasons"][5] = {
        "open_hold": 9,
        "gripper_ambiguous": 3,
        "gripper_command_mismatch": 3,
        "translation_limit": 2,
        "rotation_limit": 2,
    }
    memory["availability"][9] = {
        "open_hold": 0,
        "closed_hold": 8,
        "suppression": 11,
    }
    memory["admission_reasons"][9] = {
        "closed_hold": 8,
        "gripper_ambiguous": 3,
        "gripper_command_mismatch": 3,
        "translation_limit": 3,
        "rotation_limit": 2,
    }
    memory["admission_mask_counts"][5] = {
        0: 9,
        **{mask: 1 for mask in range(1, 11)},
    }
    memory["admission_mask_counts"][9] = {
        0: 8,
        **{mask: 1 for mask in range(1, 12)},
    }
    memory["task_texts"] = {
        task_index: task_description
        for _, task_index, task_description in (
            risk_router.TASK_MAPPING_CORRECTION_EXPECTED_MAP
        )
    }
    memory["text_to_task"] = {
        text: task for task, text in memory["task_texts"].items()
    }
    rows = _episode_rows(traces)
    for row in rows:
        row["task_description"] = memory["task_texts"][row["memory_task_index"]]
    monkeypatch.setattr(
        risk_router,
        "_semantic_manifest_sha256",
        lambda _path: risk_router.EXPERIMENT_031_SEMANTIC_MANIFEST_SHA256,
    )
    result = risk_router._compact_veto_separability(
        _args(
            tmp_path,
            experiment_id=34,
            run_id="20260718123456-1234567890",
            entrypoint_path=str(CORRECTION_WRAPPER_PATH),
        ),
        resolved_sha=risk_router.VLA_MODEL_SHA,
        memory=memory,
        rows=rows,
    )
    encoded = json.dumps(
        result, allow_nan=False, separators=(",", ":"), sort_keys=True
    ).encode()
    assert len(result["diagnostics"]["candidate_phases"]) == 12
    assert len(result["diagnostics"]["query_support"]) == 18
    assert len(encoded) <= 3850, len(encoded)


def test_detail_artifact_is_canonical_and_contains_no_raw_payload(tmp_path):
    candidate_rows = risk_router._veto_candidate_rows(
        [_trace(group="vetoed")], veto_margin=0.01, high_error_threshold=0.15
    )
    detail = risk_router._veto_detail_payload(
        candidate_rows,
        experiment_id=33,
        run_id="unit",
        task_map=[[0, 0, "task-0"]],
    )
    path = tmp_path / "detail.json"
    digest = risk_router._write_detail_result(path, detail)
    assert digest == hashlib.sha256(path.read_bytes()).hexdigest()
    payload = path.read_text()
    assert all(token not in payload for token in ("image_url", "embedding", '"action"'))
    assert json.loads(payload)["source_observability"] == {
        "contact_free_window": False,
        "phase": False,
        "subgoal": False,
    }
    decoded = json.loads(payload)
    assert decoded["schema"] == 2
    assert decoded["fields"][0] == "suite_task_id"
    assert decoded["task_map_fields"] == [
        "suite_task_id",
        "manifest_task_index",
        "task_description",
    ]
    assert decoded["codebooks"]["veto_group"] == list(
        risk_router.VETO_GROUP_NAMES
    )


def test_experiment_033_rows_anchor_and_legacy_renderer_remain_reproducible():
    legacy_detail = json.loads(LEGACY_DETAIL_PATH.read_text())
    assert risk_router._detail_rows_sha256(legacy_detail) == (
        risk_router.EXPERIMENT_033_CANDIDATE_ROWS_SHA256
    )
    legacy_compact = json.loads(LEGACY_COMPACT_PATH.read_text())
    report = risk_router.render_report(legacy_compact)
    assert "Legacy Schema Notice" in report
    assert "Legacy reported task key (unverified axis)" in report
    assert "Experiment 034 supplies the corrected attribution" in report


def test_error_result_is_bounded_by_encoded_bytes(tmp_path):
    result = risk_router._bounded_error_result(34, RuntimeError("\x00" * 3000))
    path = tmp_path / "error.json"
    risk_router._write_result(path, result)
    assert len(path.read_bytes()) <= 3900
    assert json.loads(path.read_text())["experiment"] == 34


def test_model_loader_propagates_frozen_revisions_to_every_hub_load(monkeypatch):
    calls = []

    class FakeLoaded:
        config = object()

        def to(self, device):
            calls.append(("to", device))
            return self

        def eval(self):
            return self

    class FakeAutoProcessor:
        @classmethod
        def from_pretrained(cls, model, **kwargs):
            calls.append(("siglip_processor", model, kwargs))
            return object()

    class FakeAutoModel:
        @classmethod
        def from_pretrained(cls, model, **kwargs):
            calls.append(("siglip_model", model, kwargs))
            return FakeLoaded()

    class FakePolicy:
        @classmethod
        def from_pretrained(cls, model, **kwargs):
            calls.append(("vla_model", model, kwargs))
            return FakeLoaded()

    class FakePipeline:
        @classmethod
        def from_pretrained(cls, **kwargs):
            calls.append(("processor_pipeline", kwargs))
            return object()

    fake_torch = ModuleType("torch")
    fake_torch.float16 = object()
    fake_torch.cuda = SimpleNamespace(
        is_available=lambda: True,
        synchronize=lambda: None,
    )
    fake_transformers = ModuleType("transformers")
    fake_transformers.AutoProcessor = FakeAutoProcessor
    fake_transformers.AutoModel = FakeAutoModel
    fake_factory = ModuleType("lerobot.policies.factory")
    fake_factory.make_pre_post_processors = lambda *_args, **_kwargs: pytest.fail(
        "pinned model loads must not use the unversioned processor factory"
    )
    fake_modeling = ModuleType("lerobot.policies.smolvla.modeling_smolvla")
    fake_modeling.SmolVLAPolicy = FakePolicy
    fake_processor = ModuleType("lerobot.processor")
    fake_processor.PolicyProcessorPipeline = FakePipeline
    fake_converters = ModuleType("lerobot.processor.converters")
    for name in (
        "batch_to_transition",
        "policy_action_to_transition",
        "transition_to_batch",
        "transition_to_policy_action",
    ):
        setattr(fake_converters, name, object())
    fake_constants = ModuleType("lerobot.utils.constants")
    fake_constants.POLICY_PREPROCESSOR_DEFAULT_NAME = "policy_preprocessor"
    fake_constants.POLICY_POSTPROCESSOR_DEFAULT_NAME = "policy_postprocessor"
    for name, module in {
        "torch": fake_torch,
        "transformers": fake_transformers,
        "lerobot.policies.factory": fake_factory,
        "lerobot.policies.smolvla.modeling_smolvla": fake_modeling,
        "lerobot.processor": fake_processor,
        "lerobot.processor.converters": fake_converters,
        "lerobot.utils.constants": fake_constants,
    }.items():
        monkeypatch.setitem(sys.modules, name, module)

    risk_router.exp26._load_models(
        SimpleNamespace(
            siglip_model=risk_router.SIGLIP_MODEL,
            siglip_revision=risk_router.SIGLIP_MODEL_SHA,
            vla_model=risk_router.VLA_MODEL,
            vla_revision=risk_router.VLA_MODEL_SHA,
        )
    )
    hub_calls = [call for call in calls if call[0] != "to"]
    assert hub_calls[0][2]["revision"] == risk_router.SIGLIP_MODEL_SHA
    assert hub_calls[1][2]["revision"] == risk_router.SIGLIP_MODEL_SHA
    assert hub_calls[2][2]["revision"] == risk_router.VLA_MODEL_SHA
    processor_calls = [call[1] for call in hub_calls[3:]]
    assert len(processor_calls) == 2
    assert all(
        call["revision"] == risk_router.VLA_MODEL_SHA
        for call in processor_calls
    )


def test_experiment_033_wrapper_forces_identity_and_mode(monkeypatch):
    sys.modules["physical_ai_vla_risk_router"] = risk_router
    wrapper_spec = importlib.util.spec_from_file_location(
        "physical_ai_vla_veto_separability", WRAPPER_PATH
    )
    assert wrapper_spec and wrapper_spec.loader
    wrapper = importlib.util.module_from_spec(wrapper_spec)
    wrapper_spec.loader.exec_module(wrapper)
    calls = []
    monkeypatch.setattr(
        wrapper.risk_router, "main", lambda **kwargs: calls.append(kwargs) or 0
    )
    assert wrapper.main() == 0
    assert calls == [
        {
            "default_experiment_id": 33,
            "force_experiment_id": 33,
            "force_diagnostic_only": True,
            "force_veto_separability": True,
        }
    ]


def test_experiment_033_live_execution_is_archived(monkeypatch):
    monkeypatch.setattr(sys, "argv", [str(WRAPPER_PATH)])
    with pytest.raises(SystemExit, match="2"):
        risk_router.main(
            default_experiment_id=33,
            force_experiment_id=33,
            force_diagnostic_only=True,
            force_veto_separability=True,
        )


def test_experiment_034_wrapper_forces_new_immutable_identity(monkeypatch):
    sys.modules["physical_ai_vla_risk_router"] = risk_router
    wrapper_spec = importlib.util.spec_from_file_location(
        "physical_ai_vla_task_mapping_correction", CORRECTION_WRAPPER_PATH
    )
    assert wrapper_spec and wrapper_spec.loader
    wrapper = importlib.util.module_from_spec(wrapper_spec)
    wrapper_spec.loader.exec_module(wrapper)
    calls = []
    monkeypatch.setattr(
        wrapper.risk_router, "main", lambda **kwargs: calls.append(kwargs) or 0
    )
    assert wrapper.main() == 0
    assert calls == [
        {
            "default_experiment_id": 34,
            "force_experiment_id": 34,
            "force_diagnostic_only": True,
            "force_veto_separability": True,
        }
    ]
    shared_runner = (
        ROOT / "tests" / "k8s" / "run_physical_ai_vla_closed_loop.sh"
    ).read_text()
    assert "Refusing to overwrite immutable experiment result" in shared_runner
    correction_runner = (
        ROOT
        / "tests"
        / "k8s"
        / "run_physical_ai_vla_task_mapping_correction.sh"
    ).read_text()
    assert "ZEPTO_VLA_EXPERIMENT=034" in correction_runner
    assert "ZEPTO_VLA_REQUIRE_TASK_MAPPING_CORRECTION=1" in correction_runner
    assert "did not satisfy the exact task-mapping correction schema" in shared_runner
    assert '.fields[0] == "suite_task_id"' in shared_runner
    assert '(.memory.availability | keys | sort) == ["5", "9"]' in shared_runner
    assert risk_router.EXPERIMENT_033_CANDIDATE_ROWS_SHA256 in shared_runner
    assert "publish_no_clobber" in shared_runner
    assert "os.link(temporary, target)" in shared_runner
    assert "jq -cS '.acceptance.aws_resources_deleted = true'" in shared_runner


def test_forced_main_overrides_scientific_cli_parameters(monkeypatch, tmp_path):
    manifest = _manifest(tmp_path / "manifest.json")
    result_path = tmp_path / "result.json"
    detail_path = tmp_path / "detail.json"
    captured = []
    detail = {"rows": []}
    detail_sha = hashlib.sha256(
        json.dumps(detail, separators=(",", ":"), sort_keys=True).encode()
    ).hexdigest()

    def fake_run(args):
        captured.append(args)
        args.veto_detail_payload = detail
        return {
            "status": "pass",
            "experiment": args.experiment_id,
            "detail": {"sha256": detail_sha},
        }

    monkeypatch.setattr(risk_router, "run_experiment", fake_run)
    monkeypatch.setattr(
        sys,
        "argv",
        [
            str(CORRECTION_WRAPPER_PATH),
            "--experiment-id",
            "999",
            "--task-ids",
            "1",
            "--calibration-seed",
            "9",
            "--veto-margin",
            "0.2",
            "--sample-manifest",
            str(manifest),
            "--result",
            str(result_path),
            "--detail-result",
            str(detail_path),
        ],
    )
    assert (
        risk_router.main(
            default_experiment_id=34,
            force_experiment_id=34,
            force_diagnostic_only=True,
            force_veto_separability=True,
        )
        == 0
    )
    args = captured[0]
    assert (args.experiment_id, args.task_ids, args.calibration_seed) == (34, "0,5", 28)
    assert args.veto_margin == 0.01
    assert args.diagnostic_only and args.veto_separability


def test_render_veto_report_preserves_noncausal_and_safety_boundaries(tmp_path):
    traces = [
        _trace(step=0, group="no_negative_support"),
        _trace(step=1, group="separated"),
        _trace(step=2, group="vetoed", mae=0.20),
        _trace(task=5, step=0, candidate=False, precheck_reason="no_executable_memory"),
    ]
    result = risk_router._compact_veto_separability(
        _args(tmp_path),
        resolved_sha=risk_router.VLA_MODEL_SHA,
        memory=_memory(),
        rows=_episode_rows(traces),
    )
    result["acceptance"]["aws_resources_deleted"] = True
    report = risk_router.render_report(result)
    assert "Negative-Veto Separability 033 Results" in report
    assert "no_negative_support" in report
    assert "not action correctness" in report
    assert "source contact windows, semantic phases" in report
    assert "not independent causal" in report
    assert "The veto remains unchanged" in report


def test_diagnostic_status_depends_on_accounting_not_separability(
    monkeypatch, tmp_path
):
    traces = [_trace(group="vetoed"), _trace(task=5, candidate=False)]
    args = _args(tmp_path)
    good = risk_router._compact_veto_separability(
        args,
        resolved_sha=risk_router.VLA_MODEL_SHA,
        memory=_memory(),
        rows=_episode_rows(traces),
    )
    assert good["status"] == "pass"
    assert good["finding"]["separation_status"] == "underpowered"

    monkeypatch.setattr(risk_router, "_veto_phase_summary", lambda _rows: [])
    bad = risk_router._compact_veto_separability(
        _args(tmp_path / "bad"),
        resolved_sha=risk_router.VLA_MODEL_SHA,
        memory=_memory(),
        rows=_episode_rows(traces),
    )
    assert bad["status"] == "fail"
    assert bad["completion_reason"] == "diagnostic_telemetry_incomplete"


def test_diagnostic_rejects_action_admission_and_structural_corruption(
    monkeypatch, tmp_path
):
    traces = [
        _trace(group="separated"),
        _trace(task=5, group="vetoed"),
    ]
    action_rows = _episode_rows(traces)
    action_rows[0]["reuses"] = 1
    action_result = risk_router._compact_veto_separability(
        _args(tmp_path / "action"),
        resolved_sha=risk_router.VLA_MODEL_SHA,
        memory=_memory(),
        rows=action_rows,
    )
    assert action_result["status"] == "fail"
    assert action_result["acceptance"]["routed_actions_zero"] is False

    bad_memory = _memory()
    bad_memory["admission_mask_counts"][0] = {0: 19}
    admission_result = risk_router._compact_veto_separability(
        _args(tmp_path / "admission"),
        resolved_sha=risk_router.VLA_MODEL_SHA,
        memory=bad_memory,
        rows=_episode_rows(traces),
    )
    assert admission_result["status"] == "fail"
    assert admission_result["acceptance"]["source_admission_consistent"] is False

    original = risk_router._veto_structural_summary

    def corrupt_structural(*args, **kwargs):
        pooled, by_task = original(*args, **kwargs)
        pooled[2] += 1
        return pooled, by_task

    monkeypatch.setattr(
        risk_router, "_veto_structural_summary", corrupt_structural
    )
    structural_result = risk_router._compact_veto_separability(
        _args(tmp_path / "structural"),
        resolved_sha=risk_router.VLA_MODEL_SHA,
        memory=_memory(),
        rows=_episode_rows(traces),
    )
    assert structural_result["status"] == "fail"
    assert structural_result["acceptance"]["task_structural_consistent"] is False


def test_run_experiment_033_stops_after_calibration_shadow(monkeypatch, tmp_path):
    fake_torch = ModuleType("torch")
    fake_huggingface = ModuleType("huggingface_hub")
    model_info_calls = []

    def fake_model_info(model, *, revision):
        model_info_calls.append((model, revision))
        return SimpleNamespace(sha=revision)

    fake_huggingface.model_info = fake_model_info
    monkeypatch.setitem(sys.modules, "torch", fake_torch)
    monkeypatch.setitem(sys.modules, "huggingface_hub", fake_huggingface)
    monkeypatch.setattr(risk_router.exp28, "parse_task_ids", lambda _value: [0, 5])
    policy = SimpleNamespace(config=SimpleNamespace(n_action_steps=1))
    load_args = []

    def fake_load_models(args):
        load_args.append(args)
        return object(), object(), policy, object(), object()

    monkeypatch.setattr(risk_router.exp26, "_load_models", fake_load_models)
    monkeypatch.setattr(
        risk_router.exp27, "AgentMemoryClient", lambda *_args, **_kwargs: object()
    )
    memory = _memory()
    monkeypatch.setattr(
        risk_router, "_prepare_memory_bank", lambda *_args: (memory, {})
    )
    variants = []

    def fake_episode(*, variant, task_id, seed, **_kwargs):
        variants.append(variant)
        trace = _trace(task=task_id, group="vetoed")
        trace["episode_id"] = seed
        return {
            "task_id": task_id,
            "memory_task_index": task_id,
            "task_description": f"task-{task_id}",
            "steps": 1,
            "vla_calls": 1,
            "reuses": 0,
            "traces": [trace],
            "search_wall": [trace["candidate"]["search_wall"]],
            "precheck_counts": {"eligible": 1},
        }

    monkeypatch.setattr(risk_router, "_run_episode", fake_episode)
    monkeypatch.setattr(
        risk_router,
        "select_region",
        lambda *_args, **_kwargs: pytest.fail("Experiment 033 must not run a grid"),
    )
    args = _args(
        tmp_path,
        task_ids="0,5",
        vla_model=risk_router.VLA_MODEL,
        agent_url="http://unused",
        http_timeout=1.0,
        base_policy_seed=310000,
        calibration_seed=28,
        veto_separability=True,
    )
    result = risk_router.run_experiment(args)
    assert variants == ["shadow", "shadow"]
    assert result["status"] == "pass"
    assert result["execution"] == {
        "diagnostic_only": True,
        "routed_started": False,
    }
    assert result["acceptance"]["routed_actions_zero"] is True
    assert model_info_calls == [
        (risk_router.VLA_MODEL, risk_router.VLA_MODEL_SHA),
        (risk_router.SIGLIP_MODEL, risk_router.SIGLIP_MODEL_SHA),
    ]
    assert len(load_args) == 1
    assert load_args[0].vla_revision == risk_router.VLA_MODEL_SHA
    assert load_args[0].siglip_revision == risk_router.SIGLIP_MODEL_SHA
