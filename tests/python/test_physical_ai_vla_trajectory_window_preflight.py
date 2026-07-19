from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import urllib.error

import pytest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = (
    ROOT
    / "docs"
    / "research"
    / "tools"
    / "physical_ai_vla_trajectory_window_preflight.py"
)
PRIOR_PATH = (
    ROOT
    / "docs"
    / "research"
    / "results"
    / "physical_ai_vla_veto_separability_compact_034.json"
)
SPEC = importlib.util.spec_from_file_location(
    "physical_ai_vla_trajectory_window_preflight", MODULE_PATH
)
assert SPEC and SPEC.loader
preflight = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(preflight)


class _Response:
    def __init__(self, payload: bytes):
        self.payload = payload

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self, limit: int) -> bytes:
        return self.payload[:limit]


def _prior() -> dict:
    return {
        "experiment": 34,
        "status": "pass",
        "completion_reason": "veto_separability_complete",
        "manifest_semantic_sha256": preflight.EXPECTED_MANIFEST_SHA256,
        "harness_bundle_sha256": preflight.EXPECTED_HARNESS_SHA256,
        "detail": {"rows": 127, "sha256": preflight.EXPECTED_DETAIL_SHA256},
        "scope": {
            "steps": 595,
            "suite_tasks": [0, 5],
            "task_map": preflight.EXPECTED_TASK_MAP,
        },
        "memory": {
            "records": 190,
            "availability": preflight.EXPECTED_AVAILABILITY,
        },
        "shadow": {
            "action_accounting": [595, 0],
            "task_precheck": [
                [0, 377, [127, 0, 242, 1, 7, 0, 0, 0]],
                [5, 218, [0, 0, 148, 1, 5, 0, 6, 58]],
            ],
        },
        "diagnostics": {
            "schema": 2,
            "pooled_structural": preflight.EXPECTED_POOLED_STRUCTURAL,
            "task_structural": preflight.EXPECTED_TASK_STRUCTURAL,
        },
        "acceptance": {"accounting": True, "aws_resources_deleted": True},
    }


def _dataset_rows() -> list[dict]:
    return [
        {
            "repo": specification["repo"],
            "revision": specification["revision"],
            "total_episodes": specification["total_episodes"],
            "total_frames": specification["total_frames"],
            "aligned_with_frozen_source": specification[
                "aligned_with_frozen_source"
            ],
            "feature_names": sorted(specification["features"]),
            "authoritative_contact_features": [],
            "contact_named_features": [],
            "contact_provenance_validated": False,
            "semantic_phase_features": (
                ["subtask_index"]
                if "subtask_index" in specification["features"]
                else []
            ),
        }
        for specification in preflight.DATASETS
    ]


def _dataset_fetcher(specification: dict, *, revision_override: str | None = None):
    def fetch(url: str) -> dict:
        if "api/datasets" in url:
            return {
                "id": specification["repo"],
                "sha": revision_override or specification["revision"],
            }
        if url.endswith("meta/info.json"):
            return {
                "total_episodes": specification["total_episodes"],
                "total_frames": specification["total_frames"],
                "total_tasks": 10,
            }
        return {
            "dataset_info": {
                "default": {
                    "features": {
                        name: {"_type": "Value"}
                        for name in specification["features"]
                    }
                }
            }
        }

    return fetch


def test_structural_preflight_finds_bank_independent_task_zero_ceiling():
    result = preflight.structural_preflight(_prior())

    task_zero, task_five = result["tasks"]
    assert task_zero["candidate_plus_cooldown"] == 65
    assert task_zero["required_reuse_actions"] == 76
    assert task_zero["reuse_ceiling"] == pytest.approx(65 / 377)
    assert task_zero["bank_independent_under_frozen_precheck"] is True
    assert task_zero["current_latency_pass"] is True
    assert task_five["missing_executable_memory"] == 58
    assert task_five["bank_independent_under_frozen_precheck"] is False
    assert result["bank_independent_blockers"] == [
        "suite_task_0_frozen_precheck_candidate_cooldown_below_floor"
    ]
    assert result["bank_independent_under_frozen_precheck_viable"] is False


