#!/usr/bin/env python3
"""Run Experiment 035's fail-closed trajectory-window preflight."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable


EXPERIMENT = 35
METRIC_SCALE = 1_000_000
REUSE_FLOOR = 0.20
LATENCY_FLOOR = 0.15
MAX_HTTP_BYTES = 1_000_000
EXPECTED_PRIOR_SHA256 = (
    "6320991252857db66ae06f2772a6654813327a09944ac0c39ef37fb1c5be733d"
)
EXPECTED_MANIFEST_SHA256 = (
    "0624630ce232f33c36dbe20159ce3e88729ab0feffb6b67905f4bd2b180e85ba"
)
EXPECTED_DETAIL_SHA256 = (
    "f41225183345e526503bc24576567a7c461e348f4927486bb6b339ab2c84de56"
)
EXPECTED_HARNESS_SHA256 = (
    "881ccf6420d002dbd0693b323229628ed409c1a1b05abcd8ceb0a0453bf037a5"
)
EXPECTED_TASK_MAP = [
    [
        0,
        5,
        "put both the alphabet soup and the tomato sauce in the basket",
    ],
    [
        5,
        9,
        "pick up the book and place it in the back compartment of the caddy",
    ],
]
EXPECTED_AVAILABILITY = {
    "5": {"open_hold": 9, "closed_hold": 0, "suppression": 10},
    "9": {"open_hold": 0, "closed_hold": 8, "suppression": 11},
}
PRECHECK_REASON_NAMES = (
    "eligible",
    "arm_contact",
    "finger_contact",
    "first_observation",
    "gripper_motion",
    "gripper_ambiguous",
    "state_outlier",
    "no_executable_memory",
)
EXPECTED_POOLED_PRECHECK = [127, 0, 390, 2, 12, 0, 6, 58]
EXPECTED_POOLED_STRUCTURAL = [595, 127, 65, 65, 123, 4, 2, 101333, -4907]
EXPECTED_TASK_STRUCTURAL = [
    [0, 377, 127, 65, 65, 123, 4, 2, 159610, -7729],
    [5, 218, 0, 0, 0, 0, 0, 0, 0, 0],
]
AUTHORITATIVE_CONTACT_FEATURES = {
    "contact",
    "contact_pairs",
    "observation.contact",
    "observation.contact_forces",
    "observation.contact_pairs",
    "observation.contacts",
    "observation.robot_contacts",
    "robot_contacts",
}
SEMANTIC_PHASE_FEATURES = {"subtask_index"}
DATASETS = (
    {
        "repo": "lerobot/libero_10_image",
        "revision": "7e324b526699f444044952c82ce3f438e8d300d0",
        "total_episodes": 379,
        "total_frames": 101469,
        "aligned_with_frozen_source": True,
        "validated_contact_provenance": False,
        "features": {
            "action",
            "episode_index",
            "frame_index",
            "index",
            "observation.images.image",
            "observation.images.wrist_image",
            "observation.state",
            "task_index",
            "timestamp",
        },
    },
    {
        "repo": "lerobot/libero_10_image_subtask",
        "revision": "06fdb000a8d6d3f43c79abb2545a24379265bef8",
        "total_episodes": 500,
        "total_frames": 138090,
        "aligned_with_frozen_source": False,
        "validated_contact_provenance": False,
        "features": {
            "action",
            "episode_index",
            "frame_index",
            "index",
            "observation.images.image",
            "observation.images.image2",
            "observation.state",
            "subtask_index",
            "task_index",
            "timestamp",
        },
    },
)
DEFAULT_PRIOR_RESULT = (
    Path(__file__).resolve().parent.parent
    / "results"
    / "physical_ai_vla_veto_separability_compact_034.json"
)


def _canonical_json(value: Any) -> bytes:
    return json.dumps(
        value, allow_nan=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")


def _tool_sha256() -> str:
    return hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def _dataset_contract_sha256() -> str:
    contract = [
        {
            **{key: value for key, value in row.items() if key != "features"},
            "features": sorted(row["features"]),
        }
        for row in DATASETS
    ]
    return hashlib.sha256(_canonical_json(contract)).hexdigest()


def _assert_finite(value: Any, path: str = "result") -> None:
    if isinstance(value, float) and not math.isfinite(value):
        raise ValueError(f"{path} contains a non-finite number")
    if isinstance(value, dict):
        for key, child in value.items():
            _assert_finite(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _assert_finite(child, f"{path}[{index}]")


def _read_bounded(response: Any, maximum_bytes: int) -> bytes:
    if maximum_bytes <= 0:
        raise ValueError("maximum response bytes must be positive")
    payload = response.read(maximum_bytes + 1)
    if len(payload) > maximum_bytes:
        raise RuntimeError(f"HTTP response exceeds {maximum_bytes} bytes")
    return payload


def request_json(
    url: str,
    *,
    attempts: int = 3,
    timeout: float = 30.0,
    maximum_bytes: int = MAX_HTTP_BYTES,
    opener: Callable[..., Any] = urllib.request.urlopen,
    sleeper: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    """Fetch one bounded JSON object with retryable network failure handling."""
    if attempts <= 0 or timeout <= 0.0:
        raise ValueError("HTTP attempts and timeout must be positive")
    last_error: Exception | None = None
    for attempt in range(attempts):
        request = urllib.request.Request(
            url, headers={"User-Agent": "zeptodb-research/035"}
        )
        try:
            with opener(request, timeout=timeout) as response:
                raw = _read_bounded(response, maximum_bytes)
            payload = json.loads(raw)
            if not isinstance(payload, dict):
                raise RuntimeError("HTTP JSON response is not an object")
            return payload
        except (
            json.JSONDecodeError,
            urllib.error.HTTPError,
            urllib.error.URLError,
            TimeoutError,
        ) as exc:
            last_error = exc
            if attempt + 1 == attempts:
                break
            delay = min(2.0**attempt, 5.0)
            if isinstance(exc, urllib.error.HTTPError) and exc.code == 429:
                try:
                    delay = max(delay, min(float(exc.headers.get("Retry-After", 0)), 5.0))
                except (TypeError, ValueError):
                    pass
            sleeper(delay)
    raise RuntimeError(f"request failed after {attempts} attempts") from last_error


def _dataset_urls(repo: str, revision: str) -> tuple[str, str, str]:
    encoded_repo = urllib.parse.quote(repo, safe="/")
    encoded_revision = urllib.parse.quote(revision, safe="")
    identity = (
        f"https://huggingface.co/api/datasets/{encoded_repo}/revision/"
        f"{encoded_revision}"
    )
    schema = (
        "https://datasets-server.huggingface.co/info?"
        + urllib.parse.urlencode({"dataset": repo, "revision": revision})
    )
    metadata = (
        f"https://huggingface.co/datasets/{encoded_repo}/resolve/"
        f"{encoded_revision}/meta/info.json"
    )
    return identity, schema, metadata


def inspect_dataset(
    specification: dict[str, Any],
    *,
    fetcher: Callable[[str], dict[str, Any]] = request_json,
) -> dict[str, Any]:
    repo = str(specification["repo"])
    revision = str(specification["revision"])
    identity_url, schema_url, metadata_url = _dataset_urls(repo, revision)
    identity = fetcher(identity_url)
    if identity.get("id") != repo or identity.get("sha") != revision:
        raise ValueError(f"dataset revision identity mismatch for {repo}")
    schema = fetcher(schema_url)
    dataset_info = schema.get("dataset_info")
    if not isinstance(dataset_info, dict):
        raise ValueError(f"dataset schema has invalid dataset_info for {repo}")
    default_info = dataset_info.get("default")
    if not isinstance(default_info, dict):
        raise ValueError(f"dataset schema has invalid default config for {repo}")
    features = default_info.get("features")
    if not isinstance(features, dict):
        raise ValueError(f"dataset schema is missing default features for {repo}")
    feature_names = set(map(str, features))
    expected_features = set(map(str, specification["features"]))
    if feature_names != expected_features:
        raise ValueError(f"dataset feature schema mismatch for {repo}")
    metadata = fetcher(metadata_url)
    expected_episodes = int(specification["total_episodes"])
    expected_frames = int(specification["total_frames"])
    if (
        metadata.get("total_episodes") != expected_episodes
        or metadata.get("total_frames") != expected_frames
        or metadata.get("total_tasks") != 10
    ):
        raise ValueError(f"dataset episode metadata mismatch for {repo}")
    contact_named = sorted(feature_names & AUTHORITATIVE_CONTACT_FEATURES)
    contact_validated = bool(specification["validated_contact_provenance"])
    contact = contact_named if contact_validated else []
    semantic_phase = sorted(feature_names & SEMANTIC_PHASE_FEATURES)
    any_contact_named = sorted(
        name for name in feature_names if "contact" in name.lower()
    )
    return {
        "repo": repo,
        "revision": revision,
        "total_episodes": expected_episodes,
        "total_frames": expected_frames,
        "aligned_with_frozen_source": bool(
            specification["aligned_with_frozen_source"]
        ),
        "feature_names": sorted(feature_names),
        "authoritative_contact_features": contact,
        "contact_named_features": any_contact_named,
        "contact_provenance_validated": contact_validated,
        "semantic_phase_features": semantic_phase,
    }


def inspect_datasets(
    *, fetcher: Callable[[str], dict[str, Any]] = request_json
) -> list[dict[str, Any]]:
    return [inspect_dataset(specification, fetcher=fetcher) for specification in DATASETS]


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate_prior_result(path: Path) -> tuple[dict[str, Any], str]:
    raw = path.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    _require(digest == EXPECTED_PRIOR_SHA256, "Experiment 034 compact SHA-256 mismatch")
    prior = json.loads(raw)
    _require(isinstance(prior, dict), "Experiment 034 compact result is not an object")
    _assert_finite(prior, "prior")
    _require(prior.get("experiment") == 34, "prior experiment identity mismatch")
    _require(prior.get("status") == "pass", "prior diagnostic did not pass")
    _require(
        prior.get("completion_reason") == "veto_separability_complete",
        "prior completion reason mismatch",
    )
    _require(
        prior.get("manifest_semantic_sha256") == EXPECTED_MANIFEST_SHA256,
        "prior semantic manifest mismatch",
    )
    _require(
        prior.get("harness_bundle_sha256") == EXPECTED_HARNESS_SHA256,
        "prior harness bundle mismatch",
    )
    _require(
        prior.get("detail")
        == {"rows": 127, "sha256": EXPECTED_DETAIL_SHA256},
        "prior detail identity mismatch",
    )
    scope = prior.get("scope", {})
    _require(scope.get("steps") == 595, "prior step count mismatch")
    _require(scope.get("suite_tasks") == [0, 5], "prior suite task scope mismatch")
    _require(scope.get("task_map") == EXPECTED_TASK_MAP, "prior task map mismatch")
    _require(
        prior.get("memory", {}).get("availability") == EXPECTED_AVAILABILITY,
        "prior memory availability mismatch",
    )
    _require(
        prior.get("memory", {}).get("records") == 190,
        "prior memory record count mismatch",
    )
    _require(
        prior.get("shadow", {}).get("action_accounting") == [595, 0],
        "prior action accounting mismatch",
    )
    diagnostics = prior.get("diagnostics", {})
    _require(diagnostics.get("schema") == 2, "prior diagnostic schema mismatch")
    _require(
        diagnostics.get("pooled_structural") == EXPECTED_POOLED_STRUCTURAL,
        "prior pooled structural fingerprint mismatch",
    )
    _require(
        diagnostics.get("task_structural") == EXPECTED_TASK_STRUCTURAL,
        "prior task structural fingerprint mismatch",
    )
    task_precheck = prior.get("shadow", {}).get("task_precheck")
    _require(isinstance(task_precheck, list), "prior task precheck is missing")
    pooled_precheck = [0] * len(PRECHECK_REASON_NAMES)
    total_steps = 0
    for row in task_precheck:
        _require(
            isinstance(row, list) and len(row) == 3 and len(row[2]) == len(pooled_precheck),
            "prior task precheck row is malformed",
        )
        _require(sum(map(int, row[2])) == int(row[1]), "task precheck counts do not conserve")
        total_steps += int(row[1])
        pooled_precheck = [
            current + int(value) for current, value in zip(pooled_precheck, row[2])
        ]
    _require(total_steps == 595, "task precheck steps do not conserve")
    _require(pooled_precheck == EXPECTED_POOLED_PRECHECK, "prior precheck fingerprint mismatch")
    acceptance = prior.get("acceptance")
    _require(
        isinstance(acceptance, dict) and acceptance and all(acceptance.values()),
        "prior diagnostic acceptance is incomplete",
    )
    return prior, digest


def _required_reuse_count(steps: int) -> int:
    if steps <= 0:
        raise ValueError("task steps must be positive")
    return math.ceil(REUSE_FLOOR * steps)


def _precheck_by_task(prior: dict[str, Any]) -> dict[int, list[int]]:
    return {
        int(task): list(map(int, counts))
        for task, _steps, counts in prior["shadow"]["task_precheck"]
    }


def structural_preflight(prior: dict[str, Any]) -> dict[str, Any]:
    precheck = _precheck_by_task(prior)
    task_rows = []
    bank_independent_blockers = []
    for row in prior["diagnostics"]["task_structural"]:
        (
            task,
            steps,
            candidates,
            candidate_cooldown,
            _safety_cooldown,
            _vetoed,
            _fixed_pre,
            _fixed_cooldown,
            candidate_latency_scaled,
            _fixed_latency_scaled,
        ) = map(int, row)
        counts = precheck[task]
        eligible = counts[PRECHECK_REASON_NAMES.index("eligible")]
        missing_memory = counts[PRECHECK_REASON_NAMES.index("no_executable_memory")]
        _require(eligible == candidates, f"task {task} candidate/precheck accounting mismatch")
        required = _required_reuse_count(steps)
        reuse_rate = candidate_cooldown / steps
        candidate_latency = candidate_latency_scaled / METRIC_SCALE
        bank_independent_under_frozen_precheck = (
            eligible == candidates and missing_memory == 0
        )
        reuse_pass = candidate_cooldown >= required
        latency_pass = candidate_latency >= LATENCY_FLOOR
        if bank_independent_under_frozen_precheck and not reuse_pass:
            bank_independent_blockers.append(
                f"suite_task_{task}_frozen_precheck_candidate_cooldown_below_floor"
            )
        if bank_independent_under_frozen_precheck and not latency_pass:
            bank_independent_blockers.append(
                f"suite_task_{task}_frozen_precheck_projected_latency_below_floor"
            )
        task_rows.append(
            {
                "suite_task_id": task,
                "manifest_task_index": EXPECTED_TASK_MAP[[0, 5].index(task)][1],
                "steps": steps,
                "precheck_eligible": eligible,
                "missing_executable_memory": missing_memory,
                "candidates": candidates,
                "candidate_plus_cooldown": candidate_cooldown,
                "required_reuse_actions": required,
                "reuse_ceiling": reuse_rate,
                "projected_latency_reduction": candidate_latency,
                "bank_independent_under_frozen_precheck": (
                    bank_independent_under_frozen_precheck
                ),
                "current_reuse_pass": reuse_pass,
                "current_latency_pass": latency_pass,
            }
        )
    pooled = list(map(int, prior["diagnostics"]["pooled_structural"]))
    pooled_steps, pooled_candidates, pooled_cooldown = pooled[:3]
    pooled_required = _required_reuse_count(pooled_steps)
    pooled_latency = pooled[7] / METRIC_SCALE
    result = {
        "reuse_floor": REUSE_FLOOR,
        "latency_floor": LATENCY_FLOOR,
        "cooldown_steps": 1,
        "pooled": {
            "steps": pooled_steps,
            "candidates": pooled_candidates,
            "candidate_plus_cooldown": pooled_cooldown,
            "required_reuse_actions": pooled_required,
            "reuse_ceiling": pooled_cooldown / pooled_steps,
            "projected_latency_reduction": pooled_latency,
            "current_reuse_pass": pooled_cooldown >= pooled_required,
            "current_latency_pass": pooled_latency >= LATENCY_FLOOR,
        },
        "tasks": task_rows,
        "bank_independent_blockers": bank_independent_blockers,
        "bank_independent_under_frozen_precheck_viable": (
            not bank_independent_blockers
        ),
    }
    _assert_finite(result, "structural")
    return result


def build_result(
    prior: dict[str, Any],
    prior_sha256: str,
    datasets: list[dict[str, Any]],
    *,
    generated_at: str | None = None,
) -> dict[str, Any]:
    structural = structural_preflight(prior)
    contact_observable = any(
        row["authoritative_contact_features"]
        and row["contact_provenance_validated"]
        and row["aligned_with_frozen_source"]
        for row in datasets
    )
    auxiliary_phase_feature = any(
        row["semantic_phase_features"] for row in datasets
    )
    frozen_source_semantic_phase = any(
        row["semantic_phase_features"] and row["aligned_with_frozen_source"]
        for row in datasets
    )
    blockers = []
    if not contact_observable:
        blockers.append("source_contact_unobservable")
    if not frozen_source_semantic_phase:
        blockers.append("frozen_source_semantic_phase_unobservable")
    blockers.extend(structural["bank_independent_blockers"])
    progression_allowed = not blockers
    result = {
        "schema": 1,
        "experiment": EXPERIMENT,
        "generated_at": generated_at
        or datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "classification": "Research-only",
        "preflight_tool_sha256": _tool_sha256(),
        "dataset_contract_sha256": _dataset_contract_sha256(),
        "status": "pass",
        "diagnostic_kind": "trajectory_window_preflight",
        "completion_reason": (
            "pre_vla_admitted" if progression_allowed else "pre_vla_structural_abort"
        ),
        "finding": "admitted" if progression_allowed else "structural_abort",
        "prior": {
            "experiment": 34,
            "compact_sha256": prior_sha256,
            "manifest_semantic_sha256": prior["manifest_semantic_sha256"],
            "candidate_detail_sha256": prior["detail"]["sha256"],
            "steps": prior["scope"]["steps"],
            "task_map": prior["scope"]["task_map"],
        },
        "datasets": datasets,
        "source_observability": {
            "authoritative_contact": contact_observable,
            "auxiliary_semantic_subtask_feature": auxiliary_phase_feature,
            "frozen_source_semantic_phase": frozen_source_semantic_phase,
            "contact_free_label_eligible": contact_observable,
            "proxy_state_action_contact_label_allowed": False,
            "historical_simulator_replay_mapping_provided": False,
            "historical_simulator_replay_mapping_validated": False,
        },
        "structural": structural,
        "frozen_precheck_policy": {
            "source": "experiment_034_recorded_precheck_and_candidate_mask",
            "recompute_from_treatment_bank": False,
            "control_normalization_and_gripper_boundary_required_for_paired_stage": True,
        },
        "progression": {
            "allowed": progression_allowed,
            "blockers": blockers,
            "source_window_extraction_started": False,
            "paired_bank_comparison_started": False,
            "separability_comparison_started": False,
            "threshold_grid_started": False,
        },
        "execution": {
            "source_rows_downloaded": 0,
            "source_images_downloaded": 0,
            "models_loaded": 0,
            "vla_calls": 0,
            "retrieved_actions": 0,
            "aws_resources_created": 0,
        },
        "acceptance": {
            "prior_evidence_valid": True,
            "dataset_revisions_pinned": True,
            "dataset_feature_name_sets_and_counts_valid": True,
            "contact_provenance_available": contact_observable,
            "frozen_source_semantic_phase_available": frozen_source_semantic_phase,
            "bank_independent_under_frozen_precheck_viable": structural[
                "bank_independent_under_frozen_precheck_viable"
            ],
            "downstream_stages_blocked_after_failure": not progression_allowed,
            "zero_vla_calls": True,
            "zero_retrieved_actions": True,
            "zero_cloud_resources": True,
            "diagnostic_valid": True,
        },
    }
    _assert_finite(result)
    return result


def _percent(value: float) -> str:
    return f"{100.0 * value:.2f}%"


def render_report(result: dict[str, Any]) -> str:
    structural = result["structural"]
    lines = [
        "# Physical AI VLA Trajectory-Window Preflight 035 Results",
        "",
        f"Generated at: {result['generated_at']}",
        "Classification: Research-only",
        "Status: pass - diagnostic-valid structural abort",
        "",
        "## Result",
        "",
        "Experiment 035 stopped before source-window extraction, model loading,",
        "EKS, or VLA execution. The pinned frozen source exposes neither",
        "authoritative historical contact nor semantic phase, and the suite-task-0",
        "candidate-plus-cooldown ceiling is bank-independent under the frozen",
        "Experiment 034 precheck mask at 65/377",
        "(17.24%), below the required 76/377 (20%).",
        "",
        "This is a valid negative progression result. It is not a paired bank",
        "comparison and provides no action-correctness, risk-free, or safety evidence.",
        "",
        "## Source Observability",
        "",
        "| Dataset | Episodes | Frozen-source aligned | Contact feature | Semantic subtask feature |",
        "| --- | ---: | --- | --- | --- |",
    ]
    for dataset in result["datasets"]:
        contact = ", ".join(dataset["authoritative_contact_features"]) or "none"
        phase = ", ".join(dataset["semantic_phase_features"]) or "none"
        lines.append(
            f"| `{dataset['repo']}`@`{dataset['revision']}` | "
            f"{dataset['total_episodes']} | "
            f"{'yes' if dataset['aligned_with_frozen_source'] else 'no'} | "
            f"{contact} | {phase} |"
        )
    lines.extend(
        [
            "",
            "The separate 500-episode auxiliary corpus exposes `subtask_index`, but",
            "it is not aligned to the frozen 379-episode source bank. The validated",
            "frozen-source top-level feature-name set exposes no contact, object state,",
            "simulator state, or semantic subtask. No historical simulator replay",
            "mapping was provided or validated by this run. State/action rules",
            "therefore remain kinematic",
            "proxies and were not promoted to `contact-free` or semantic labels.",
            "",
            "## Frozen Structural Preflight",
            "",
            "| Scope | Steps | Eligible | Missing memory | Candidates | After cooldown | Required | Reuse ceiling | Candidate latency | Bank-independent under frozen precheck |",
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
        ]
    )
    pooled = structural["pooled"]
    lines.append(
        "| pooled | "
        f"{pooled['steps']} | - | - | {pooled['candidates']} | "
        f"{pooled['candidate_plus_cooldown']} | {pooled['required_reuse_actions']} | "
        f"{_percent(pooled['reuse_ceiling'])} | "
        f"{_percent(pooled['projected_latency_reduction'])} | no |"
    )
    for row in structural["tasks"]:
        lines.append(
            f"| suite task {row['suite_task_id']} / manifest {row['manifest_task_index']} | "
            f"{row['steps']} | {row['precheck_eligible']} | "
            f"{row['missing_executable_memory']} | {row['candidates']} | "
            f"{row['candidate_plus_cooldown']} | {row['required_reuse_actions']} | "
            f"{_percent(row['reuse_ceiling'])} | "
            f"{_percent(row['projected_latency_reduction'])} | "
            f"{'yes' if row['bank_independent_under_frozen_precheck'] else 'no'} |"
        )
    lines.extend(
        [
            "",
            "Suite task 0 already converted every precheck-eligible observation into",
            "a candidate and had zero missing-memory observations. A source-bank",
            "change cannot add candidates to that frozen trace without changing the",
            "precheck, cooldown, workload, or policy scope. Those changes were outside",
            "the pre-registration.",
            "",
            "Manifest task 9's 58 missing open-hold opportunities may be repairable,",
            "but they cannot repair the independent per-task failure on suite task 0.",
            "",
            "## Diagnostic Acceptance",
            "",
            "| Check | Status |",
            "| --- | --- |",
        ]
    )
    for name, passed in result["acceptance"].items():
        lines.append(f"| {name.replace('_', ' ')} | {'pass' if passed else 'not met'} |")
    lines.extend(
        [
            "",
            "`contact_provenance_available`,",
            "`frozen_source_semantic_phase_available`, and",
            "`bank_independent_under_frozen_precheck_viable` are scientific",
            "progression gates; their expected failure does not invalidate the",
            "preflight accounting.",
            "",
            "## Execution Accounting",
            "",
            "- Source rows/images downloaded: 0 / 0.",
            "- Models loaded and VLA calls: 0 / 0.",
            "- Retrieved actions: 0.",
            "- Threshold grids and paired-bank comparisons: not started.",
            "- AWS/EKS resources created: 0; cleanup was not required.",
            "",
            "## Interpretation",
            "",
            "Do not build the proposed trajectory bank or rerun the same EKS trace.",
            "The next useful test is a separately pre-registered, multi-seed shadow",
            "experiment that uses pinned direct-VLA actions to measure whether query",
            "contact eligibility and cooldown placement remain below the target on",
            "new frozen traces before any bank encoding or retrieval. Provenance-",
            "preserving contact plus source-aligned semantic labels, or a raw-demo",
            "simulator replay mapping that reconstructs both, is still required before",
            "historical windows can be called contact-free and phase-local.",
            "",
        ]
    )
    return "\n".join(lines)


def _stage_payload(path: Path, payload: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp")
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    return temporary


def _write_no_clobber(path: Path, payload: bytes) -> None:
    if path.exists():
        raise FileExistsError(f"refusing to overwrite {path}")
    temporary = _stage_payload(path, payload)
    try:
        os.link(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _write_pair_no_clobber(
    result_path: Path,
    result_payload: bytes,
    report_path: Path,
    report_payload: bytes,
) -> None:
    if result_path.resolve(strict=False) == report_path.resolve(strict=False):
        raise ValueError("result and report paths must be distinct")
    result_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    for path in (result_path, report_path):
        if path.exists():
            raise FileExistsError(f"refusing to overwrite {path}")
    result_temporary = _stage_payload(result_path, result_payload)
    try:
        report_temporary = _stage_payload(report_path, report_payload)
    except BaseException:
        result_temporary.unlink(missing_ok=True)
        raise
    try:
        os.link(result_temporary, result_path)
        os.link(report_temporary, report_path)
    except BaseException:
        for path, temporary in (
            (report_path, report_temporary),
            (result_path, result_temporary),
        ):
            try:
                if path.exists() and os.path.samefile(path, temporary):
                    path.unlink()
            except OSError:
                pass
        raise
    finally:
        result_temporary.unlink(missing_ok=True)
        report_temporary.unlink(missing_ok=True)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--prior-result", type=Path, default=DEFAULT_PRIOR_RESULT
    )
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        prior, prior_sha256 = validate_prior_result(args.prior_result)
        datasets = inspect_datasets()
        result = build_result(prior, prior_sha256, datasets)
        encoded = _canonical_json(result) + b"\n"
        _write_pair_no_clobber(
            args.result,
            encoded,
            args.report,
            render_report(result).encode("utf-8"),
        )
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"Experiment 035 preflight failed: {exc}", file=sys.stderr)
        return 1
    print(
        "Experiment 035 diagnostic passed and stopped before VLA/EKS: "
        + ", ".join(result["progression"]["blockers"])
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