def test_required_reuse_count_uses_ceil_boundary():
    assert preflight._required_reuse_count(5) == 1
    assert preflight._required_reuse_count(377) == 76
    with pytest.raises(ValueError, match="steps must be positive"):
        preflight._required_reuse_count(0)


def test_structural_preflight_rejects_candidate_precheck_mismatch():
    prior = _prior()
    prior["shadow"]["task_precheck"][0][2][0] = 126

    with pytest.raises(ValueError, match="candidate/precheck"):
        preflight.structural_preflight(prior)


def test_validate_prior_result_requires_exact_digest_and_conservation(
    tmp_path, monkeypatch
):
    path = tmp_path / "prior.json"
    raw = json.dumps(_prior(), separators=(",", ":")).encode()
    path.write_bytes(raw)
    monkeypatch.setattr(
        preflight, "EXPECTED_PRIOR_SHA256", hashlib.sha256(raw).hexdigest()
    )

    prior, digest = preflight.validate_prior_result(path)

    assert prior["experiment"] == 34
    assert digest == hashlib.sha256(raw).hexdigest()

    corrupt = json.loads(raw)
    corrupt["shadow"]["task_precheck"][0][2][2] -= 1
    corrupt_raw = json.dumps(corrupt, separators=(",", ":")).encode()
    path.write_bytes(corrupt_raw)
    monkeypatch.setattr(
        preflight, "EXPECTED_PRIOR_SHA256", hashlib.sha256(corrupt_raw).hexdigest()
    )
    with pytest.raises(ValueError, match="do not conserve"):
        preflight.validate_prior_result(path)


def test_validate_prior_result_rejects_wrong_raw_digest(tmp_path):
    path = tmp_path / "prior.json"
    path.write_text(json.dumps(_prior()))

    with pytest.raises(ValueError, match="compact SHA-256"):
        preflight.validate_prior_result(path)


def test_repository_prior_artifact_is_directly_reproducible():
    prior, digest = preflight.validate_prior_result(PRIOR_PATH)

    assert prior["experiment"] == 34
    assert digest == preflight.EXPECTED_PRIOR_SHA256


def test_inspect_dataset_validates_revision_and_observability():
    base = preflight.inspect_dataset(
        preflight.DATASETS[0], fetcher=_dataset_fetcher(preflight.DATASETS[0])
    )
    subtask = preflight.inspect_dataset(
        preflight.DATASETS[1], fetcher=_dataset_fetcher(preflight.DATASETS[1])
    )

    assert base["authoritative_contact_features"] == []
    assert base["semantic_phase_features"] == []
    assert base["aligned_with_frozen_source"] is True
    assert base["total_episodes"] == 379
    assert subtask["authoritative_contact_features"] == []
    assert subtask["semantic_phase_features"] == ["subtask_index"]
    assert subtask["aligned_with_frozen_source"] is False
    assert subtask["total_episodes"] == 500


def test_inspect_dataset_rejects_revision_or_schema_drift():
    specification = preflight.DATASETS[0]
    with pytest.raises(ValueError, match="revision identity"):
        preflight.inspect_dataset(
            specification,
            fetcher=_dataset_fetcher(specification, revision_override="bad"),
        )

    def missing_feature(url: str) -> dict:
        payload = _dataset_fetcher(specification)(url)
        if "dataset_info" in payload:
            payload["dataset_info"]["default"]["features"].pop("action")
        return payload

    with pytest.raises(ValueError, match="feature schema"):
        preflight.inspect_dataset(specification, fetcher=missing_feature)

    def wrong_episode_count(url: str) -> dict:
        payload = _dataset_fetcher(specification)(url)
        if url.endswith("meta/info.json"):
            payload["total_episodes"] += 1
        return payload

    with pytest.raises(ValueError, match="episode metadata"):
        preflight.inspect_dataset(specification, fetcher=wrong_episode_count)


def test_inspect_dataset_rejects_malformed_nested_schema():
    specification = preflight.DATASETS[0]

    def malformed(url: str) -> dict:
        if "api/datasets" in url:
            return {"id": specification["repo"], "sha": specification["revision"]}
        return {"dataset_info": None}

    with pytest.raises(ValueError, match="invalid dataset_info"):
        preflight.inspect_dataset(specification, fetcher=malformed)


def test_contact_field_name_alone_is_not_authoritative():
    specification = {
        **preflight.DATASETS[0],
        "features": set(preflight.DATASETS[0]["features"])
        | {"observation.contacts"},
        "validated_contact_provenance": False,
    }

    result = preflight.inspect_dataset(
        specification, fetcher=_dataset_fetcher(specification)
    )

    assert result["contact_named_features"] == ["observation.contacts"]
    assert result["contact_provenance_validated"] is False
    assert result["authoritative_contact_features"] == []


def test_request_json_retries_network_failure_without_unbounded_wait():
    calls = 0
    sleeps = []

    def opener(_request, *, timeout):
        nonlocal calls
        calls += 1
        assert timeout == 1.0
        if calls == 1:
            raise urllib.error.URLError("temporary")
        return _Response(b'{"ok":true}')

    result = preflight.request_json(
        "https://example.test/data",
        attempts=2,
        timeout=1.0,
        opener=opener,
        sleeper=sleeps.append,
    )

    assert result == {"ok": True}
    assert calls == 2
    assert sleeps == [1.0]


def test_request_json_honors_bounded_retry_after_for_rate_limit():
    calls = 0
    sleeps = []

    def opener(request, *, timeout):
        nonlocal calls
        calls += 1
        if calls == 1:
            raise urllib.error.HTTPError(
                request.full_url,
                429,
                "rate limited",
                {"Retry-After": "4"},
                None,
            )
        return _Response(b'{"ok":true}')

    result = preflight.request_json(
        "https://example.test/rate-limit",
        attempts=2,
        opener=opener,
        sleeper=sleeps.append,
    )

    assert result == {"ok": True}
    assert sleeps == [4.0]


def test_request_json_rejects_oversize_malformed_and_exhausted_responses():
    with pytest.raises(RuntimeError, match="exceeds 4 bytes"):
        preflight.request_json(
            "https://example.test/large",
            attempts=1,
            maximum_bytes=4,
            opener=lambda *_args, **_kwargs: _Response(b"12345"),
        )
    with pytest.raises(RuntimeError, match="failed after 1 attempts"):
        preflight.request_json(
            "https://example.test/malformed",
            attempts=1,
            opener=lambda *_args, **_kwargs: _Response(b"not-json"),
        )
    with pytest.raises(RuntimeError, match="failed after 2 attempts"):
        preflight.request_json(
            "https://example.test/timeout",
            attempts=2,
            opener=lambda *_args, **_kwargs: (_ for _ in ()).throw(TimeoutError()),
            sleeper=lambda _delay: None,
        )


def test_build_result_fails_closed_before_cloud_or_vla():
    result = preflight.build_result(
        _prior(), "a" * 64, _dataset_rows(), generated_at="2026-07-19T00:00:00+00:00"
    )

    assert result["status"] == "pass"
    assert result["finding"] == "structural_abort"
    assert result["completion_reason"] == "pre_vla_structural_abort"
    assert result["progression"]["allowed"] is False
    assert result["progression"]["blockers"] == [
        "source_contact_unobservable",
        "frozen_source_semantic_phase_unobservable",
        "suite_task_0_frozen_precheck_candidate_cooldown_below_floor",
    ]
    assert result["execution"] == {
        "source_rows_downloaded": 0,
        "source_images_downloaded": 0,
        "models_loaded": 0,
        "vla_calls": 0,
        "retrieved_actions": 0,
        "aws_resources_created": 0,
    }
    assert result["acceptance"]["diagnostic_valid"] is True
    assert result["acceptance"]["dataset_feature_name_sets_and_counts_valid"] is True
    assert result["acceptance"]["contact_provenance_available"] is False
    assert result["acceptance"]["frozen_source_semantic_phase_available"] is False
    assert (
        result["acceptance"][
            "bank_independent_under_frozen_precheck_viable"
        ]
        is False
    )
    assert result["source_observability"]["auxiliary_semantic_subtask_feature"] is True
    assert result["source_observability"]["frozen_source_semantic_phase"] is False
    assert (
        result["source_observability"][
            "historical_simulator_replay_mapping_validated"
        ]
        is False
    )
    assert b"NaN" not in preflight._canonical_json(result)
    assert len(result["preflight_tool_sha256"]) == 64
    assert len(result["dataset_contract_sha256"]) == 64
    assert result["frozen_precheck_policy"]["recompute_from_treatment_bank"] is False


def test_contact_feature_does_not_override_structural_abort():
    datasets = _dataset_rows()
    datasets[0]["authoritative_contact_features"] = ["observation.contacts"]
    datasets[0]["contact_provenance_validated"] = True

    result = preflight.build_result(_prior(), "a" * 64, datasets)

    assert result["source_observability"]["authoritative_contact"] is True
    assert result["progression"]["allowed"] is False
    assert result["progression"]["blockers"] == [
        "frozen_source_semantic_phase_unobservable",
        "suite_task_0_frozen_precheck_candidate_cooldown_below_floor"
    ]


def test_render_report_keeps_safety_boundary_and_resource_accounting():
    result = preflight.build_result(_prior(), "a" * 64, _dataset_rows())
    report = preflight.render_report(result)

    assert "65/377" in report
    assert "required 76/377" in report
    assert "AWS/EKS resources created: 0" in report
    assert "no action-correctness, risk-free, or safety evidence" in report


def test_no_clobber_writer_preserves_existing_artifact(tmp_path):
    path = tmp_path / "result.json"
    preflight._write_no_clobber(path, b"first")

    with pytest.raises(FileExistsError, match="refusing to overwrite"):
        preflight._write_no_clobber(path, b"second")

    assert path.read_bytes() == b"first"
    assert path.stat().st_mode & 0o777 == 0o600


def test_pair_writer_rejects_alias_and_existing_second_target(tmp_path):
    same = tmp_path / "same"
    with pytest.raises(ValueError, match="must be distinct"):
        preflight._write_pair_no_clobber(same, b"json", same, b"report")
    assert not same.exists()

    result = tmp_path / "result.json"
    report = tmp_path / "report.md"
    report.write_text("existing")
    with pytest.raises(FileExistsError, match="refusing to overwrite"):
        preflight._write_pair_no_clobber(result, b"json", report, b"report")
    assert not result.exists()
    assert report.read_text() == "existing"


def test_pair_writer_rolls_back_first_link_when_second_link_fails(
    tmp_path, monkeypatch
):
    result = tmp_path / "result.json"
    report = tmp_path / "report.md"
    real_link = preflight.os.link
    calls = 0

    def link(source, target):
        nonlocal calls
        calls += 1
        if calls == 2:
            raise OSError("simulated report publication failure")
        return real_link(source, target)

    monkeypatch.setattr(preflight.os, "link", link)
    with pytest.raises(OSError, match="simulated"):
        preflight._write_pair_no_clobber(result, b"json", report, b"report")

    assert not result.exists()
    assert not report.exists()
    assert list(tmp_path.glob(".*.tmp")) == []


def test_pair_writer_rolls_back_first_link_when_interrupted(tmp_path, monkeypatch):
    result = tmp_path / "result.json"
    report = tmp_path / "report.md"
    real_link = preflight.os.link
    calls = 0

    def link(source, target):
        nonlocal calls
        calls += 1
        if calls == 2:
            real_link(source, target)
            raise KeyboardInterrupt()
        return real_link(source, target)

    monkeypatch.setattr(preflight.os, "link", link)
    with pytest.raises(KeyboardInterrupt):
        preflight._write_pair_no_clobber(result, b"json", report, b"report")

    assert not result.exists()
    assert not report.exists()
    assert list(tmp_path.glob(".*.tmp")) == []
