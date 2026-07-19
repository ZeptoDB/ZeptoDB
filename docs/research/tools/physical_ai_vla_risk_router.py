#!/usr/bin/env python3
"""Run research-only Experiment 031 with risk-partitioned free-space reuse."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import statistics
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import physical_ai_vla_closed_loop as exp27  # noqa: E402
import physical_ai_vla_early_exit as exp26  # noqa: E402
import physical_ai_vla_safety_gate as exp30  # noqa: E402
import physical_ai_vla_skip_region as exp28  # noqa: E402


EXPERIMENT = 31
SUITE = exp27.SUITE
VLA_MODEL = exp27.VLA_MODEL
VLA_MODEL_SHA = exp27.VLA_MODEL_SHA
SIGLIP_MODEL = exp27.SIGLIP_MODEL
SIGLIP_MODEL_SHA = "7fd15f0689c79d79e38b1c2e2e2370a7bf2761ed"
AGENT_ID = "physical-ai-vla-risk-router"
EXECUTABLE_TYPE = "bounded_free_space_candidate"
SUPPRESSION_TYPE = "non_executable_action"
DIAGNOSTIC_SCALE = 1_000_000
DIAGNOSTIC_THRESHOLD_SCALE = 1_000
DIAGNOSTIC_GATE_NAMES = (
    "reuse_below_min",
    "reuse_above_max",
    "mean_action_mae",
    "p95_action_mae",
    "projected_latency_reduction",
)
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
ROUTE_REASON_NAMES = (
    "accepted",
    "no_candidate",
    "confidence",
    "margin",
    "candidate_gripper_transition",
    "candidate_translation",
    "candidate_rotation",
    "candidate_disagreement",
    "candidate_state_outlier",
    "negative_veto",
    "cooldown",
)
MEMORY_ADMISSION_REASON_NAMES = (
    "open_hold",
    "closed_hold",
    "gripper_ambiguous",
    "gripper_command_mismatch",
    "translation_limit",
    "rotation_limit",
)
MEMORY_ADMISSION_MASK_NAMES = (
    "gripper_ambiguous",
    "gripper_command_mismatch",
    "translation_limit",
    "rotation_limit",
)
VETO_GROUP_NAMES = (
    "no_negative_support",
    "separated",
    "vetoed",
)
QUERY_PHASE_NAMES = ("early", "middle", "late")
HOLD_CLASS_NAMES = ("open_hold", "closed_hold", "ambiguous")
CANDIDATE_SAFETY_REASON_NAMES = (
    "safe",
    "candidate_gripper_transition",
    "candidate_translation",
    "candidate_rotation",
    "candidate_disagreement",
    "candidate_state_outlier",
)
VETO_COUNTERFACTUAL_MARGINS = (0.0, 0.005, 0.01, 0.02)
EXPERIMENT_031_REFERENCE_STEPS = 595
EXPERIMENT_031_REFERENCE_PRECHECK = [127, 0, 390, 2, 12, 0, 6, 58]
EXPERIMENT_031_REFERENCE_AVAILABILITY = {
    0: {"open_hold": 13, "closed_hold": 1, "suppression": 5},
    5: {"open_hold": 9, "closed_hold": 0, "suppression": 10},
}
EXPERIMENT_031_SEMANTIC_MANIFEST_SHA256 = (
    "0624630ce232f33c36dbe20159ce3e88729ab0feffb6b67905f4bd2b180e85ba"
)
TASK_MAPPING_CORRECTION_EXPERIMENT = 34
TASK_MAPPING_CORRECTION_EXPECTED_MAP = (
    (
        0,
        5,
        "put both the alphabet soup and the tomato sauce in the basket",
    ),
    (
        5,
        9,
        "pick up the book and place it in the back compartment of the caddy",
    ),
)
TASK_MAPPING_CORRECTION_EXPECTED_AVAILABILITY = {
    5: {"open_hold": 9, "closed_hold": 0, "suppression": 10},
    9: {"open_hold": 0, "closed_hold": 8, "suppression": 11},
}
EXPERIMENT_033_CANDIDATE_ROWS_SHA256 = (
    "466467ef024f62bc815069dfa849838cd3a2ec3408319c022fa88a85b6f4552a"
)


def _harness_bundle_sha256(extra_paths: list[Path] | None = None) -> str:
    digest = hashlib.sha256()
    paths = sorted(
        {
            Path(__file__).resolve(),
            Path(exp26.__file__).resolve(),
            Path(exp27.__file__).resolve(),
            Path(exp28.__file__).resolve(),
            Path(exp30.__file__).resolve(),
            *(path.resolve() for path in (extra_paths or [])),
        },
        key=lambda path: path.name,
    )
    for path in paths:
        digest.update(path.name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def _semantic_manifest_sha256(path: Path) -> str:
    manifest = json.loads(path.read_text())
    identity_fields = (
        "episode_index",
        "task_index",
        "task",
        "row_index",
        "state",
        "action",
    )

    def semantic_rows(name: str) -> list[dict[str, Any]]:
        return [
            {field: row[field] for field in identity_fields}
            for row in manifest[name]
        ]

    semantic = {
        "dataset": manifest["dataset"],
        "dataset_revision": manifest["dataset_revision"],
        "seed": manifest["seed"],
        "memories": semantic_rows("memories"),
        "queries": semantic_rows("queries"),
    }
    encoded = json.dumps(
        semantic, allow_nan=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _tenant_id(experiment_id: int, run_id: str) -> str:
    if experiment_id <= 0 or not run_id:
        raise ValueError("experiment identity is invalid")
    return f"physical-ai-{experiment_id:03d}-{run_id}"


def _route_group(row: dict[str, Any]) -> tuple[int, Any]:
    return int(row["task_id"]), row.get("episode_id")


def _count_vector(counts: dict[str, int], names: tuple[str, ...]) -> list[int]:
    unknown = set(counts) - set(names)
    if unknown:
        raise ValueError(f"unknown diagnostic count names: {sorted(unknown)}")
    return [int(counts.get(name, 0)) for name in names]


def _norm(values: list[float]) -> float:
    return math.sqrt(sum(float(value) ** 2 for value in values))


def gripper_width(state: list[float]) -> float:
    if len(state) != 8 or not all(math.isfinite(float(value)) for value in state):
        raise ValueError("state must contain eight finite values")
    return float(state[6]) - float(state[7])


def classify_memory(
    *,
    state: list[float],
    action: list[float],
    gripper_boundary: float,
    gripper_guard_band: float,
    translation_limit: float,
    rotation_limit: float,
) -> str:
    memory_class, _ = classify_memory_with_reason(
        state=state,
        action=action,
        gripper_boundary=gripper_boundary,
        gripper_guard_band=gripper_guard_band,
        translation_limit=translation_limit,
        rotation_limit=rotation_limit,
    )
    return memory_class


def classify_memory_with_reason(
    *,
    state: list[float],
    action: list[float],
    gripper_boundary: float,
    gripper_guard_band: float,
    translation_limit: float,
    rotation_limit: float,
) -> tuple[str, str]:
    if len(action) != 7 or not all(math.isfinite(float(value)) for value in action):
        raise ValueError("action must contain seven finite values")
    if (
        not math.isfinite(gripper_boundary)
        or gripper_guard_band < 0.0
        or translation_limit <= 0.0
        or rotation_limit <= 0.0
    ):
        raise ValueError("memory admission limits are invalid")
    width = gripper_width(state)
    if abs(width - gripper_boundary) <= gripper_guard_band:
        return "suppression", "gripper_ambiguous"
    expected_command = -1 if width > gripper_boundary else 1
    command = -1 if float(action[6]) < 0.0 else 1
    if command != expected_command:
        return "suppression", "gripper_command_mismatch"
    if _norm(list(map(float, action[:3]))) > translation_limit:
        return "suppression", "translation_limit"
    if _norm(list(map(float, action[3:6]))) > rotation_limit:
        return "suppression", "rotation_limit"
    memory_class = "open_hold" if command == -1 else "closed_hold"
    return memory_class, memory_class


def memory_admission_mask(
    *,
    state: list[float],
    action: list[float],
    gripper_boundary: float,
    gripper_guard_band: float,
    translation_limit: float,
    rotation_limit: float,
) -> int:
    classify_memory_with_reason(
        state=state,
        action=action,
        gripper_boundary=gripper_boundary,
        gripper_guard_band=gripper_guard_band,
        translation_limit=translation_limit,
        rotation_limit=rotation_limit,
    )
    width = gripper_width(state)
    expected_command = -1 if width > gripper_boundary else 1
    command = -1 if float(action[6]) < 0.0 else 1
    checks = (
        abs(width - gripper_boundary) <= gripper_guard_band,
        command != expected_command,
        _norm(list(map(float, action[:3]))) > translation_limit,
        _norm(list(map(float, action[3:6]))) > rotation_limit,
    )
    mask = 0
    for index, failed in enumerate(checks):
        if failed:
            mask |= 1 << index
    return mask


def classify_contact_names(pairs: list[tuple[str, str]]) -> dict[str, int]:
    robot_tokens = ("robot0", "gripper", "finger", "hand", "wrist")
    finger_tokens = ("gripper", "finger")
    counts = {"total": 0, "finger": 0, "arm": 0}
    for first, second in pairs:
        first_lower = first.lower()
        second_lower = second.lower()
        first_robot = any(token in first_lower for token in robot_tokens)
        second_robot = any(token in second_lower for token in robot_tokens)
        if not first_robot and not second_robot:
            continue
        counts["total"] += 1
        robot_name = first_lower if first_robot else second_lower
        if first_robot and second_robot:
            counts["arm"] += 1
        elif any(token in robot_name for token in finger_tokens):
            counts["finger"] += 1
        else:
            counts["arm"] += 1
    return counts


def max_state_z(
    state: list[float], state_means: list[float], state_stds: list[float]
) -> float:
    if len(state) != 8 or len(state_means) != 8 or len(state_stds) != 8:
        raise ValueError("state statistics must have eight dimensions")
    if any(float(std) <= 0.0 for std in state_stds):
        raise ValueError("state standard deviations must be positive")
    return max(
        abs((float(value) - float(mean)) / float(std))
        for value, mean, std in zip(state, state_means, state_stds)
    )


def free_space_precheck(
    *,
    state: list[float],
    previous_width: float | None,
    contacts: dict[str, int],
    available: dict[str, int],
    gripper_boundary: float,
    gripper_guard_band: float,
    max_gripper_delta: float,
    state_z: float,
    state_z_limit: float,
) -> tuple[str | None, str]:
    if max_gripper_delta <= 0.0 or state_z_limit <= 0.0:
        raise ValueError("precheck limits must be positive")
    if set(contacts) != {"total", "finger", "arm"}:
        raise ValueError("contact counts are incomplete")
    if any(int(value) < 0 for value in contacts.values()):
        raise ValueError("contact counts must be non-negative")
    if contacts["arm"]:
        return None, "arm_contact"
    if contacts["finger"] or contacts["total"]:
        return None, "finger_contact"
    width = gripper_width(state)
    if previous_width is None:
        return None, "first_observation"
    if abs(width - previous_width) > max_gripper_delta:
        return None, "gripper_motion"
    if abs(width - gripper_boundary) <= gripper_guard_band:
        return None, "gripper_ambiguous"
    if state_z > state_z_limit:
        return None, "state_outlier"
    hold_class = "open_hold" if width > gripper_boundary else "closed_hold"
    if int(available.get(hold_class, 0)) <= 0:
        return None, "no_executable_memory"
    return hold_class, "eligible"


def candidate_safety(
    *,
    action: list[float],
    state: list[float],
    hold_class: str,
    disagreement: float,
    state_means: list[float],
    state_stds: list[float],
    translation_limit: float,
    rotation_limit: float,
    disagreement_limit: float,
    state_z_limit: float,
) -> tuple[bool, str]:
    if len(action) != 7 or hold_class not in {"open_hold", "closed_hold"}:
        raise ValueError("candidate action or hold class is invalid")
    limits = (
        translation_limit,
        rotation_limit,
        disagreement_limit,
        state_z_limit,
    )
    if any(float(value) <= 0.0 for value in limits):
        raise ValueError("candidate safety limits must be positive")
    expected_command = -1 if hold_class == "open_hold" else 1
    command = -1 if float(action[6]) < 0.0 else 1
    if command != expected_command:
        return False, "candidate_gripper_transition"
    if _norm(list(map(float, action[:3]))) > translation_limit:
        return False, "candidate_translation"
    if _norm(list(map(float, action[3:6]))) > rotation_limit:
        return False, "candidate_rotation"
    if float(disagreement) > disagreement_limit:
        return False, "candidate_disagreement"
    if max_state_z(state, state_means, state_stds) > state_z_limit:
        return False, "candidate_state_outlier"
    return True, "safe"


def route_candidate(
    candidate: dict[str, Any] | None,
    *,
    threshold: float,
    min_margin: float,
    veto_margin: float,
    previous_reuse: bool,
) -> tuple[bool, str]:
    if veto_margin < 0.0:
        raise ValueError("veto margin must be non-negative")
    if candidate is None:
        return False, "no_candidate"
    if float(candidate["confidence"]) < threshold:
        return False, "confidence"
    if float(candidate["margin"]) < min_margin:
        return False, "margin"
    if not bool(candidate["safe"]):
        return False, str(candidate["safety_reason"])
    if veto_group(candidate, veto_margin) == "vetoed":
        return False, "negative_veto"
    if previous_reuse:
        return False, "cooldown"
    return True, "accepted"


def simulate_route_outcomes(
    rows: list[dict[str, Any]],
    *,
    threshold: float,
    min_margin: float,
    veto_margin: float,
) -> tuple[list[bool], list[str]]:
    mask: list[bool] = []
    reasons: list[str] = []
    current_group: tuple[int, Any] | None = None
    previous_reuse = False
    for row in rows:
        group = _route_group(row)
        if group != current_group:
            current_group = group
            previous_reuse = False
        should_reuse, reason = route_candidate(
            row.get("candidate"),
            threshold=threshold,
            min_margin=min_margin,
            veto_margin=veto_margin,
            previous_reuse=previous_reuse,
        )
        mask.append(should_reuse)
        reasons.append(reason)
        previous_reuse = should_reuse
    return mask, reasons


def simulate_reuse_mask(
    rows: list[dict[str, Any]],
    *,
    threshold: float,
    min_margin: float,
    veto_margin: float,
) -> list[bool]:
    mask, _ = simulate_route_outcomes(
        rows,
        threshold=threshold,
        min_margin=min_margin,
        veto_margin=veto_margin,
    )
    return mask


def evaluate_region(
    rows: list[dict[str, Any]],
    *,
    threshold: float,
    min_margin: float,
    veto_margin: float,
) -> dict[str, Any]:
    if not rows:
        raise ValueError("shadow rows must not be empty")
    mask, reasons = simulate_route_outcomes(
        rows,
        threshold=threshold,
        min_margin=min_margin,
        veto_margin=veto_margin,
    )
    errors = [
        float(row["candidate_mae"])
        for row, reuse in zip(rows, mask)
        if reuse and row.get("candidate_mae") is not None
    ]
    if len(errors) != sum(mask):
        raise ValueError("reused shadow rows must contain candidate action errors")
    baseline = statistics.fmean(float(row["policy_wall"]) for row in rows)
    projected = statistics.fmean(
        float(row["memory_wall"]) + (0.0 if reuse else float(row["policy_wall"]))
        for row, reuse in zip(rows, mask)
    )
    reason_counts: dict[str, int] = {}
    task_counts: dict[int, dict[str, Any]] = {}
    hold_counts: dict[tuple[int, str], dict[str, Any]] = {}
    for row, reuse, reason in zip(rows, mask, reasons):
        reason_counts[reason] = reason_counts.get(reason, 0) + 1
        task_id = int(row["task_id"])
        task = task_counts.setdefault(
            task_id,
            {
                "steps": 0,
                "candidates": 0,
                "reuses": 0,
                "errors": [],
                "baseline": 0.0,
                "projected": 0.0,
            },
        )
        task["steps"] += 1
        task["candidates"] += int(row.get("candidate") is not None)
        task["reuses"] += int(reuse)
        task["baseline"] += float(row["policy_wall"])
        task["projected"] += float(row["memory_wall"]) + (
            0.0 if reuse else float(row["policy_wall"])
        )
        if reuse:
            task["errors"].append(float(row["candidate_mae"]))
        hold_class = row.get("hold_class")
        if hold_class is not None:
            hold = hold_counts.setdefault(
                (task_id, str(hold_class)),
                {"candidates": 0, "reuses": 0, "errors": []},
            )
            hold["candidates"] += 1
            hold["reuses"] += int(reuse)
            if reuse:
                hold["errors"].append(float(row["candidate_mae"]))
    return {
        "threshold": threshold,
        "min_margin": min_margin,
        "reuses": sum(mask),
        "reuse_rate": sum(mask) / len(mask),
        "mean_action_mae": statistics.fmean(errors) if errors else math.inf,
        "p95_action_mae": exp27.percentile(errors, 0.95) if errors else math.inf,
        "projected_latency_reduction": 1.0 - projected / baseline,
        "route_reasons": dict(sorted(reason_counts.items())),
        "tasks": [
            {
                "task_id": task_id,
                "steps": counts["steps"],
                "candidates": counts["candidates"],
                "reuses": counts["reuses"],
                "mean_action_mae": (
                    statistics.fmean(counts["errors"])
                    if counts["errors"]
                    else math.inf
                ),
                "p95_action_mae": (
                    exp27.percentile(counts["errors"], 0.95)
                    if counts["errors"]
                    else math.inf
                ),
                "projected_latency_reduction": (
                    1.0 - counts["projected"] / counts["baseline"]
                ),
            }
            for task_id, counts in sorted(task_counts.items())
        ],
        "holds": [
            {
                "task_id": task_id,
                "hold_class": hold_class,
                "candidates": counts["candidates"],
                "reuses": counts["reuses"],
                "mean_action_mae": (
                    statistics.fmean(counts["errors"])
                    if counts["errors"]
                    else math.inf
                ),
                "p95_action_mae": (
                    exp27.percentile(counts["errors"], 0.95)
                    if counts["errors"]
                    else math.inf
                ),
            }
            for (task_id, hold_class), counts in sorted(hold_counts.items())
        ],
    }


def _region_gate_mask(
    row: dict[str, Any],
    *,
    target_reuse_min: float,
    target_reuse_max: float,
    max_mean_action_mae: float,
    max_p95_action_mae: float,
    min_projected_latency_reduction: float,
) -> int:
    failures = 0
    checks = (
        float(row["reuse_rate"]) < target_reuse_min,
        float(row["reuse_rate"]) > target_reuse_max,
        float(row["mean_action_mae"]) > max_mean_action_mae,
        float(row["p95_action_mae"]) > max_p95_action_mae,
        float(row["projected_latency_reduction"])
        < min_projected_latency_reduction,
    )
    for index, failed in enumerate(checks):
        if failed:
            failures |= 1 << index
    return failures


def _scaled_metric(value: float) -> int | None:
    return (
        int(round(float(value) * DIAGNOSTIC_SCALE))
        if math.isfinite(float(value))
        else None
    )


def _public_region(row: dict[str, Any]) -> dict[str, float | int]:
    return {
        name: row[name]
        for name in (
            "threshold",
            "min_margin",
            "reuses",
            "reuse_rate",
            "mean_action_mae",
            "p95_action_mae",
            "projected_latency_reduction",
        )
    }


def _region_violation_key(
    row: dict[str, Any],
    *,
    failure_mask: int,
    target_reuse_min: float,
    target_reuse_max: float,
    max_mean_action_mae: float,
    max_p95_action_mae: float,
    min_projected_latency_reduction: float,
) -> tuple[Any, ...]:
    reuse_rate = float(row["reuse_rate"])
    mean_mae = float(row["mean_action_mae"])
    p95_mae = float(row["p95_action_mae"])
    latency = float(row["projected_latency_reduction"])
    violations = (
        max(0.0, target_reuse_min - reuse_rate) / max(target_reuse_min, 1e-12),
        max(0.0, reuse_rate - target_reuse_max) / max(target_reuse_max, 1e-12),
        max(0.0, mean_mae - max_mean_action_mae)
        / max(max_mean_action_mae, 1e-12),
        max(0.0, p95_mae - max_p95_action_mae)
        / max(max_p95_action_mae, 1e-12),
        max(0.0, min_projected_latency_reduction - latency)
        / max(abs(min_projected_latency_reduction), 1e-12),
    )
    return (
        bin(failure_mask).count("1"),
        max(violations),
        sum(violations),
        abs(reuse_rate - (target_reuse_min + target_reuse_max) / 2.0),
        p95_mae,
        -latency,
        float(row["threshold"]),
        float(row["min_margin"]),
    )


def veto_group(candidate: dict[str, Any], veto_margin: float) -> str:
    if not math.isfinite(float(veto_margin)) or veto_margin < 0.0:
        raise ValueError("veto margin must be finite and non-negative")
    positive_similarity = float(candidate["positive_similarity"])
    if not math.isfinite(positive_similarity):
        raise ValueError("positive similarity must be finite")
    negative_similarity = candidate.get("negative_similarity")
    if negative_similarity is None:
        return "no_negative_support"
    negative_similarity = float(negative_similarity)
    if not math.isfinite(negative_similarity):
        raise ValueError("negative similarity must be finite when present")
    if negative_similarity >= positive_similarity - veto_margin:
        return "vetoed"
    return "separated"


def _negative_veto(candidate: dict[str, Any], veto_margin: float) -> bool:
    return veto_group(candidate, veto_margin) == "vetoed"


def _cooldown_mask(
    rows: list[dict[str, Any]], predicate: Any
) -> list[bool]:
    mask: list[bool] = []
    current_group: tuple[int, Any] | None = None
    previous_reuse = False
    for row in rows:
        group = _route_group(row)
        if group != current_group:
            current_group = group
            previous_reuse = False
        reuse = bool(predicate(row)) and not previous_reuse
        mask.append(reuse)
        previous_reuse = reuse
    return mask


def _projected_latency_reduction(
    rows: list[dict[str, Any]], mask: list[bool]
) -> float:
    if len(rows) != len(mask) or not rows:
        raise ValueError("latency projection requires aligned non-empty rows")
    baseline = statistics.fmean(float(row["policy_wall"]) for row in rows)
    if baseline <= 0.0:
        raise ValueError("baseline policy latency must be positive")
    projected = statistics.fmean(
        float(row["memory_wall"]) + (0.0 if reuse else float(row["policy_wall"]))
        for row, reuse in zip(rows, mask)
    )
    return 1.0 - projected / baseline


def _candidate_diagnostics(
    candidates: list[dict[str, Any]],
    *,
    rows: list[dict[str, Any]],
    veto_margin: float,
    target_reuse_min: float,
    target_reuse_max: float,
    max_mean_action_mae: float,
    max_p95_action_mae: float,
    min_projected_latency_reduction: float,
) -> dict[str, Any]:
    if not candidates:
        raise ValueError("candidate diagnostics require at least one region")
    failure_masks = [
        _region_gate_mask(
            row,
            target_reuse_min=target_reuse_min,
            target_reuse_max=target_reuse_max,
            max_mean_action_mae=max_mean_action_mae,
            max_p95_action_mae=max_p95_action_mae,
            min_projected_latency_reduction=min_projected_latency_reduction,
        )
        for row in candidates
    ]
    closest_index = min(
        range(len(candidates)),
        key=lambda index: _region_violation_key(
            candidates[index],
            failure_mask=failure_masks[index],
            target_reuse_min=target_reuse_min,
            target_reuse_max=target_reuse_max,
            max_mean_action_mae=max_mean_action_mae,
            max_p95_action_mae=max_p95_action_mae,
            min_projected_latency_reduction=min_projected_latency_reduction,
        ),
    )
    closest = candidates[closest_index]
    gate_counts = [
        sum(bool(mask & (1 << index)) for mask in failure_masks)
        for index in range(len(DIAGNOSTIC_GATE_NAMES))
    ]
    signature_counts: dict[str, int] = {}
    for mask in failure_masks:
        key = str(mask)
        signature_counts[key] = signature_counts.get(key, 0) + 1
    candidate_count = sum(row.get("candidate") is not None for row in rows)
    candidate_mask = _cooldown_mask(
        rows, lambda row: row.get("candidate") is not None
    )
    safety_mask = _cooldown_mask(
        rows,
        lambda row: row.get("candidate") is not None
        and bool(row["candidate"]["safe"]),
    )
    veto_mask = _cooldown_mask(
        rows,
        lambda row: row.get("candidate") is not None
        and not _negative_veto(row["candidate"], veto_margin),
    )
    fixed_pre_cooldown = sum(
        row.get("candidate") is not None
        and bool(row["candidate"]["safe"])
        and not _negative_veto(row["candidate"], veto_margin)
        for row in rows
    )
    fixed_mask = _cooldown_mask(
        rows,
        lambda row: row.get("candidate") is not None
        and bool(row["candidate"]["safe"])
        and not _negative_veto(row["candidate"], veto_margin),
    )
    unsafe_count = sum(
        row.get("candidate") is not None and not bool(row["candidate"]["safe"])
        for row in rows
    )
    negative_veto_count = sum(
        row.get("candidate") is not None
        and _negative_veto(row["candidate"], veto_margin)
        for row in rows
    )
    return {
        "schema": 2,
        "metric_scale": DIAGNOSTIC_SCALE,
        "parameter_scale": DIAGNOSTIC_THRESHOLD_SCALE,
        "veto_margin": _scaled_metric(veto_margin),
        "limits": [
            _scaled_metric(target_reuse_min),
            _scaled_metric(target_reuse_max),
            _scaled_metric(max_mean_action_mae),
            _scaled_metric(max_p95_action_mae),
            _scaled_metric(min_projected_latency_reduction),
        ],
        "grid": [
            [
                int(round(float(row["threshold"]) * DIAGNOSTIC_THRESHOLD_SCALE)),
                int(round(float(row["min_margin"]) * DIAGNOSTIC_THRESHOLD_SCALE)),
                int(row["reuses"]),
                _scaled_metric(float(row["mean_action_mae"])),
                _scaled_metric(float(row["p95_action_mae"])),
                _scaled_metric(float(row["projected_latency_reduction"])),
                failure_mask,
            ]
            for row, failure_mask in zip(candidates, failure_masks)
        ],
        "gate_counts": gate_counts,
        "signatures": signature_counts,
        "no_reuse_points": sum(int(row["reuses"]) == 0 for row in candidates),
        "ceilings": [
            len(rows),
            candidate_count,
            sum(candidate_mask),
            sum(safety_mask),
            sum(veto_mask),
            fixed_pre_cooldown,
            sum(fixed_mask),
            unsafe_count,
            negative_veto_count,
            _scaled_metric(_projected_latency_reduction(rows, candidate_mask)),
            _scaled_metric(_projected_latency_reduction(rows, fixed_mask)),
        ],
        "closest_index": closest_index,
        "closest_rejections": _count_vector(
            closest["route_reasons"], ROUTE_REASON_NAMES
        ),
        "closest_tasks": [
            [
                int(row["task_id"]),
                int(row["steps"]),
                int(row["candidates"]),
                int(row["reuses"]),
                _scaled_metric(float(row["mean_action_mae"])),
                _scaled_metric(float(row["p95_action_mae"])),
                _scaled_metric(float(row["projected_latency_reduction"])),
            ]
            for row in closest["tasks"]
        ],
        "closest_holds": [
            [
                int(row["task_id"]),
                0 if row["hold_class"] == "open_hold" else 1,
                int(row["candidates"]),
                int(row["reuses"]),
                _scaled_metric(float(row["mean_action_mae"])),
                _scaled_metric(float(row["p95_action_mae"])),
            ]
            for row in closest["holds"]
        ],
    }


def select_region(
    rows: list[dict[str, Any]],
    *,
    threshold_min: float,
    threshold_max: float,
    threshold_step: float,
    margin_candidates: list[float],
    veto_margin: float,
    target_reuse_min: float,
    target_reuse_max: float,
    max_mean_action_mae: float,
    max_p95_action_mae: float,
    min_projected_latency_reduction: float,
) -> dict[str, Any]:
    numeric_limits = (
        threshold_min,
        threshold_max,
        threshold_step,
        veto_margin,
        target_reuse_min,
        target_reuse_max,
        max_mean_action_mae,
        max_p95_action_mae,
        min_projected_latency_reduction,
    )
    if not all(math.isfinite(float(value)) for value in numeric_limits):
        raise ValueError("region search parameters must be finite")
    if not 0.0 <= target_reuse_min <= target_reuse_max <= 1.0:
        raise ValueError("target reuse range is invalid")
    if (
        threshold_step <= 0.0
        or threshold_min > threshold_max
        or veto_margin < 0.0
        or max_mean_action_mae < 0.0
        or max_p95_action_mae < 0.0
    ):
        raise ValueError("threshold range is invalid")
    if not margin_candidates:
        raise ValueError("margin candidates must not be empty")
    if any(not math.isfinite(value) or value < 0.0 for value in margin_candidates):
        raise ValueError("margin candidates must be finite and non-negative")
    margins = sorted(set(map(float, margin_candidates)))
    thresholds = []
    value = threshold_min
    while value <= threshold_max + threshold_step / 10.0:
        thresholds.append(round(value, 8))
        value += threshold_step
    candidates = [
        evaluate_region(
            rows,
            threshold=threshold,
            min_margin=margin,
            veto_margin=veto_margin,
        )
        for threshold in thresholds
        for margin in margins
    ]
    diagnostics = _candidate_diagnostics(
        candidates,
        rows=rows,
        veto_margin=veto_margin,
        target_reuse_min=target_reuse_min,
        target_reuse_max=target_reuse_max,
        max_mean_action_mae=max_mean_action_mae,
        max_p95_action_mae=max_p95_action_mae,
        min_projected_latency_reduction=min_projected_latency_reduction,
    )
    failure_masks = [row[-1] for row in diagnostics["grid"]]
    safe = [
        row
        for row, failure_mask in zip(candidates, failure_masks)
        if (failure_mask & 0b11100) == 0
    ]
    viable = [
        row
        for row, failure_mask in zip(candidates, failure_masks)
        if failure_mask == 0
    ]
    ranking = lambda row: (
        -float(row["reuse_rate"]),
        float(row["p95_action_mae"]),
        -float(row["projected_latency_reduction"]),
        float(row["threshold"]),
        float(row["min_margin"]),
    )
    return {
        "viable": bool(viable),
        "selected": _public_region(min(viable, key=ranking)) if viable else None,
        "best_safe": _public_region(min(safe, key=ranking)) if safe else None,
        "candidates": len(candidates),
        "diagnostics": diagnostics,
    }


def _namespace(task_index: int, memory_class: str) -> str:
    if memory_class not in {"open_hold", "closed_hold", "suppression"}:
        raise ValueError("unknown memory class")
    return f"libero-risk-task-{task_index}-{memory_class}"


def _contact_snapshot(env: Any) -> dict[str, int]:
    sim = env.envs[0]._env.sim
    pairs = []
    for index in range(int(sim.data.ncon)):
        contact = sim.data.contact[index]
        pairs.append(
            (
                sim.model.geom_id2name(int(contact.geom1)) or "",
                sim.model.geom_id2name(int(contact.geom2)) or "",
            )
        )
    return classify_contact_names(pairs)


def _insert_memories(
    client: exp27.AgentMemoryClient,
    memories: list[dict[str, Any]],
    vectors: Any,
    classes: list[str],
    admission_masks: list[int],
    *,
    experiment_id: int,
    run_id: str,
) -> dict[int, dict[str, int]]:
    availability: dict[int, dict[str, int]] = {}
    if not (
        len(memories) == len(classes) == len(admission_masks)
    ):
        raise ValueError("memory admission metadata must align")
    for sample, vector, memory_class, admission_mask in zip(
        memories, vectors.tolist(), classes, admission_masks
    ):
        task_index = int(sample["task_index"])
        availability.setdefault(
            task_index, {"open_hold": 0, "closed_hold": 0, "suppression": 0}
        )
        availability[task_index][memory_class] += 1
        memory_type = (
            EXECUTABLE_TYPE if memory_class != "suppression" else SUPPRESSION_TYPE
        )
        response, _ = client.request(
            "POST",
            "/api/ai/memories",
            {
                "memory_id": (
                    f"{run_id}-{task_index}-{memory_class}-"
                    f"{sample['episode_index']}"
                ),
                "tenant_id": _tenant_id(experiment_id, run_id),
                "namespace": _namespace(task_index, memory_class),
                "agent_id": AGENT_ID,
                "type": memory_type,
                "content": sample["task"],
                "metadata_json": json.dumps(
                    {
                        "episode_index": sample["episode_index"],
                        "row_index": sample["row_index"],
                        "task_index": task_index,
                        "memory_class": memory_class,
                        "admission_mask": int(admission_mask),
                        "action": sample["action"],
                    },
                    separators=(",", ":"),
                ),
                "embedding": vector,
                "token_count": max(1, len(sample["task"]) // 4),
                "importance": 0.0,
                "created_at_ns": sample["episode_index"] + 1,
            },
        )
        if not response.get("ok"):
            raise RuntimeError(f"ZeptoDB insert failed: {response}")
    return availability


def _search_partition(
    client: exp27.AgentMemoryClient,
    vector: Any,
    *,
    experiment_id: int,
    run_id: str,
    task_index: int,
    memory_class: str,
    top_k: int,
) -> tuple[list[dict[str, Any]], float]:
    memory_type = (
        EXECUTABLE_TYPE if memory_class != "suppression" else SUPPRESSION_TYPE
    )
    response, latency_ms = client.request(
        "POST",
        "/api/ai/memories/search",
        {
            "tenant_id": _tenant_id(experiment_id, run_id),
            "namespace": _namespace(task_index, memory_class),
            "agent_id": AGENT_ID,
            "type": memory_type,
            "query_embedding": vector.tolist(),
            "limit": top_k,
        },
    )
    matches = []
    for match in response.get("matches", []):
        metadata = json.loads(match["metadata_json"])
        if (
            int(metadata["task_index"]) != task_index
            or str(metadata["memory_class"]) != memory_class
        ):
            raise RuntimeError("risk-partition search crossed a hard partition")
        matches.append(
            {
                "similarity": float(match["similarity"]),
                "action": list(map(float, metadata["action"])),
                "episode_index": int(metadata.get("episode_index", -1)),
                "row_index": int(metadata.get("row_index", -1)),
                "admission_mask": int(metadata.get("admission_mask", 0)),
            }
        )
    return matches, latency_ms


def _prepare_memory_bank(
    args: argparse.Namespace,
    siglip: Any,
    siglip_processor: Any,
    client: exp27.AgentMemoryClient,
) -> tuple[dict[str, Any], dict[int, Any]]:
    import torch

    memories, _ = exp26.load_sample_manifest(
        args.sample_manifest,
        memory_per_task=args.memory_per_task,
        query_per_task=args.query_per_task,
    )
    with ThreadPoolExecutor(max_workers=args.download_workers) as pool:
        images = list(
            pool.map(lambda row: exp26._fetch_image(row["front_image_url"]), memories)
        )
    state_means, state_stds = exp26._state_stats(memories)
    action_scale = exp26._action_scale(memories)
    gripper_boundary = exp30.derive_gripper_boundary(memories)
    classified = [
        classify_memory_with_reason(
            state=list(map(float, row["state"])),
            action=list(map(float, row["action"])),
            gripper_boundary=gripper_boundary,
            gripper_guard_band=args.gripper_guard_band,
            translation_limit=args.translation_limit,
            rotation_limit=args.rotation_limit,
        )
        for row in memories
    ]
    classes = [memory_class for memory_class, _reason in classified]
    admission_masks = [
        memory_admission_mask(
            state=list(map(float, row["state"])),
            action=list(map(float, row["action"])),
            gripper_boundary=gripper_boundary,
            gripper_guard_band=args.gripper_guard_band,
            translation_limit=args.translation_limit,
            rotation_limit=args.rotation_limit,
        )
        for row in memories
    ]
    admission_reasons: dict[int, dict[str, int]] = {}
    admission_mask_counts: dict[int, dict[int, int]] = {}
    for row, (_memory_class, reason) in zip(memories, classified):
        task_index = int(row["task_index"])
        counts = admission_reasons.setdefault(task_index, {})
        counts[reason] = counts.get(reason, 0) + 1
    for row, admission_mask in zip(memories, admission_masks):
        task_index = int(row["task_index"])
        counts = admission_mask_counts.setdefault(task_index, {})
        counts[admission_mask] = counts.get(admission_mask, 0) + 1
    task_texts = {int(row["task_index"]): str(row["task"]) for row in memories}
    ordered_tasks = sorted(task_texts)
    text_inputs = siglip_processor(
        text=[task_texts[index] for index in ordered_tasks],
        padding="max_length",
        return_tensors="pt",
    )
    text_inputs = {key: value.to("cuda") for key, value in text_inputs.items()}
    with torch.inference_mode():
        text_features = siglip.get_text_features(**text_inputs)
        text_features = torch.nn.functional.normalize(text_features.float(), dim=-1)
    text_by_task = {
        task_index: text_features[position]
        for position, task_index in enumerate(ordered_tasks)
    }
    batches = []
    with torch.inference_mode():
        for start in range(0, len(memories), args.batch_size):
            image_features = exp26._encode_visual_batch(
                siglip, siglip_processor, images[start : start + args.batch_size]
            )
            text_batch = torch.stack(
                [
                    text_by_task[int(row["task_index"])]
                    for row in memories[start : start + args.batch_size]
                ]
            )
            batches.append(
                torch.nn.functional.normalize(
                    image_features + text_batch, dim=-1
                ).cpu()
            )
    visual = torch.cat(batches)
    vectors = torch.stack(
        [
            exp26._combine_embedding(vector, row["state"], state_means, state_stds)
            for vector, row in zip(visual, memories)
        ]
    )
    availability = _insert_memories(
        client,
        memories,
        vectors,
        classes,
        admission_masks,
        experiment_id=args.experiment_id,
        run_id=args.run_id,
    )
    return (
        {
            "memories": memories,
            "state_means": state_means,
            "state_stds": state_stds,
            "action_scale": action_scale,
            "gripper_boundary": gripper_boundary,
            "availability": availability,
            "admission_reasons": admission_reasons,
            "admission_mask_counts": admission_mask_counts,
            "task_texts": task_texts,
            "text_to_task": {text: index for index, text in task_texts.items()},
        },
        text_by_task,
    )


def _query_candidate(
    *,
    observation: dict[str, Any],
    task_index: int,
    hold_class: str,
    args: argparse.Namespace,
    siglip: Any,
    siglip_processor: Any,
    client: exp27.AgentMemoryClient,
    memory: dict[str, Any],
    text_by_task: dict[int, Any],
) -> dict[str, Any]:
    state = observation["observation.state"][0].tolist()
    vector, encoder_wall, encoder_gpu = exp26._encode_query(
        siglip,
        siglip_processor,
        observation["observation.images.image"][0].cpu(),
        text_by_task[task_index],
        {"state": state},
        memory["state_means"],
        memory["state_stds"],
    )
    positive, positive_search = _search_partition(
        client,
        vector,
        experiment_id=args.experiment_id,
        run_id=args.run_id,
        task_index=task_index,
        memory_class=hold_class,
        top_k=args.top_k,
    )
    if not positive:
        raise RuntimeError("declared executable partition returned no memories")
    suppression = []
    suppression_search = 0.0
    if memory["availability"][task_index]["suppression"]:
        suppression, suppression_search = _search_partition(
            client,
            vector,
            experiment_id=args.experiment_id,
            run_id=args.run_id,
            task_index=task_index,
            memory_class="suppression",
            top_k=args.top_k,
        )
    prior, confidence = exp26.weighted_action_prior(
        positive, memory["action_scale"]
    )
    margin = (
        float(positive[0]["similarity"]) - float(positive[1]["similarity"])
        if len(positive) > 1
        else 0.0
    )
    disagreement = exp30.candidate_disagreement(
        positive, prior, memory["action_scale"]
    )
    safe, safety_reason = candidate_safety(
        action=prior,
        state=state,
        hold_class=hold_class,
        disagreement=disagreement,
        state_means=memory["state_means"],
        state_stds=memory["state_stds"],
        translation_limit=args.translation_limit,
        rotation_limit=args.rotation_limit,
        disagreement_limit=args.disagreement_limit,
        state_z_limit=args.state_z_limit,
    )
    return {
        "prior": prior,
        "confidence": confidence,
        "margin": margin,
        "safe": safe,
        "safety_reason": safety_reason,
        "positive_similarity": float(positive[0]["similarity"]),
        "positive_second_similarity": (
            float(positive[1]["similarity"]) if len(positive) > 1 else None
        ),
        "negative_similarity": (
            float(suppression[0]["similarity"]) if suppression else None
        ),
        "positive_neighbors": len(positive),
        "negative_neighbors": len(suppression),
        "positive_episode_index": int(positive[0]["episode_index"]),
        "negative_episode_index": (
            int(suppression[0]["episode_index"]) if suppression else None
        ),
        "negative_admission_mask": (
            int(suppression[0]["admission_mask"]) if suppression else None
        ),
        "disagreement": disagreement,
        "encoder_wall": encoder_wall,
        "encoder_gpu": encoder_gpu,
        "positive_search_wall": positive_search,
        "suppression_search_wall": suppression_search,
        "search_wall": positive_search + suppression_search,
    }


def _run_episode(
    *,
    variant: str,
    task_id: int,
    seed: int,
    args: argparse.Namespace,
    policy: Any,
    preprocessor: Any,
    postprocessor: Any,
    siglip: Any,
    siglip_processor: Any,
    client: exp27.AgentMemoryClient,
    memory: dict[str, Any],
    text_by_task: dict[int, Any],
    threshold: float | None = None,
    min_margin: float | None = None,
) -> dict[str, Any]:
    import numpy as np
    from lerobot.envs.factory import make_env_pre_post_processors

    env, env_config = exp27._make_task_env(task_id, args.episode_length)
    env_preprocessor, env_postprocessor = make_env_pre_post_processors(
        env_config, policy.config
    )
    raw_observation, _ = env.reset(seed=[seed])
    task_description = list(env.call("task_description"))[0]
    task_index = exp27.resolve_memory_task_index(
        task_description, memory["text_to_task"]
    )
    traces: list[dict[str, Any]] = []
    decision_wall: list[float] = []
    policy_gpu: list[float] = []
    encoder_gpu: list[float] = []
    search_wall: list[float] = []
    fallback_counts: dict[str, int] = {}
    precheck_counts: dict[str, int] = {}
    contact_counts = {"finger": 0, "arm": 0}
    previous_width: float | None = None
    previous_reuse = False
    check_post_reuse = False
    reuses = 0
    vla_calls = 0
    post_reuse_hazards = 0
    success = False
    reward_total = 0.0
    steps = 0
    try:
        for step in range(args.episode_length):
            observation = exp27._processed_observation(
                env, raw_observation, env_preprocessor
            )
            state = observation["observation.state"][0].tolist()
            decision_start = time.perf_counter_ns()
            contacts = _contact_snapshot(env)
            contact_counts["finger"] += int(contacts["finger"] > 0)
            contact_counts["arm"] += int(contacts["arm"] > 0)
            state_z = max_state_z(
                state, memory["state_means"], memory["state_stds"]
            )
            width = gripper_width(state)
            observed_hold_class = (
                "ambiguous"
                if abs(width - memory["gripper_boundary"])
                <= args.gripper_guard_band
                else (
                    "open_hold"
                    if width > memory["gripper_boundary"]
                    else "closed_hold"
                )
            )
            if check_post_reuse and state_z > args.hazard_state_z:
                post_reuse_hazards += 1
            check_post_reuse = False
            hold_class, precheck_reason = free_space_precheck(
                state=state,
                previous_width=previous_width,
                contacts=contacts,
                available=memory["availability"][task_index],
                gripper_boundary=memory["gripper_boundary"],
                gripper_guard_band=args.gripper_guard_band,
                max_gripper_delta=args.max_gripper_delta,
                state_z=state_z,
                state_z_limit=args.state_z_limit,
            )
            precheck_counts[precheck_reason] = (
                precheck_counts.get(precheck_reason, 0) + 1
            )
            candidate = None
            memory_wall = 0.0
            if hold_class is not None:
                candidate = _query_candidate(
                    observation=observation,
                    task_index=task_index,
                    hold_class=hold_class,
                    args=args,
                    siglip=siglip,
                    siglip_processor=siglip_processor,
                    client=client,
                    memory=memory,
                    text_by_task=text_by_task,
                )
                memory_wall = float(candidate["encoder_wall"]) + float(
                    candidate["search_wall"]
                )
                encoder_gpu.append(float(candidate["encoder_gpu"]))
                search_wall.append(float(candidate["search_wall"]))
            policy_wall = 0.0
            policy_gpu_ms = 0.0
            reused_this_step = False
            if variant == "shadow":
                action, policy_wall, policy_gpu_ms = exp30._policy_action_for_step(
                    task_id=task_id,
                    step=step,
                    args=args,
                    policy=policy,
                    preprocessor=preprocessor,
                    postprocessor=postprocessor,
                    env_postprocessor=env_postprocessor,
                    observation=observation,
                )
                vla_calls += 1
                policy_gpu.append(policy_gpu_ms)
                candidate_mae = None
                candidate_dimension_errors = None
                if candidate is not None:
                    predicted = (
                        exp27._prior_action(candidate["prior"], env_postprocessor)
                        .reshape(-1)
                        .tolist()
                    )
                    expected = action.reshape(-1).tolist()
                    candidate_dimension_errors = [
                        abs(float(predicted[index]) - float(expected[index]))
                        / float(memory["action_scale"][index])
                        for index in range(len(memory["action_scale"]))
                    ]
                    candidate_mae = statistics.fmean(candidate_dimension_errors)
                traces.append(
                    {
                        "task_id": task_id,
                        "memory_task_index": task_index,
                        "episode_id": seed,
                        "step": step,
                        "query_phase": exp28.trajectory_phase(
                            step, args.episode_length
                        ),
                        "precheck_reason": precheck_reason,
                        "hold_class": hold_class,
                        "observed_hold_class": observed_hold_class,
                        "policy_wall": policy_wall,
                        "memory_wall": memory_wall,
                        "candidate": candidate,
                        "candidate_mae": candidate_mae,
                        "candidate_dimension_errors": candidate_dimension_errors,
                    }
                )
                previous_reuse = False
            elif variant == "routed":
                if threshold is None or min_margin is None:
                    raise ValueError("routed episode requires a frozen region")
                should_reuse, reason = route_candidate(
                    candidate,
                    threshold=threshold,
                    min_margin=min_margin,
                    veto_margin=args.veto_margin,
                    previous_reuse=previous_reuse,
                )
                if should_reuse:
                    action = exp27._prior_action(
                        candidate["prior"], env_postprocessor
                    )
                    reuses += 1
                    reused_this_step = True
                    check_post_reuse = True
                else:
                    action, policy_wall, policy_gpu_ms = (
                        exp30._policy_action_for_step(
                            task_id=task_id,
                            step=step,
                            args=args,
                            policy=policy,
                            preprocessor=preprocessor,
                            postprocessor=postprocessor,
                            env_postprocessor=env_postprocessor,
                            observation=observation,
                        )
                    )
                    vla_calls += 1
                    policy_gpu.append(policy_gpu_ms)
                    fallback_reason = (
                        precheck_reason if candidate is None else reason
                    )
                    fallback_counts[fallback_reason] = (
                        fallback_counts.get(fallback_reason, 0) + 1
                    )
                previous_reuse = should_reuse
            else:
                raise ValueError(f"unknown episode variant: {variant}")
            decision_wall.append(
                (time.perf_counter_ns() - decision_start) / 1_000_000
            )
            previous_width = gripper_width(state)
            raw_observation, reward, terminated, truncated, info = env.step(action)
            if reused_this_step and _contact_snapshot(env)["total"] > 0:
                post_reuse_hazards += 1
            reward_total += float(np.asarray(reward).reshape(-1)[0])
            steps = step + 1
            success = success or exp27.extract_success(info)
            if bool(np.asarray(terminated).reshape(-1)[0]) or bool(
                np.asarray(truncated).reshape(-1)[0]
            ):
                success = success or exp27.extract_success(info)
                break
    finally:
        env.close()
    return {
        "task_id": task_id,
        "memory_task_index": task_index,
        "task_description": task_description,
        "success": success,
        "steps": steps,
        "reward": reward_total,
        "vla_calls": vla_calls,
        "reuses": reuses,
        "traces": traces,
        "decision_wall": decision_wall,
        "policy_gpu": policy_gpu,
        "encoder_gpu": encoder_gpu,
        "search_wall": search_wall,
        "fallback_counts": fallback_counts,
        "precheck_counts": precheck_counts,
        "contact_counts": contact_counts,
        "post_reuse_hazards": post_reuse_hazards,
    }


def _flatten(rows: list[dict[str, Any]], key: str) -> list[Any]:
    return [value for row in rows for value in row[key]]


def _sum_counts(rows: list[dict[str, Any]], key: str) -> dict[str, int]:
    names = {name for row in rows for name in row[key]}
    return {name: sum(int(row[key].get(name, 0)) for row in rows) for name in names}


def _scaled_distribution(values: list[float]) -> list[int | None]:
    if not values:
        return [None, None, None]
    if not all(math.isfinite(float(value)) for value in values):
        raise ValueError("diagnostic distribution values must be finite")
    return [
        _scaled_metric(statistics.fmean(values)),
        _scaled_metric(exp27.percentile(values, 0.50)),
        _scaled_metric(exp27.percentile(values, 0.95)),
    ]


def _veto_candidate_rows(
    traces: list[dict[str, Any]], *, veto_margin: float, high_error_threshold: float
) -> list[dict[str, Any]]:
    if (
        not math.isfinite(float(high_error_threshold))
        or high_error_threshold < 0.0
    ):
        raise ValueError("high-error threshold must be finite and non-negative")
    result = []
    for trace in traces:
        candidate = trace.get("candidate")
        if candidate is None:
            continue
        phase = str(trace.get("query_phase"))
        hold_class = str(trace.get("hold_class"))
        if phase not in QUERY_PHASE_NAMES or hold_class not in HOLD_CLASS_NAMES[:2]:
            raise ValueError("candidate trace phase or hold class is invalid")
        candidate_mae = float(trace["candidate_mae"])
        dimension_errors = list(trace.get("candidate_dimension_errors") or [])
        if (
            not math.isfinite(candidate_mae)
            or len(dimension_errors) != 7
            or not all(math.isfinite(float(value)) for value in dimension_errors)
            or not math.isclose(
                candidate_mae,
                statistics.fmean(map(float, dimension_errors)),
                rel_tol=1e-9,
                abs_tol=1e-9,
            )
        ):
            raise ValueError("candidate action-error telemetry is inconsistent")
        positive_similarity = float(candidate["positive_similarity"])
        negative_similarity = candidate.get("negative_similarity")
        positive_neighbors = int(candidate["positive_neighbors"])
        negative_neighbors = int(candidate["negative_neighbors"])
        positive_second = candidate.get("positive_second_similarity")
        if positive_neighbors <= 0 or (
            (positive_neighbors >= 2) != (positive_second is not None)
        ):
            raise ValueError("positive-neighbor telemetry is inconsistent")
        if positive_second is not None:
            positive_second = float(positive_second)
            if not math.isfinite(positive_second) or not math.isclose(
                float(candidate["margin"]),
                positive_similarity - positive_second,
                rel_tol=1e-9,
                abs_tol=1e-9,
            ):
                raise ValueError("positive top-two margin is inconsistent")
        elif not math.isclose(
            float(candidate["margin"]), 0.0, rel_tol=0.0, abs_tol=1e-12
        ):
            raise ValueError("single-neighbor candidate margin must be zero")
        negative_fields_present = (
            negative_similarity is not None
            and candidate.get("negative_episode_index") is not None
            and candidate.get("negative_admission_mask") is not None
        )
        if (negative_neighbors > 0) != negative_fields_present:
            raise ValueError("negative-neighbor telemetry is inconsistent")
        group = veto_group(candidate, veto_margin)
        gap = (
            None
            if negative_similarity is None
            else positive_similarity - float(negative_similarity)
        )
        numeric = (
            float(candidate["confidence"]),
            float(candidate["margin"]),
            float(candidate["disagreement"]),
            positive_similarity,
        )
        if not all(math.isfinite(value) for value in numeric) or (
            gap is not None and not math.isfinite(gap)
        ):
            raise ValueError("candidate separability telemetry must be finite")
        safety_reason = str(candidate["safety_reason"])
        if safety_reason not in CANDIDATE_SAFETY_REASON_NAMES:
            raise ValueError("unknown candidate-safety proxy reason")
        result.append(
            {
                "task_id": int(trace["task_id"]),
                "memory_task_index": int(trace["memory_task_index"]),
                "episode_id": int(trace["episode_id"]),
                "step": int(trace["step"]),
                "phase": phase,
                "hold_class": hold_class,
                "group": group,
                "candidate_mae": candidate_mae,
                "dimension_errors": list(map(float, dimension_errors)),
                "high_error": candidate_mae > high_error_threshold,
                "positive_similarity": positive_similarity,
                "positive_second_similarity": positive_second,
                "negative_similarity": (
                    None
                    if negative_similarity is None
                    else float(negative_similarity)
                ),
                "gap": gap,
                "confidence": float(candidate["confidence"]),
                "margin": float(candidate["margin"]),
                "disagreement": float(candidate["disagreement"]),
                "positive_neighbors": positive_neighbors,
                "negative_neighbors": negative_neighbors,
                "positive_episode_index": int(
                    candidate["positive_episode_index"]
                ),
                "negative_episode_index": (
                    None
                    if candidate.get("negative_episode_index") is None
                    else int(candidate["negative_episode_index"])
                ),
                "negative_admission_mask": (
                    None
                    if candidate.get("negative_admission_mask") is None
                    else int(candidate["negative_admission_mask"])
                ),
                "safety_reason": safety_reason,
            }
        )
    return result


def _veto_detail_payload(
    candidate_rows: list[dict[str, Any]],
    *,
    experiment_id: int,
    run_id: str,
    task_map: list[list[Any]],
) -> dict[str, Any]:
    fields = [
        "suite_task_id",
        "episode_seed",
        "step",
        "query_phase",
        "hold",
        "veto_group",
        "positive_neighbors",
        "positive_top1",
        "positive_top2",
        "positive_source_episode",
        "negative_neighbors",
        "negative_top1",
        "negative_source_episode",
        "negative_admission_mask",
        "confidence",
        "positive_margin",
        "positive_negative_gap",
        "neighbor_disagreement",
        "candidate_mae",
        "dimension_error_0",
        "dimension_error_1",
        "dimension_error_2",
        "dimension_error_3",
        "dimension_error_4",
        "dimension_error_5",
        "dimension_error_6",
        "candidate_safety_reason",
    ]
    rows = []
    for row in candidate_rows:
        rows.append(
            [
                row["task_id"],
                row["episode_id"],
                row["step"],
                QUERY_PHASE_NAMES.index(row["phase"]),
                HOLD_CLASS_NAMES.index(row["hold_class"]),
                VETO_GROUP_NAMES.index(row["group"]),
                row["positive_neighbors"],
                _scaled_metric(row["positive_similarity"]),
                _scaled_metric(row["positive_second_similarity"])
                if row["positive_second_similarity"] is not None
                else None,
                row["positive_episode_index"],
                row["negative_neighbors"],
                _scaled_metric(row["negative_similarity"])
                if row["negative_similarity"] is not None
                else None,
                row["negative_episode_index"],
                row["negative_admission_mask"],
                _scaled_metric(row["confidence"]),
                _scaled_metric(row["margin"]),
                _scaled_metric(row["gap"]) if row["gap"] is not None else None,
                _scaled_metric(row["disagreement"]),
                _scaled_metric(row["candidate_mae"]),
                *[_scaled_metric(value) for value in row["dimension_errors"]],
                CANDIDATE_SAFETY_REASON_NAMES.index(row["safety_reason"]),
            ]
        )
    return {
        "schema": 2,
        "experiment": experiment_id,
        "run_id": run_id,
        "metric_scale": DIAGNOSTIC_SCALE,
        "fields": fields,
        "codebooks": {
            "query_phase": list(QUERY_PHASE_NAMES),
            "hold": list(HOLD_CLASS_NAMES),
            "veto_group": list(VETO_GROUP_NAMES),
            "candidate_safety_reason": list(CANDIDATE_SAFETY_REASON_NAMES),
        },
        "task_map_fields": [
            "suite_task_id",
            "manifest_task_index",
            "task_description",
        ],
        "task_map": task_map,
        "source_observability": {
            "contact_free_window": False,
            "phase": False,
            "subgoal": False,
        },
        "rows": rows,
    }


def _detail_rows_sha256(detail: dict[str, Any]) -> str:
    encoded = json.dumps(
        detail["rows"], allow_nan=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _veto_group_summary(candidate_rows: list[dict[str, Any]]) -> list[list[Any]]:
    summaries = []
    for group_code, group in enumerate(VETO_GROUP_NAMES):
        rows = [row for row in candidate_rows if row["group"] == group]
        source_counts: dict[int, int] = {}
        for row in rows:
            source = int(row["positive_episode_index"])
            source_counts[source] = source_counts.get(source, 0) + 1
        maximum_source = max(source_counts.values(), default=0)
        negative = [
            float(row["negative_similarity"])
            for row in rows
            if row["negative_similarity"] is not None
        ]
        gaps = [float(row["gap"]) for row in rows if row["gap"] is not None]
        summaries.append(
            [
                group_code,
                len(rows),
                sum(bool(row["high_error"]) for row in rows),
                *_scaled_distribution(
                    [float(row["candidate_mae"]) for row in rows]
                ),
                *_scaled_distribution(
                    [float(row["positive_similarity"]) for row in rows]
                ),
                *_scaled_distribution(negative),
                *_scaled_distribution(gaps),
                _scaled_metric(
                    statistics.fmean(float(row["confidence"]) for row in rows)
                )
                if rows
                else None,
                _scaled_metric(
                    statistics.fmean(float(row["margin"]) for row in rows)
                )
                if rows
                else None,
                *_scaled_distribution(
                    [float(row["disagreement"]) for row in rows]
                ),
                len(source_counts),
                _scaled_metric(maximum_source / len(rows)) if rows else None,
            ]
        )
    return summaries


def _veto_counterfactuals(
    candidate_rows: list[dict[str, Any]], margins: tuple[float, ...]
) -> list[list[Any]]:
    result = []
    previous_vetoed = -1
    for margin in margins:
        groups = []
        for row in candidate_rows:
            if row["negative_similarity"] is None:
                groups.append("no_negative_support")
            elif float(row["negative_similarity"]) >= (
                float(row["positive_similarity"]) - margin
            ):
                groups.append("vetoed")
            else:
                groups.append("separated")
        vetoed = groups.count("vetoed")
        if vetoed < previous_vetoed:
            raise ValueError("veto counterfactuals must be monotonic")
        previous_vetoed = vetoed
        separated_rows = [
            row for row, group in zip(candidate_rows, groups) if group == "separated"
        ]
        result.append(
            [
                _scaled_metric(margin),
                groups.count("no_negative_support"),
                groups.count("separated"),
                vetoed,
                *_scaled_distribution(
                    [float(row["candidate_mae"]) for row in separated_rows]
                ),
                sum(bool(row["high_error"]) for row in separated_rows),
            ]
        )
    return result


def _veto_phase_summary(candidate_rows: list[dict[str, Any]]) -> list[list[Any]]:
    result = []
    keys = sorted(
        {
            (int(row["task_id"]), str(row["phase"]), str(row["hold_class"]))
            for row in candidate_rows
        }
    )
    for task_id, phase, hold_class in keys:
        rows = [
            row
            for row in candidate_rows
            if (
                int(row["task_id"]),
                str(row["phase"]),
                str(row["hold_class"]),
            )
            == (task_id, phase, hold_class)
        ]
        gaps = [float(row["gap"]) for row in rows if row["gap"] is not None]
        sources = {int(row["positive_episode_index"]) for row in rows}
        result.append(
            [
                task_id,
                QUERY_PHASE_NAMES.index(phase),
                HOLD_CLASS_NAMES.index(hold_class),
                len(rows),
                *[sum(row["group"] == group for row in rows) for group in VETO_GROUP_NAMES],
                sum(bool(row["high_error"]) for row in rows),
                *_scaled_distribution(
                    [float(row["candidate_mae"]) for row in rows]
                ),
                _scaled_metric(exp27.percentile(gaps, 0.50)) if gaps else None,
                len(sources),
            ]
        )
    return result


def _query_support_summary(traces: list[dict[str, Any]]) -> list[list[int]]:
    keys = sorted(
        {
            (
                int(row["task_id"]),
                str(row["query_phase"]),
                str(row["observed_hold_class"]),
            )
            for row in traces
        }
    )
    result = []
    for task_id, phase, hold_class in keys:
        rows = [
            row
            for row in traces
            if (
                int(row["task_id"]),
                str(row["query_phase"]),
                str(row["observed_hold_class"]),
            )
            == (task_id, phase, hold_class)
        ]
        result.append(
            [
                task_id,
                QUERY_PHASE_NAMES.index(phase),
                HOLD_CLASS_NAMES.index(hold_class),
                len(rows),
                sum(row.get("candidate") is not None for row in rows),
                sum(row.get("precheck_reason") == "no_executable_memory" for row in rows),
            ]
        )
    return result


def _veto_structural_summary(
    traces: list[dict[str, Any]], *, veto_margin: float
) -> tuple[list[int | None], list[list[int | None]]]:
    def summarize(rows: list[dict[str, Any]], task_id: int | None) -> list[int | None]:
        candidate_count = sum(row.get("candidate") is not None for row in rows)
        candidate_mask = _cooldown_mask(
            rows, lambda row: row.get("candidate") is not None
        )
        safety_mask = _cooldown_mask(
            rows,
            lambda row: row.get("candidate") is not None
            and bool(row["candidate"]["safe"]),
        )
        fixed_pre = sum(
            row.get("candidate") is not None
            and bool(row["candidate"]["safe"])
            and not _negative_veto(row["candidate"], veto_margin)
            for row in rows
        )
        fixed_mask = _cooldown_mask(
            rows,
            lambda row: row.get("candidate") is not None
            and bool(row["candidate"]["safe"])
            and not _negative_veto(row["candidate"], veto_margin),
        )
        values: list[int | None] = [
            len(rows),
            candidate_count,
            sum(candidate_mask),
            sum(safety_mask),
            sum(
                row.get("candidate") is not None
                and _negative_veto(row["candidate"], veto_margin)
                for row in rows
            ),
            fixed_pre,
            sum(fixed_mask),
            _scaled_metric(_projected_latency_reduction(rows, candidate_mask)),
            _scaled_metric(_projected_latency_reduction(rows, fixed_mask)),
        ]
        return ([task_id] + values) if task_id is not None else values

    tasks = sorted({int(row["task_id"]) for row in traces})
    return summarize(traces, None), [
        summarize(
            [row for row in traces if int(row["task_id"]) == task_id], task_id
        )
        for task_id in tasks
    ]


def _resolved_task_map(
    rows: list[dict[str, Any]], memory: dict[str, Any]
) -> list[list[Any]]:
    mappings = []
    seen_suite_tasks: set[int] = set()
    seen_memory_tasks: set[int] = set()
    for row in sorted(rows, key=lambda item: int(item["task_id"])):
        suite_task_id = int(row["task_id"])
        memory_task_index = int(row["memory_task_index"])
        task_description = str(row["task_description"])
        if suite_task_id in seen_suite_tasks:
            raise ValueError("suite task appears more than once in task mapping")
        if memory_task_index in seen_memory_tasks:
            raise ValueError("memory task appears more than once in task mapping")
        if memory_task_index not in memory["availability"]:
            raise ValueError("resolved memory task is absent from availability")
        if memory.get("text_to_task", {}).get(task_description) != memory_task_index:
            raise ValueError("resolved task text/index mapping is inconsistent")
        if str(memory.get("task_texts", {}).get(memory_task_index)) != task_description:
            raise ValueError("resolved task description is inconsistent")
        seen_suite_tasks.add(suite_task_id)
        seen_memory_tasks.add(memory_task_index)
        mappings.append([suite_task_id, memory_task_index, task_description])
    return mappings


def _compact_veto_separability(
    args: argparse.Namespace,
    *,
    resolved_sha: str,
    memory: dict[str, Any],
    rows: list[dict[str, Any]],
) -> dict[str, Any]:
    traces = _flatten(rows, "traces")
    task_ids = sorted({int(row["task_id"]) for row in rows})
    task_map = _resolved_task_map(rows, memory)
    memory_task_ids = [int(row[1]) for row in task_map]
    normalized_task_map = tuple(
        (int(row[0]), int(row[1]), str(row[2])) for row in task_map
    )
    sample_manifest = Path(args.sample_manifest)
    manifest_semantic_sha = _semantic_manifest_sha256(sample_manifest)
    result_availability = {
        task_id: memory["availability"][task_id] for task_id in memory_task_ids
    }
    correction_scope = int(args.experiment_id) == TASK_MAPPING_CORRECTION_EXPERIMENT
    high_error_threshold = float(args.max_p95_action_mae)
    candidate_rows = _veto_candidate_rows(
        traces,
        veto_margin=float(args.veto_margin),
        high_error_threshold=high_error_threshold,
    )
    detail = _veto_detail_payload(
        candidate_rows,
        experiment_id=int(args.experiment_id),
        run_id=str(args.run_id),
        task_map=task_map,
    )
    detail_encoded = json.dumps(
        detail, allow_nan=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    args.veto_detail_payload = detail
    group_summary = _veto_group_summary(candidate_rows)
    counterfactuals = _veto_counterfactuals(
        candidate_rows, VETO_COUNTERFACTUAL_MARGINS
    )
    phase_summary = _veto_phase_summary(candidate_rows)
    support_summary = _query_support_summary(traces)
    pooled, task_structural = _veto_structural_summary(
        traces, veto_margin=float(args.veto_margin)
    )
    group_counts = [row[1] for row in group_summary]
    high_counts = [row[2] for row in group_summary]
    comparable = {
        VETO_GROUP_NAMES[index]: {
            "count": group_counts[index],
            "high": high_counts[index],
            "p95": group_summary[index][5],
        }
        for index in range(len(VETO_GROUP_NAMES))
    }
    vetoed = comparable["vetoed"]
    separated = comparable["separated"]
    minimum_group = int(args.veto_min_group_samples)
    support_sufficient = (
        vetoed["count"] >= minimum_group
        and separated["count"] >= minimum_group
    )
    vetoed_rate = (
        vetoed["high"] / vetoed["count"] if vetoed["count"] else None
    )
    separated_rate = (
        separated["high"] / separated["count"]
        if separated["count"]
        else None
    )
    rate_delta = (
        vetoed_rate - separated_rate
        if vetoed_rate is not None and separated_rate is not None
        else None
    )
    if not support_sufficient:
        separation_status = "underpowered"
    elif (
        rate_delta is not None
        and rate_delta >= 0.10
        and vetoed["p95"] is not None
        and separated["p95"] is not None
        and vetoed["p95"] >= separated["p95"]
    ):
        separation_status = "supported"
    elif rate_delta is not None and rate_delta <= 0.0:
        separation_status = "not_supported"
    else:
        separation_status = "mixed"
    policy_walls = [float(trace["policy_wall"]) for trace in traces]
    candidates = [
        trace["candidate"] for trace in traces if trace.get("candidate") is not None
    ]
    searches = _flatten(rows, "search_wall")
    encoder_walls = [float(candidate["encoder_wall"]) for candidate in candidates]
    positive_searches = [
        float(candidate.get("positive_search_wall", 0.0)) for candidate in candidates
    ]
    suppression_searches = [
        float(candidate.get("suppression_search_wall", 0.0))
        for candidate in candidates
    ]
    current_precheck = [
        sum(int(row["precheck_counts"].get(name, 0)) for row in rows)
        for name in PRECHECK_REASON_NAMES
    ]
    required_reuses = math.ceil(float(args.min_reuse_rate) * len(traces))
    pooled_structural_viable = (
        int(pooled[2]) >= required_reuses
        and int(pooled[7]) >= _scaled_metric(float(args.min_latency_reduction))
    )
    per_task_structural_viable = all(
        int(row[3]) >= math.ceil(float(args.min_reuse_rate) * int(row[1]))
        and int(row[8]) >= _scaled_metric(float(args.min_latency_reduction))
        for row in task_structural
    )
    query_support_complete = (
        all(int(row[2]) > 0 for row in task_structural)
        and sum(int(row[5]) for row in support_summary) == 0
    )
    admission = [
        [
            task_id,
            _count_vector(
                memory["admission_reasons"].get(task_id, {}),
                MEMORY_ADMISSION_REASON_NAMES,
            ),
            [
                [int(mask), int(count)]
                for mask, count in sorted(
                    memory["admission_mask_counts"].get(task_id, {}).items()
                )
            ],
        ]
        for task_id in memory_task_ids
    ]
    candidate_count = len(candidate_rows)
    configured_counterfactual = next(
        row
        for row in counterfactuals
        if row[0] == _scaled_metric(float(args.veto_margin))
    )
    shadow_action_accounting = [
        sum(int(row.get("vla_calls", -1)) for row in rows),
        sum(int(row.get("reuses", -1)) for row in rows),
    ]

    def structural_row_consistent(row: list[int | None], offset: int) -> bool:
        steps = int(row[offset])
        candidates = int(row[offset + 1])
        candidate_cooldown = int(row[offset + 2])
        safety_cooldown = int(row[offset + 3])
        vetoed_count = int(row[offset + 4])
        fixed_pre = int(row[offset + 5])
        fixed_cooldown = int(row[offset + 6])
        return (
            0 <= candidate_cooldown <= candidates <= steps
            and 0 <= safety_cooldown <= candidate_cooldown
            and 0 <= vetoed_count <= candidates
            and 0 <= fixed_cooldown <= fixed_pre <= candidates
            and fixed_cooldown <= safety_cooldown
        )

    structural_sums_match = all(
        int(pooled[index])
        == sum(int(row[index + 1]) for row in task_structural)
        for index in range(7)
    )

    def admission_consistent(row: list[Any]) -> bool:
        task_id = int(row[0])
        reasons = [int(value) for value in row[1]]
        masks = [[int(value) for value in item] for item in row[2]]
        availability = memory["availability"][task_id]
        total = sum(int(value) for value in availability.values())
        executable = int(availability["open_hold"]) + int(
            availability["closed_hold"]
        )
        return (
            len(reasons) == len(MEMORY_ADMISSION_REASON_NAMES)
            and all(value >= 0 for value in reasons)
            and reasons[0] == int(availability["open_hold"])
            and reasons[1] == int(availability["closed_hold"])
            and sum(reasons[2:]) == int(availability["suppression"])
            and sum(reasons) == total
            and all(
                len(item) == 2
                and 0 <= item[0] < (1 << len(MEMORY_ADMISSION_MASK_NAMES))
                and item[1] > 0
                for item in masks
            )
            and len({item[0] for item in masks}) == len(masks)
            and sum(item[1] for item in masks) == total
            and sum(item[1] for item in masks if item[0] == 0) == executable
            and sum(item[1] for item in masks if item[0] != 0)
            == int(availability["suppression"])
        )

    acceptance = {
        "candidate_groups_consistent": (
            sum(group_counts) == candidate_count
            and configured_counterfactual[1:4] == group_counts
            and all(
                int(count) >= 0 and 0 <= int(high) <= int(count)
                for count, high in zip(group_counts, high_counts)
            )
            and sum(high_counts)
            == sum(bool(row["high_error"]) for row in candidate_rows)
        ),
        "counterfactuals_consistent": all(
            sum(row[1:4]) == candidate_count for row in counterfactuals
        ),
        "phase_attribution_consistent": (
            sum(row[3] for row in phase_summary) == candidate_count
            and all(
                sum(row[4:7]) == row[3] and 0 <= row[7] <= row[3]
                for row in phase_summary
            )
        ),
        "support_attribution_consistent": (
            sum(row[3] for row in support_summary) == len(traces)
            and sum(row[4] for row in support_summary) == candidate_count
        ),
        "task_mapping_consistent": (
            len(task_map) == len(task_ids)
            and [int(row[0]) for row in task_map] == task_ids
            and all(
                int(trace["memory_task_index"])
                == next(
                    int(mapping[1])
                    for mapping in task_map
                    if int(mapping[0]) == int(trace["task_id"])
                )
                for trace in traces
            )
            and (
                not correction_scope
                or (
                    normalized_task_map == TASK_MAPPING_CORRECTION_EXPECTED_MAP
                    and result_availability
                    == TASK_MAPPING_CORRECTION_EXPECTED_AVAILABILITY
                    and manifest_semantic_sha
                    == EXPERIMENT_031_SEMANTIC_MANIFEST_SHA256
                )
            )
        ),
        "task_structural_consistent": (
            sum(row[1] for row in task_structural) == len(traces)
            and sum(row[2] for row in task_structural) == candidate_count
            and structural_sums_match
            and structural_row_consistent(pooled, 0)
            and all(structural_row_consistent(row, 1) for row in task_structural)
        ),
        "source_admission_consistent": all(
            admission_consistent(row) for row in admission
        ),
        "detail_rows_consistent": (
            len(detail["rows"]) == candidate_count
            and all(len(row) == len(detail["fields"]) for row in detail["rows"])
            and int(detail["schema"]) == 2
            and int(detail["experiment"]) == int(args.experiment_id)
            and detail["fields"][0] == "suite_task_id"
            and tuple(
                (int(row[0]), int(row[1]), str(row[2]))
                for row in detail["task_map"]
            )
            == normalized_task_map
        ),
        "routed_actions_zero": (
            shadow_action_accounting == [len(traces), 0]
            and all(
                int(row.get("vla_calls", -1)) == int(row["steps"])
                and int(row.get("reuses", -1)) == 0
                and len(row["traces"]) == int(row["steps"])
                for row in rows
            )
        ),
    }
    if correction_scope:
        acceptance["trace_replication_consistent"] = (
            len(traces) == EXPERIMENT_031_REFERENCE_STEPS
            and candidate_count == 127
            and group_counts == [0, 4, 123]
            and current_precheck == EXPERIMENT_031_REFERENCE_PRECHECK
            and _detail_rows_sha256(detail)
            == EXPERIMENT_033_CANDIDATE_ROWS_SHA256
        )
    telemetry_complete = all(acceptance.values())
    legacy_availability = {
        task_id: memory["availability"][task_id] for task_id in task_ids
    }
    result = {
        "status": "pass" if telemetry_complete else "fail",
        "experiment": int(args.experiment_id),
        "suite": SUITE,
        "run_id": str(args.run_id),
        "diagnostic_kind": "veto_separability",
        "completion_reason": (
            "veto_separability_complete"
            if telemetry_complete
            else "diagnostic_telemetry_incomplete"
        ),
        "vla_sha": resolved_sha,
        "siglip_model": str(args.siglip_model),
        "siglip_sha": str(args.resolved_siglip_sha),
        "harness_bundle_sha256": _harness_bundle_sha256(
            [Path(args.entrypoint_path)]
        ),
        "manifest_semantic_sha256": manifest_semantic_sha,
        "scope": {
            "suite_tasks": task_ids,
            "task_map": task_map,
            "steps": len(traces),
            "calibration_seed": int(args.calibration_seed),
        },
        "memory": {
            "records": len(memory["memories"]),
            "availability": result_availability,
            "admission": admission,
        },
        "diagnostics": {
            "schema": 2,
            "metric_scale": DIAGNOSTIC_SCALE,
            "veto_margin": _scaled_metric(float(args.veto_margin)),
            "high_error_threshold": _scaled_metric(high_error_threshold),
            "minimum_group_samples": minimum_group,
            "groups": group_summary,
            "counterfactuals": counterfactuals,
            "candidate_phases": phase_summary,
            "query_support": support_summary,
            "pooled_structural": pooled,
            "task_structural": task_structural,
        },
        "shadow": {
            "action_accounting": shadow_action_accounting,
            "task_precheck": [
                [
                    int(row["task_id"]),
                    int(row["steps"]),
                    _count_vector(row["precheck_counts"], PRECHECK_REASON_NAMES),
                ]
                for row in rows
            ],
            "timing": {
                "policy_mean": statistics.fmean(policy_walls),
                "encoder_mean": statistics.fmean(encoder_walls)
                if encoder_walls
                else 0.0,
                "positive_search_mean": statistics.fmean(positive_searches)
                if positive_searches
                else 0.0,
                "suppression_search_mean": statistics.fmean(suppression_searches)
                if suppression_searches
                else 0.0,
                "search_p95": exp27.percentile(searches, 0.95),
            },
        },
        "detail": {
            "rows": len(detail["rows"]),
            "sha256": hashlib.sha256(detail_encoded).hexdigest(),
        },
        "finding": {
            "separation_status": separation_status,
            "support_sufficient": support_sufficient,
            "vetoed_high_error_rate": _scaled_metric(vetoed_rate)
            if vetoed_rate is not None
            else None,
            "separated_high_error_rate": _scaled_metric(separated_rate)
            if separated_rate is not None
            else None,
            "high_error_rate_delta": _scaled_metric(rate_delta)
            if rate_delta is not None
            else None,
            "query_support_complete": query_support_complete,
            "pooled_structural_viable": pooled_structural_viable,
            "per_task_structural_viable": per_task_structural_viable,
            "replication_032_legacy_reported": [
                len(traces) == EXPERIMENT_031_REFERENCE_STEPS,
                current_precheck == EXPERIMENT_031_REFERENCE_PRECHECK,
                legacy_availability
                == EXPERIMENT_031_REFERENCE_AVAILABILITY,
                manifest_semantic_sha == EXPERIMENT_031_SEMANTIC_MANIFEST_SHA256,
                resolved_sha == VLA_MODEL_SHA,
                str(args.resolved_siglip_sha) == SIGLIP_MODEL_SHA,
            ],
        },
        "execution": {"diagnostic_only": True, "routed_started": False},
        "acceptance": acceptance,
    }
    return result


def _compact_failure(
    args: argparse.Namespace,
    *,
    resolved_sha: str,
    reason: str,
    memory: dict[str, Any],
    calibration: dict[str, Any],
    rows: list[dict[str, Any]],
    status: str = "fail",
) -> dict[str, Any]:
    traces = _flatten(rows, "traces")
    searches = _flatten(rows, "search_wall")
    task_ids = sorted({int(row["task_id"]) for row in rows})
    candidates = [
        trace["candidate"] for trace in traces if trace.get("candidate") is not None
    ]
    policy_walls = [float(trace["policy_wall"]) for trace in traces]
    sample_manifest = getattr(args, "sample_manifest", None)
    experiment_id = int(getattr(args, "experiment_id", EXPERIMENT))
    encoder_walls = [float(candidate["encoder_wall"]) for candidate in candidates]
    positive_searches = [
        float(candidate.get("positive_search_wall", candidate.get("search_wall", 0.0)))
        for candidate in candidates
    ]
    suppression_searches = [
        float(candidate.get("suppression_search_wall", 0.0))
        for candidate in candidates
    ]
    compact_calibration = {
        "viable": bool(calibration["viable"]),
        "candidates": int(calibration["candidates"]),
        "diagnostics": calibration["diagnostics"],
    }
    return {
        "status": status,
        "experiment": experiment_id,
        "suite": SUITE,
        "run_id": str(getattr(args, "run_id", "unknown")),
        "vla_sha": resolved_sha,
        "siglip_model": str(getattr(args, "siglip_model", SIGLIP_MODEL)),
        "siglip_sha": str(getattr(args, "resolved_siglip_sha", "unknown")),
        "harness_bundle_sha256": _harness_bundle_sha256(),
        "manifest_semantic_sha256": (
            _semantic_manifest_sha256(Path(sample_manifest))
            if sample_manifest is not None
            else None
        ),
        "stop_reason": reason,
        "scope": {
            "tasks": task_ids,
            "steps": len(traces),
            "calibration_seed": getattr(args, "calibration_seed", None),
            "config": [
                int(getattr(args, "episode_length", 0)),
                int(getattr(args, "base_policy_seed", 0)),
                _scaled_metric(getattr(args, "gripper_guard_band", 0.0)),
                _scaled_metric(getattr(args, "max_gripper_delta", 0.0)),
                _scaled_metric(getattr(args, "translation_limit", 0.0)),
                _scaled_metric(getattr(args, "rotation_limit", 0.0)),
                _scaled_metric(getattr(args, "disagreement_limit", 0.0)),
                _scaled_metric(getattr(args, "state_z_limit", 0.0)),
                _scaled_metric(getattr(args, "hazard_state_z", 0.0)),
            ],
        },
        "memory": {
            "records": len(memory["memories"]),
            "availability": {
                task_id: memory["availability"][task_id] for task_id in task_ids
            },
        },
        "calibration": compact_calibration,
        "shadow": {
            "task_precheck": [
                [
                    int(row["task_id"]),
                    int(row["steps"]),
                    _count_vector(row["precheck_counts"], PRECHECK_REASON_NAMES),
                ]
                for row in rows
            ],
            "timing": {
                "policy_mean": statistics.fmean(policy_walls) if policy_walls else 0.0,
                "encoder_mean": (
                    statistics.fmean(encoder_walls) if encoder_walls else 0.0
                ),
                "encoder_p95": exp27.percentile(encoder_walls, 0.95),
                "positive_search_mean": (
                    statistics.fmean(positive_searches) if positive_searches else 0.0
                ),
                "suppression_search_mean": (
                    statistics.fmean(suppression_searches)
                    if suppression_searches
                    else 0.0
                ),
                "search_mean": statistics.fmean(searches) if searches else 0.0,
                "search_p95": exp27.percentile(searches, 0.95),
            },
        },
        "acceptance": {
            "diagnostic_only": bool(getattr(args, "diagnostic_only", False)),
            "routed_started": False,
        },
    }


def run_experiment(args: argparse.Namespace) -> dict[str, Any]:
    import torch
    from huggingface_hub import model_info

    experiment_id = int(getattr(args, "experiment_id", EXPERIMENT))
    task_ids = exp28.parse_task_ids(args.task_ids)
    resolved_sha = model_info(args.vla_model, revision=VLA_MODEL_SHA).sha
    if resolved_sha != VLA_MODEL_SHA:
        raise RuntimeError(f"unexpected VLA revision: {resolved_sha}")
    args.resolved_siglip_sha = model_info(
        args.siglip_model, revision=SIGLIP_MODEL_SHA
    ).sha
    if args.resolved_siglip_sha != SIGLIP_MODEL_SHA:
        raise RuntimeError(
            f"unexpected SigLIP revision: {args.resolved_siglip_sha}"
        )
    args.vla_revision = VLA_MODEL_SHA
    args.siglip_revision = SIGLIP_MODEL_SHA
    siglip, siglip_processor, policy, preprocessor, postprocessor = exp26._load_models(
        args
    )
    if int(policy.config.n_action_steps) != 1:
        raise RuntimeError(
            f"Experiment {experiment_id:03d} requires n_action_steps=1"
        )
    client = exp27.AgentMemoryClient(args.agent_url, timeout=args.http_timeout)
    memory, text_by_task = _prepare_memory_bank(
        args, siglip, siglip_processor, client
    )

    def run(variant: str, task_id: int, seed: int, **region: float) -> dict[str, Any]:
        args.policy_seed_base = args.base_policy_seed + seed * 100000
        return _run_episode(
            variant=variant,
            task_id=task_id,
            seed=seed,
            args=args,
            policy=policy,
            preprocessor=preprocessor,
            postprocessor=postprocessor,
            siglip=siglip,
            siglip_processor=siglip_processor,
            client=client,
            memory=memory,
            text_by_task=text_by_task,
            **region,
        )

    calibration_rows = [
        run("shadow", task_id, args.calibration_seed + task_id)
        for task_id in task_ids
    ]
    for row in calibration_rows:
        memory_task_index = int(row["memory_task_index"])
        if not any(
            memory["availability"][memory_task_index][name]
            for name in ("open_hold", "closed_hold")
        ):
            raise RuntimeError(
                f"resolved memory task {memory_task_index} has no executable partition"
            )
    calibration_traces = _flatten(calibration_rows, "traces")
    if bool(getattr(args, "veto_separability", False)):
        return _compact_veto_separability(
            args,
            resolved_sha=resolved_sha,
            memory=memory,
            rows=calibration_rows,
        )
    calibration = select_region(
        calibration_traces,
        threshold_min=args.threshold_min,
        threshold_max=args.threshold_max,
        threshold_step=args.threshold_step,
        margin_candidates=args.margin_candidates,
        veto_margin=args.veto_margin,
        target_reuse_min=args.min_reuse_rate,
        target_reuse_max=args.max_reuse_rate,
        max_mean_action_mae=args.max_mean_action_mae,
        max_p95_action_mae=args.max_p95_action_mae,
        min_projected_latency_reduction=args.min_latency_reduction,
    )
    if args.diagnostic_only:
        diagnostics = calibration["diagnostics"]
        grid = diagnostics["grid"]
        gate_counts_consistent = diagnostics["gate_counts"] == [
            sum(bool(row[-1] & (1 << bit)) for row in grid) for bit in range(5)
        ]
        signature_counts_consistent = sum(
            diagnostics["signatures"].values()
        ) == len(grid)
        route_attribution_complete = sum(
            diagnostics["closest_rejections"]
        ) == diagnostics["ceilings"][0]
        task_attribution_complete = sum(
            row[1] for row in diagnostics["closest_tasks"]
        ) == diagnostics["ceilings"][0]
        hold_attribution_complete = sum(
            row[2] for row in diagnostics["closest_holds"]
        ) == diagnostics["ceilings"][1]
        acceptance = {
            "grid_48_complete": len(grid) == calibration["candidates"] == 48,
            "counts_consistent": (
                gate_counts_consistent and signature_counts_consistent
            ),
            "route_attribution_complete": (
                route_attribution_complete
                and task_attribution_complete
                and hold_attribution_complete
            ),
            "routed_actions_zero": True,
        }
        telemetry_complete = all(acceptance.values())
        result = _compact_failure(
            args,
            resolved_sha=resolved_sha,
            reason=(
                "calibration_failure_attribution_complete"
                if telemetry_complete
                else "diagnostic_telemetry_incomplete"
            ),
            memory=memory,
            calibration=calibration,
            rows=calibration_rows,
            status="pass" if telemetry_complete else "fail",
        )
        result["acceptance"] = acceptance
        result["execution"] = {
            "diagnostic_only": True,
            "routed_started": False,
        }
        current_precheck = [
            sum(row[2][index] for row in result["shadow"]["task_precheck"])
            for index in range(len(PRECHECK_REASON_NAMES))
        ]
        result["finding"] = {
            "calibration_viable": bool(calibration["viable"]),
            "replication_031": [
                diagnostics["ceilings"][0] == EXPERIMENT_031_REFERENCE_STEPS,
                current_precheck == EXPERIMENT_031_REFERENCE_PRECHECK,
                result["memory"]["availability"]
                == EXPERIMENT_031_REFERENCE_AVAILABILITY,
                result["manifest_semantic_sha256"]
                == EXPERIMENT_031_SEMANTIC_MANIFEST_SHA256,
                resolved_sha == VLA_MODEL_SHA,
            ],
        }
        if telemetry_complete:
            result["completion_reason"] = result.pop("stop_reason")
        return result
    if not calibration["viable"]:
        return _compact_failure(
            args,
            resolved_sha=resolved_sha,
            reason="no_viable_free_space_region",
            memory=memory,
            calibration=calibration,
            rows=calibration_rows,
        )
    selected = calibration["selected"]
    threshold = float(selected["threshold"])
    min_margin = float(selected["min_margin"])

    validation_rows = [
        run("shadow", task_id, args.evaluation_seed + task_id)
        for task_id in task_ids
    ]
    validation = evaluate_region(
        _flatten(validation_rows, "traces"),
        threshold=threshold,
        min_margin=min_margin,
        veto_margin=args.veto_margin,
    )
    validation_pass = (
        float(validation["reuse_rate"]) >= args.min_reuse_rate
        and float(validation["p95_action_mae"]) <= args.max_p95_action_mae
        and float(validation["projected_latency_reduction"])
        >= args.min_latency_reduction
    )
    if not validation_pass:
        validation_traces = _flatten(validation_rows, "traces")
        failure = _compact_failure(
            args,
            resolved_sha=resolved_sha,
            reason="held_out_shadow_preflight_failed",
            memory=memory,
            calibration=calibration,
            rows=calibration_rows,
        )
        failure["held_out"] = {
            "metrics": [
                len(validation_traces),
                int(validation["reuses"]),
                _scaled_metric(float(validation["mean_action_mae"])),
                _scaled_metric(float(validation["p95_action_mae"])),
                _scaled_metric(float(validation["projected_latency_reduction"])),
            ],
            "precheck": _sum_counts(validation_rows, "precheck_counts"),
        }
        return failure

    routed_rows = [
        run(
            "routed",
            task_id,
            args.evaluation_seed + task_id,
            threshold=threshold,
            min_margin=min_margin,
        )
        for task_id in task_ids
    ]
    task_results = [
        {
            "task_id": task_id,
            "baseline_success": baseline["success"],
            "routed_success": routed["success"],
            "baseline_steps": baseline["steps"],
            "routed_steps": routed["steps"],
            "routed_vla_calls": routed["vla_calls"],
            "routed_skips": routed["reuses"],
        }
        for task_id, baseline, routed in zip(
            task_ids, validation_rows, routed_rows
        )
    ]
    quality = exp27.paired_quality(task_results)
    decisions = sum(row["steps"] for row in routed_rows)
    reuses = sum(row["reuses"] for row in routed_rows)
    reuse_rate = reuses / decisions
    baseline_policy = [
        float(trace["policy_wall"])
        for trace in _flatten(validation_rows, "traces")
    ]
    routed_decisions = list(map(float, _flatten(routed_rows, "decision_wall")))
    latency_reduction = 1.0 - statistics.fmean(
        routed_decisions
    ) / statistics.fmean(baseline_policy)
    baseline_gpu = statistics.fmean(
        list(map(float, _flatten(validation_rows, "policy_gpu")))
    )
    routed_gpu = (
        sum(map(float, _flatten(routed_rows, "policy_gpu")))
        + sum(map(float, _flatten(routed_rows, "encoder_gpu")))
    ) / decisions
    searches = list(map(float, _flatten(routed_rows, "search_wall")))
    post_reuse_hazards = sum(
        int(row["post_reuse_hazards"]) for row in routed_rows
    )
    acceptance = {
        "held_out_shadow": validation_pass,
        "baseline_success": sum(row["success"] for row in validation_rows) >= 1,
        "success_noninferior": quality["success_rate_delta"] >= 0.0,
        "paired_regressions": quality["paired_regressions"] == 0,
        "actual_reuse_rate": reuse_rate >= args.min_reuse_rate,
        "latency_reduction": latency_reduction >= args.min_latency_reduction,
        "gpu_reduction": 1.0 - routed_gpu / baseline_gpu >= args.min_gpu_reduction,
        "post_reuse_hazards": post_reuse_hazards == 0,
        "search_p95": exp27.percentile(searches, 0.95) < 30.0,
    }
    return {
        "status": "pass" if all(acceptance.values()) else "fail",
        "experiment": experiment_id,
        "suite": SUITE,
        "vla_model": args.vla_model,
        "vla_sha": resolved_sha,
        "siglip_model": args.siglip_model,
        "runtime": {
            "gpu": torch.cuda.get_device_name(0),
            "cuda": torch.version.cuda,
            "peak_gpu_memory_bytes": int(torch.cuda.max_memory_allocated()),
        },
        "scope": {
            "tasks": task_ids,
            "calibration_seed": args.calibration_seed,
            "evaluation_seed": args.evaluation_seed,
            "memories": len(memory["memories"]),
            "gripper_boundary": memory["gripper_boundary"],
        },
        "memory": {"availability": memory["availability"]},
        "region": {
            "threshold": threshold,
            "min_margin": min_margin,
            "veto_margin": args.veto_margin,
        },
        "calibration": selected,
        "validation": validation,
        "quality": quality,
        "routed": {
            "steps": decisions,
            "vla_calls": sum(row["vla_calls"] for row in routed_rows),
            "reuses": reuses,
            "reuse_rate": reuse_rate,
            "decision": exp27.latency_summary(routed_decisions),
            "latency_reduction": latency_reduction,
            "gpu_per_decision_ms": routed_gpu,
            "gpu_reduction": 1.0 - routed_gpu / baseline_gpu,
            "post_reuse_hazards": post_reuse_hazards,
            "search": exp27.latency_summary(searches),
            "precheck": _sum_counts(routed_rows, "precheck_counts"),
            "fallback": _sum_counts(routed_rows, "fallback_counts"),
        },
        "tasks": [
            {
                "i": row["task_id"],
                "b": int(row["baseline_success"]),
                "r": int(row["routed_success"]),
                "bs": row["baseline_steps"],
                "rs": row["routed_steps"],
                "rv": row["routed_vla_calls"],
                "mr": row["routed_skips"],
            }
            for row in task_results
        ],
        "acceptance": acceptance,
    }


def _failure_labels(mask: int) -> str:
    labels = [
        name for index, name in enumerate(DIAGNOSTIC_GATE_NAMES) if mask & (1 << index)
    ]
    return ", ".join(labels) if labels else "pass"


def _decoded_diagnostics(result: dict[str, Any]) -> list[dict[str, Any]]:
    diagnostics = result.get("calibration", {}).get("diagnostics")
    if not diagnostics:
        return []
    metric_scale = int(diagnostics["metric_scale"])
    parameter_scale = int(diagnostics["parameter_scale"])
    steps = int(diagnostics["ceilings"][0])
    decoded = []
    for index, row in enumerate(diagnostics["grid"]):
        threshold, margin, reuses, mean_mae, p95_mae, latency, mask = row
        decoded.append(
            {
                "index": index,
                "threshold": threshold / parameter_scale,
                "margin": margin / parameter_scale,
                "reuses": int(reuses),
                "reuse_rate": int(reuses) / steps,
                "mean_action_mae": (
                    None if mean_mae is None else mean_mae / metric_scale
                ),
                "p95_action_mae": (
                    None if p95_mae is None else p95_mae / metric_scale
                ),
                "latency_reduction": latency / metric_scale,
                "failure_mask": int(mask),
            }
        )
    return decoded


def _pareto_indexes(rows: list[dict[str, Any]]) -> list[int]:
    finite = [
        row
        for row in rows
        if row["mean_action_mae"] is not None and row["p95_action_mae"] is not None
    ]
    unique: list[dict[str, Any]] = []
    seen: set[tuple[Any, ...]] = set()
    for row in finite:
        signature = (
            row["reuses"],
            row["mean_action_mae"],
            row["p95_action_mae"],
            row["latency_reduction"],
        )
        if signature not in seen:
            seen.add(signature)
            unique.append(row)

    def dominates(first: dict[str, Any], second: dict[str, Any]) -> bool:
        no_worse = (
            first["reuses"] >= second["reuses"]
            and first["mean_action_mae"] <= second["mean_action_mae"]
            and first["p95_action_mae"] <= second["p95_action_mae"]
            and first["latency_reduction"] >= second["latency_reduction"]
        )
        strictly_better = (
            first["reuses"] > second["reuses"]
            or first["mean_action_mae"] < second["mean_action_mae"]
            or first["p95_action_mae"] < second["p95_action_mae"]
            or first["latency_reduction"] > second["latency_reduction"]
        )
        return no_worse and strictly_better

    return [
        row["index"]
        for row in unique
        if not any(dominates(other, row) for other in unique if other is not row)
    ]


def _render_veto_separability_report(result: dict[str, Any], generated: str) -> str:
    diagnostics = result["diagnostics"]
    scale = int(diagnostics["metric_scale"])
    finding = result["finding"]
    experiment_id = int(result.get("experiment", 33))
    task_map = result["scope"].get("task_map")
    memory_axis_label = (
        "Manifest task index"
        if task_map is not None
        else "Legacy reported task key (unverified axis)"
    )
    detail_filename = (
        f"physical_ai_vla_veto_separability_candidates_{experiment_id:03d}.json"
    )
    replication = finding.get(
        "replication_032_legacy_reported",
        finding.get("replication_032", []),
    )
    replication_labels = (
        "steps",
        "precheck",
        "historical mis-keyed availability field",
        "semantic manifest",
        "VLA revision",
        "SigLIP revision",
    )

    def metric(value: int | None, digits: int = 6) -> str:
        return "N/A" if value is None else f"{value / scale:.{digits}f}"

    def rate(value: int | None) -> str:
        return "N/A" if value is None else f"{value / scale:.1%}"

    lines = [
        f"# Physical AI VLA Negative-Veto Separability {experiment_id:03d} Results",
        "",
        f"Generated at: {generated}",
        "Classification: Research-only",
        f"Status: {result['status']}",
        "",
        "## Result",
        "",
        f"Completed at `{result['completion_reason']}` after "
        f"{result['scope']['steps']} shadow steps on LIBERO suite tasks "
        f"{', '.join(map(str, result['scope'].get('suite_tasks', result['scope'].get('tasks', []))))}.",
        f"Separability finding: `{finding['separation_status']}`.",
        "No retrieved action was executed and no confidence/margin routing grid",
        "was evaluated. This run captures exploratory candidate attribution and",
        "coverage evidence; the frozen scope cannot supply a conclusive separated",
        "comparison group.",
        "",
        "Candidate-to-VLA agreement is a reference proxy, not action correctness,",
        "risk-free behavior, or physical-safety evidence. The source bank contains",
        "one midpoint per episode; source contact windows, semantic phases, and",
        "subgoals are not observable in this experiment.",
        "",
        "## Diagnostic Acceptance",
        "",
        "| Check | Status |",
        "| --- | --- |",
    ]
    lines.extend(
        f"| {name.replace('_', ' ')} | {'pass' if passed else 'fail'} |"
        for name, passed in result["acceptance"].items()
    )
    lines += [
        "",
        "## Provenance",
        "",
        f"- Run ID: `{result['run_id']}`.",
        f"- VLA revision: `{result['vla_sha']}`.",
        f"- SigLIP model/revision: `{result['siglip_model']}` / "
        f"`{result['siglip_sha']}`.",
        f"- Harness bundle SHA-256: `{result['harness_bundle_sha256']}`.",
        f"- Semantic manifest SHA-256: `{result['manifest_semantic_sha256']}`.",
        "- Candidate detail: "
        f"[`{detail_filename}`]({detail_filename}), "
        f"{result['detail']['rows']} rows, SHA-256 "
        f"`{result['detail']['sha256']}`.",
        "- Experiment 032 legacy reported anchors: "
        + ", ".join(
            f"{name}={'match' if matched else 'mismatch'}"
            for name, matched in zip(replication_labels, replication)
        )
        + ".",
        "  The historical availability field used suite IDs as manifest indexes;",
        "  its match is payload compatibility only, not queried-partition evidence.",
        "",
    ]
    if task_map is None:
        lines += [
            "## Legacy Schema Notice",
            "",
            "This schema-1 payload predates explicit suite-to-manifest task mapping.",
            "Its source-memory table is retained as historical evidence but its task",
            "keys are not verified manifest indexes and must not support coverage",
            "claims. Experiment 034 supplies the corrected attribution.",
        ]
    else:
        lines += [
            "## Task Identity Mapping",
            "",
            "LIBERO suite task IDs and manifest task indexes use different orders.",
            "Retrieval and source-admission attribution use the text-resolved manifest",
            "task index below.",
            "",
            "| LIBERO suite task ID | Manifest task index | Instruction |",
            "| ---: | ---: | --- |",
        ]
        lines.extend(f"| {row[0]} | {row[1]} | {row[2]} |" for row in task_map)
    lines += [
        "",
        "## Veto Groups",
        "",
        f"Configured veto margin: {diagnostics['veto_margin'] / scale:.3f}; "
        f"high-error threshold: > {diagnostics['high_error_threshold'] / scale:.3f}; "
        f"minimum comparable-group size: {diagnostics['minimum_group_samples']}.",
        "",
        "| Group | N | High error | Rate | MAE mean / p95 | Positive p50 | Negative p50 | Gap p50 / p95 | Disagreement mean | Unique top sources | Max source share |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in diagnostics["groups"]:
        group = VETO_GROUP_NAMES[int(row[0])]
        high_rate = row[2] / row[1] if row[1] else None
        lines.append(
            f"| {group} | {row[1]} | {row[2]} | "
            f"{'N/A' if high_rate is None else f'{high_rate:.1%}'} | "
            f"{metric(row[3])} / {metric(row[5])} | {metric(row[7])} | "
            f"{metric(row[10])} | {metric(row[13])} / {metric(row[14])} | "
            f"{metric(row[17])} | {row[20]} | {rate(row[21])} |"
        )
    lines += [
        "",
        "`no_negative_support` means no suppression neighbor existed. It is not",
        "evidence that the positive candidate was safely separated.",
        "",
        "### Veto-margin counterfactual",
        "",
        "These rows reclassify the same candidates without simulating route reuse.",
        "",
        "| Margin | No negative | Separated | Vetoed | Separated MAE mean / p95 | Separated high error |",
        "| ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in diagnostics["counterfactuals"]:
        lines.append(
            f"| {row[0] / scale:.3f} | {row[1]} | {row[2]} | {row[3]} | "
            f"{metric(row[4])} / {metric(row[6])} | {row[7]} |"
        )
    lines += [
        "",
        "## Query-Phase Candidate Attribution",
        "",
        "Query phase is the early/middle/late third of the configured episode",
        "limit, not a semantic source-memory phase.",
        "",
        "| LIBERO suite task | Query phase | Hold | N | No negative | Separated | Vetoed | High error | MAE mean / p95 | Gap p50 | Unique sources |",
        "| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in diagnostics["candidate_phases"]:
        lines.append(
            f"| {row[0]} | {QUERY_PHASE_NAMES[row[1]]} | "
            f"{HOLD_CLASS_NAMES[row[2]]} | {row[3]} | {row[4]} | {row[5]} | "
            f"{row[6]} | {row[7]} | {metric(row[8])} / {metric(row[10])} | "
            f"{metric(row[11])} | {row[12]} |"
        )
    lines += [
        "",
        "## Query Support And Source Admission",
        "",
        "| LIBERO suite task | Query phase | Observed hold | Steps | Candidates | Missing executable memory |",
        "| ---: | --- | --- | ---: | ---: | ---: |",
    ]
    for row in diagnostics["query_support"]:
        lines.append(
            f"| {row[0]} | {QUERY_PHASE_NAMES[row[1]]} | "
            f"{HOLD_CLASS_NAMES[row[2]]} | {row[3]} | {row[4]} | {row[5]} |"
        )
    lines += [
        "",
        f"| {memory_axis_label} | Open hold | Closed hold | Suppression | Admission reasons | Admission-mask histogram |",
        "| ---: | ---: | ---: | ---: | --- | --- |",
    ]
    admission_by_task = {int(row[0]): row for row in result["memory"]["admission"]}
    for task_id, availability in sorted(
        result["memory"]["availability"].items(), key=lambda item: int(item[0])
    ):
        admission = admission_by_task[int(task_id)]
        reasons = ", ".join(
            f"{name}={count}"
            for name, count in zip(MEMORY_ADMISSION_REASON_NAMES, admission[1])
            if count
        )
        masks = ", ".join(f"{mask}:{count}" for mask, count in admission[2])
        lines.append(
            f"| {task_id} | {availability['open_hold']} | "
            f"{availability['closed_hold']} | {availability['suppression']} | "
            f"{reasons or 'none'} | {masks or 'none'} |"
        )
    lines += [
        "",
        "Admission-mask bits are 1=gripper ambiguous, 2=command mismatch,",
        "4=translation limit, and 8=rotation limit; a mask of 0 means none of these",
        "source state/action rules fired. Source contact is not observable.",
    ]
    pooled = diagnostics["pooled_structural"]
    lines += [
        "",
        "## Structural Preflight",
        "",
        "| Scope | Steps | Candidates | Candidate + cooldown | Safety proxy + cooldown | Vetoed | Fixed pre-cooldown | Fixed + cooldown | Candidate latency | Fixed latency |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        f"| pooled | {pooled[0]} | {pooled[1]} | {pooled[2]} | {pooled[3]} | "
        f"{pooled[4]} | {pooled[5]} | {pooled[6]} | {rate(pooled[7])} | "
        f"{rate(pooled[8])} |",
    ]
    for row in diagnostics["task_structural"]:
        lines.append(
            f"| suite task {row[0]} | {row[1]} | {row[2]} | {row[3]} | {row[4]} | "
            f"{row[5]} | {row[6]} | {row[7]} | {rate(row[8])} | "
            f"{rate(row[9])} |"
        )
    timing = result["shadow"]["timing"]
    lines += [
        "",
        "## Timing",
        "",
        f"VLA mean {timing['policy_mean']:.3f} ms; encoder mean "
        f"{timing['encoder_mean']:.3f} ms; positive/suppression search mean "
        f"{timing['positive_search_mean']:.3f}/{timing['suppression_search_mean']:.3f} ms; "
        f"combined search p95 {timing['search_p95']:.3f} ms.",
        "",
        "## Interpretation",
        "",
        f"Comparable-group support sufficient: {finding['support_sufficient']}; "
        f"vetoed/separated high-error rates: "
        f"{rate(finding['vetoed_high_error_rate'])}/"
        f"{rate(finding['separated_high_error_rate'])}; delta "
        f"{rate(finding['high_error_rate_delta'])}.",
        f"Query support complete: {finding['query_support_complete']}; pooled/per-task "
        f"structural preflight viable: {finding['pooled_structural_viable']}/"
        f"{finding['per_task_structural_viable']}.",
        "",
        "The two episodes are serially correlated. Group differences are",
        "descriptive associations on one ordered trace, not independent causal",
        "effects. Underpowered groups cannot support either a separation or",
        "non-separation claim. The veto remains unchanged.",
        "",
        "## Next Research Step",
        "",
        "Use the observed support and admission gaps to pre-register a separate",
        "trajectory-window memory experiment with source contact/phase labels,",
        "suite task 5 / manifest task 9 open-hold coverage, and a frozen-bank",
        "paired control. Routed execution remains blocked.",
    ]
    return "\n".join(lines) + "\n"


def render_report(result: dict[str, Any]) -> str:
    generated = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    if result.get("diagnostic_kind") == "veto_separability":
        return _render_veto_separability_report(result, generated)
    diagnostics = result.get("calibration", {}).get("diagnostics")
    diagnostic_run = bool(result.get("execution", {}).get("diagnostic_only"))
    experiment_id = int(result.get("experiment", EXPERIMENT))
    title = (
        f"# Physical AI Calibration Failure Attribution {experiment_id:03d} Results"
        if diagnostics and diagnostic_run
        else f"# Physical AI Risk-Partitioned Free-Space Router {experiment_id:03d} Results"
    )
    lines = [
        title,
        "",
        f"Generated at: {generated}",
        "Classification: Research-only",
        f"Status: {result['status']}",
        "",
    ]
    decoded = _decoded_diagnostics(result)
    diagnostic_reason = result.get("completion_reason") or result.get("stop_reason")
    if diagnostic_reason and diagnostics:
        limits = [
            value / diagnostics["metric_scale"] for value in diagnostics["limits"]
        ]
        reuse_min, reuse_max, mean_max, p95_max, latency_min = limits
        metric_scale = int(diagnostics["metric_scale"])
        steps = int(diagnostics["ceilings"][0])
        required_reuses = math.ceil(reuse_min * steps)
        ceilings = diagnostics["ceilings"]
        closest = decoded[int(diagnostics["closest_index"])]
        task_label = ", ".join(map(str, result["scope"]["tasks"]))
        timing = result["shadow"].get("timing", {})
        viable = bool(
            result.get("finding", {}).get(
                "calibration_viable", result.get("calibration", {}).get("viable", False)
            )
        )
        action = "Completed" if result.get("completion_reason") else "Stopped"
        cleanup = result.get("acceptance", {}).get("aws_resources_deleted")
        cleanup_text = (
            "pass" if cleanup is True else "fail" if cleanup is False else "not recorded"
        )
        replication = result.get("finding", {}).get("replication_031", [])
        replication_labels = (
            "steps",
            "precheck",
            "memory partitions",
            "semantic manifest",
            "VLA revision",
        )
        replication_text = ", ".join(
            f"{name}={'match' if matched else 'mismatch'}"
            for name, matched in zip(replication_labels, replication)
        )
        task_precheck = result["shadow"]["task_precheck"]
        aggregate_precheck = [
            sum(row[2][index] for row in task_precheck)
            for index in range(len(PRECHECK_REASON_NAMES))
        ]
        lines += [
            "## Result",
            "",
            f"{action} at `{diagnostic_reason}` before routed execution.",
            f"Calibration scope: tasks {task_label}; {steps} shadow steps.",
            f"Calibration finding: {'a viable grid point exists' if viable else 'no viable grid point'}.",
            f"Experiment 031 replication fingerprint: {replication_text or 'not recorded'}.",
            f"AWS temporary-resource cleanup: {cleanup_text}.",
            "",
            "No memory action was executed. The grid points reuse the same ordered",
            "calibration traces; they are parameter counterfactuals, not independent",
            "trials. This diagnostic cannot establish physical-robot safety.",
            "",
            "## Diagnostic Acceptance",
            "",
            "| Check | Status |",
            "| --- | --- |",
        ]
        lines.extend(
            f"| {name.replace('_', ' ')} | {'pass' if passed else 'fail'} |"
            for name, passed in result.get("acceptance", {}).items()
        )
        lines += [
            "",
            "## Provenance And Timing",
            "",
            f"- Run ID: `{result.get('run_id', 'unknown')}`.",
            f"- VLA revision: `{result.get('vla_sha', 'unknown')}`.",
            f"- SigLIP model/revision: `{result.get('siglip_model', 'unknown')}` / "
            f"`{result.get('siglip_sha', 'unknown')}`.",
            f"- Harness bundle SHA-256: `{result.get('harness_bundle_sha256', 'unknown')}`.",
            f"- Semantic manifest SHA-256: `{result.get('manifest_semantic_sha256', 'unknown')}`.",
            f"- Calibration seed: {result['scope'].get('calibration_seed')}.",
            "",
            "| Timing component | Value (ms) |",
            "| --- | ---: |",
            f"| VLA policy mean | {timing.get('policy_mean', 0.0):.3f} |",
            f"| Query encoder mean / p95 | {timing.get('encoder_mean', 0.0):.3f} / {timing.get('encoder_p95', 0.0):.3f} |",
            f"| Positive search mean | {timing.get('positive_search_mean', 0.0):.3f} |",
            f"| Suppression search mean | {timing.get('suppression_search_mean', 0.0):.3f} |",
            f"| Combined search mean / p95 | {timing.get('search_mean', 0.0):.3f} / {timing.get('search_p95', 0.0):.3f} |",
            "",
            "## Structural Funnel",
            "",
            f"The pooled reuse floor requires {required_reuses}/{steps} actions "
            f"({reuse_min:.1%}). The table shows counterfactual ceilings on this one",
            "trace at the configured safety and veto settings.",
            "",
            "| Counterfactual stage | Reuses | Rate | Projected latency reduction |",
            "| --- | ---: | ---: | ---: |",
            f"| Precheck candidates, no cooldown | {ceilings[1]} | {ceilings[1] / steps:.1%} | N/A |",
            f"| Candidates + cooldown | {ceilings[2]} | {ceilings[2] / steps:.1%} | {ceilings[9] / metric_scale:.1%} |",
            f"| Candidate-safety proxy + cooldown | {ceilings[3]} | {ceilings[3] / steps:.1%} | N/A |",
            f"| Veto + cooldown | {ceilings[4]} | {ceilings[4] / steps:.1%} | N/A |",
            f"| Candidate-safety proxy + veto, no cooldown | {ceilings[5]} | {ceilings[5] / steps:.1%} | N/A |",
            f"| Candidate-safety proxy + veto + cooldown | {ceilings[6]} | {ceilings[6] / steps:.1%} | {ceilings[10] / metric_scale:.1%} |",
            "",
            f"The configured candidate-safety proxy rejected {ceilings[7]} candidates; "
            f"the negative-veto proxy matched {ceilings[8]}. They may overlap, and",
            "their individual counts are not causal effect estimates or physical-safety labels.",
            "",
            "| Precheck outcome | Count |",
            "| --- | ---: |",
        ]
        lines.extend(
            f"| {name} | {count} |"
            for name, count in zip(PRECHECK_REASON_NAMES, aggregate_precheck)
            if count
        )
        lines += [
            "",
            "| Task | Steps | Precheck outcomes |",
            "| ---: | ---: | --- |",
        ]
        lines.extend(
            f"| {task_id} | {task_steps} | "
            + ", ".join(
                f"{name}={count}"
                for name, count in zip(PRECHECK_REASON_NAMES, counts)
                if count
            )
            + " |"
            for task_id, task_steps, counts in task_precheck
        )
        lines += [
            "",
            "| Task | Open hold | Closed hold | Suppression |",
            "| ---: | ---: | ---: | ---: |",
        ]
        for task_id, availability in sorted(
            result["memory"]["availability"].items(), key=lambda item: int(item[0])
        ):
            lines.append(
                f"| {task_id} | {availability['open_hold']} | "
                f"{availability['closed_hold']} | {availability['suppression']} |"
            )

        lines += [
            "",
            "## Calibration Gate Attribution",
            "",
            "A grid point may fail more than one gate, so counts overlap. Gate",
            "frequency depends on the configured threshold/margin grid and is not an",
            "observation-level or causal failure frequency.",
            "Zero-reuse points have no measured MAE and are shown as N/A; their",
            "quality bits mean the quality requirement could not be demonstrated.",
            "",
            "| Gate | Requirement | Failed points | Passed points |",
            "| --- | --- | ---: | ---: |",
        ]
        requirements = (
            f">= {reuse_min:.1%}",
            f"<= {reuse_max:.1%}",
            f"<= {mean_max:.6f}",
            f"<= {p95_max:.6f}",
            f">= {latency_min:.1%}",
        )
        for name, requirement, failed in zip(
            DIAGNOSTIC_GATE_NAMES, requirements, diagnostics["gate_counts"]
        ):
            lines.append(
                f"| {name} | {requirement} | {failed} | {len(decoded) - failed} |"
            )

        signature_text = "; ".join(
            f"{mask} ({_failure_labels(int(mask))})={count}"
            for mask, count in sorted(
                diagnostics["signatures"].items(), key=lambda item: int(item[0])
            )
        )
        lines += [
            "",
            f"Failure-mask histogram: {signature_text}.",
            f"No-reuse grid points: {diagnostics['no_reuse_points']}.",
            "",
            "### Representative frontier",
            "",
            "The closest point minimizes failed-gate count, then normalized gate",
            "violation. It is a descriptive heuristic, not an optimum or held-out",
            "recommendation. Pareto points are non-dominated on pooled reuse, mean/p95",
            "MAE, and projected latency; identical metric rows are deduplicated.",
            "",
            f"Pareto grid indexes: {', '.join(map(str, _pareto_indexes(decoded)))}.",
            "",
            "| Anchor | Grid | Threshold | Margin | Reuse | Mean MAE | p95 MAE | Latency | Failures |",
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
        ]
        nonempty = [row for row in decoded if row["p95_action_mae"] is not None]
        anchors = [
            ("closest", closest),
            ("max reuse", max(decoded, key=lambda row: (row["reuses"], -row["index"]))),
            (
                "best latency",
                max(
                    decoded,
                    key=lambda row: (row["latency_reduction"], -row["index"]),
                ),
            ),
        ]
        if nonempty:
            anchors.extend(
                [
                    (
                        "best mean",
                        min(
                            nonempty,
                            key=lambda row: (
                                row["mean_action_mae"],
                                -row["reuses"],
                                row["index"],
                            ),
                        ),
                    ),
                    (
                        "best p95",
                        min(
                            nonempty,
                            key=lambda row: (
                                row["p95_action_mae"],
                                -row["reuses"],
                                row["index"],
                            ),
                        ),
                    ),
                ]
            )
        for label, row in anchors:
            mean_text = (
                "N/A"
                if row["mean_action_mae"] is None
                else f"{row['mean_action_mae']:.6f}"
            )
            p95_text = (
                "N/A"
                if row["p95_action_mae"] is None
                else f"{row['p95_action_mae']:.6f}"
            )
            lines.append(
                f"| {label} | {row['index']} | {row['threshold']:.3f} | "
                f"{row['margin']:.3f} | {row['reuses']}/{steps} "
                f"({row['reuse_rate']:.1%}) | {mean_text} | {p95_text} | "
                f"{row['latency_reduction']:.1%} | "
                f"{_failure_labels(row['failure_mask'])} |"
            )

        lines += [
            "",
            "### Closest candidate routing breakdown",
            "",
            f"Grid {closest['index']} uses confidence {closest['threshold']:.3f} and",
            f"margin {closest['margin']:.3f}. Routing outcomes below are the",
            "first failing condition in route order and therefore do not represent",
            "independent gate counts.",
            "",
            "| Route outcome | Count |",
            "| --- | ---: |",
        ]
        lines.extend(
            f"| {name} | {count} |"
            for name, count in zip(
                ROUTE_REASON_NAMES, diagnostics["closest_rejections"]
            )
            if count
        )
        lines += [
            "",
            "| Task | Steps | Candidates | Reuses | Mean MAE | p95 MAE | Latency |",
            "| ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
        for task_id, task_steps, candidate_count, reuses, mean, p95, latency in diagnostics[
            "closest_tasks"
        ]:
            mean_text = "N/A" if mean is None else f"{mean / metric_scale:.6f}"
            p95_text = "N/A" if p95 is None else f"{p95 / metric_scale:.6f}"
            lines.append(
                f"| {task_id} | {task_steps} | {candidate_count} | {reuses} | "
                f"{mean_text} | {p95_text} | {latency / metric_scale:.1%} |"
            )
        lines += [
            "",
            "| Task | Hold partition | Candidates | Reuses | Mean MAE | p95 MAE |",
            "| ---: | --- | ---: | ---: | ---: | ---: |",
        ]
        for task_id, hold_code, candidate_count, reuses, mean, p95 in diagnostics[
            "closest_holds"
        ]:
            mean_text = "N/A" if mean is None else f"{mean / metric_scale:.6f}"
            p95_text = "N/A" if p95 is None else f"{p95 / metric_scale:.6f}"
            lines.append(
                f"| {task_id} | {'open' if hold_code == 0 else 'closed'} | "
                f"{candidate_count} | {reuses} | {mean_text} | {p95_text} |"
            )

        maximum_gate_count = max(diagnostics["gate_counts"])
        common_gates = [
            name
            for name, count in zip(
                DIAGNOSTIC_GATE_NAMES, diagnostics["gate_counts"]
            )
            if count == maximum_gate_count
        ]
        outcome_fields = (
            "reuses",
            "mean_action_mae",
            "p95_action_mae",
            "latency_reduction",
            "failure_mask",
        )
        unique_outcomes = {
            tuple(row[field] for field in outcome_fields) for row in decoded
        }
        outcome_label = "row" if len(unique_outcomes) == 1 else "rows"
        outcomes_by_threshold: dict[float, set[tuple[Any, ...]]] = {}
        for row in decoded:
            outcomes_by_threshold.setdefault(row["threshold"], set()).add(
                tuple(row[field] for field in outcome_fields)
            )
        margins_inert = all(
            len(outcomes) == 1 for outcomes in outcomes_by_threshold.values()
        )
        query_mean = timing.get("encoder_mean", 0.0) + timing.get(
            "search_mean", 0.0
        )
        policy_mean = timing.get("policy_mean", 0.0)
        query_share = query_mean / policy_mean if policy_mean else 0.0
        veto_rate = ceilings[8] / ceilings[1] if ceilings[1] else 0.0
        missing_memory = aggregate_precheck[
            PRECHECK_REASON_NAMES.index("no_executable_memory")
        ]
        tasks_without_candidates = [
            str(task_id)
            for task_id, _task_steps, candidates, *_rest in diagnostics[
                "closest_tasks"
            ]
            if candidates == 0
        ]
        lines += [
            "",
            f"## All {len(decoded)} Calibration Points",
            "",
            "| Grid | Threshold | Margin | Reuse | Mean MAE | p95 MAE | Latency | Failures |",
            "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
        ]
        for row in decoded:
            mean_text = (
                "N/A"
                if row["mean_action_mae"] is None
                else f"{row['mean_action_mae']:.6f}"
            )
            p95_text = (
                "N/A"
                if row["p95_action_mae"] is None
                else f"{row['p95_action_mae']:.6f}"
            )
            lines.append(
                f"| {row['index']} | {row['threshold']:.3f} | "
                f"{row['margin']:.3f} | {row['reuses']}/{steps} "
                f"({row['reuse_rate']:.1%}) | {mean_text} | {p95_text} | "
                f"{row['latency_reduction']:.1%} | "
                f"{_failure_labels(row['failure_mask'])} |"
            )

        lines += [
            "",
            "## Interpretation",
            "",
            f"The most common grid gate flag(s) were {', '.join(f'`{name}`' for name in common_gates)} "
            f"({maximum_gate_count}/{len(decoded)} points). This describes this grid,",
            "not independent failures or causal dominance.",
        ]
        if replication and all(replication):
            lines += [
                "Experiment 032 matches all preserved Experiment 031 aggregate and",
                "semantic-input anchors. It is an instrumented replication of that",
                "setup, not recovery of the discarded original per-step trace.",
            ]
        elif replication:
            lines += [
                "At least one preserved Experiment 031 replication anchor differs.",
                "Therefore the 48-point attribution applies only to the new Experiment",
                "032 trace and cannot directly explain the original run.",
            ]
        if ceilings[2] < required_reuses:
            lines += [
                "The cooldown-only ceiling is already below the reuse floor, so",
                "threshold or margin tuning cannot make this design viable. Candidate",
                "coverage and/or cooldown policy would need to change on this trace.",
            ]
        elif ceilings[6] < required_reuses:
            lines += [
                "The precheck density is nominally sufficient, but the configured",
                "safety, negative-veto, and cooldown combination reduces it below the",
                "reuse floor. Confidence or margin tuning cannot close that deficit on",
                "this trace at these configured fixed gates.",
            ]
        else:
            lines += [
                "The configured fixed-gate ceiling does not itself preclude the reuse",
                "floor on this trace. The grid may show an association between density,",
                "quality, and latency, but it does not identify a causal mechanism.",
            ]
        lines += [
            "Metrics are pooled across two episodes and weighted by their step counts;",
            "they do not prove that either task individually passes. Task and hold MAE",
            "must be interpreted with their accepted-action counts, especially for p95.",
            "Projected latency uses measured encoder plus combined positive/suppression",
            "search time and VLA fallback time; it excludes cheap precheck and routing",
            "overhead. Any next experiment should remain shadow-only until a held-out",
            "region clears every gate.",
            "",
            "## Decision And Next Experiment",
            "",
            f"The {len(decoded)} grid points collapse to {len(unique_outcomes)} distinct outcome {outcome_label}.",
        ]
        if margins_inert:
            lines += [
                "Every configured margin produced the same outcome at a given",
                "confidence threshold, so another margin sweep over this memory bank",
                "would add little information.",
            ]
        if ceilings[1]:
            lines += [
                f"The configured negative veto matched {ceilings[8]}/{ceilings[1]} candidates "
                f"({veto_rate:.1%}), while the configured candidate-safety proxy rejected {ceilings[7]}.",
                "This identifies veto separability as the next uncertainty; it is not",
                "evidence that the veto should be weakened.",
            ]
        if query_mean:
            lines += [
                f"Mean encoder plus search cost was {query_mean:.3f} ms versus a",
                f"{policy_mean:.3f} ms VLA call ({query_share:.1%}). Search latency is",
                "not the binding constraint at the observed reuse density.",
            ]
        if tasks_without_candidates or missing_memory:
            task_text = ", ".join(tasks_without_candidates) or "none"
            lines += [
                f"Task(s) without a closest-point candidate: {task_text};",
                f"missing-executable-memory precheck blocks: {missing_memory}.",
                "Rebuild memory from contact-free, phase-local trajectory windows",
                "before interpreting another threshold grid.",
            ]
        lines += [
            "The next pre-registered experiment should remain shadow-only and record",
            "positive/suppression similarity gaps, phase support, neighbor",
            "disagreement, admission reasons, and candidate-to-VLA error for vetoed",
            "and allowed candidates. It should abort before a grid unless per-task",
            "coverage and the candidate-plus-cooldown ceiling can satisfy both reuse",
            "and projected-latency floors.",
        ]
        return "\n".join(lines) + "\n"
    if result.get("stop_reason"):
        shadow_timing = result["shadow"].get("timing", {})
        search_p95 = result["shadow"].get(
            "search_p95", shadow_timing.get("search_p95", 0.0)
        )
        lines += [
            "## Result",
            "",
            f"Stopped at `{result['stop_reason']}` before routed execution.",
            f"Shadow steps: {result['scope']['steps']}.",
            f"ZeptoDB search p95: {search_p95:.3f} ms.",
            "",
            "The compact result did not preserve per-candidate metrics, so exact",
            "attribution across the calibration grid is not recoverable from this run.",
        ]
        return "\n".join(lines) + "\n"
    routed = result["routed"]
    quality = result["quality"]
    validation = result["validation"]
    lines += [
        "## Held-Out Shadow",
        "",
        f"Projected reuse: {validation['reuse_rate']:.1%}; action MAE p95: "
        f"{validation['p95_action_mae']:.4f}; projected latency reduction: "
        f"{validation['projected_latency_reduction']:.1%}.",
        "",
        "## Closed Loop",
        "",
        f"- Actual free-space action reuse: {routed['reuses']}/"
        f"{routed['steps']} ({routed['reuse_rate']:.1%}).",
        f"- Corrected latency reduction: {routed['latency_reduction']:.1%}.",
        f"- GPU-time reduction per decision: {routed['gpu_reduction']:.1%}.",
        f"- Paired regressions: {quality['paired_regressions']}.",
        f"- Post-reuse proxy hazards: {routed['post_reuse_hazards']}.",
        f"- ZeptoDB search p50/p95: {routed['search']['p50_ms']:.3f}/"
        f"{routed['search']['p95_ms']:.3f} ms.",
        "",
        "## Acceptance",
        "",
        "| Check | Status |",
        "| --- | --- |",
    ]
    lines.extend(
        f"| {name.replace('_', ' ')} | {'pass' if passed else 'fail'} |"
        for name, passed in result["acceptance"].items()
    )
    lines += [
        "",
        "## Interpretation",
        "",
        "Only contact-free, stable-gripper states can execute memory actions.",
        "Non-executable memories are queried only as suppression evidence.",
        "Contact and transition regions continue through the VLA; VLA feature",
        "caching remains a separate experiment. This simulator result is not",
        "physical-robot safety certification.",
    ]
    return "\n".join(lines) + "\n"


def _write_result(path: Path, result: dict[str, Any]) -> None:
    encoded = json.dumps(
        result, allow_nan=False, separators=(",", ":"), sort_keys=True
    )
    if len(encoded.encode("utf-8")) > 3900:
        raise RuntimeError("compact result exceeds the Kubernetes termination-message limit")
    path.write_text(encoded)


def _write_detail_result(path: Path, detail: dict[str, Any]) -> str:
    encoded = json.dumps(
        detail, allow_nan=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    path.write_bytes(encoded)
    return hashlib.sha256(encoded).hexdigest()


def _bounded_error_result(experiment_id: int, exc: Exception) -> dict[str, Any]:
    error = str(exc)
    result = {
        "status": "error",
        "experiment": experiment_id,
        "error_type": type(exc).__name__,
        "error": error,
    }
    while len(
        json.dumps(
            result, allow_nan=False, separators=(",", ":"), sort_keys=True
        ).encode("utf-8")
    ) > 3900:
        error = error[: len(error) // 2]
        result["error"] = error
    return result


def main(
    *,
    default_experiment_id: int = EXPERIMENT,
    force_experiment_id: int | None = None,
    force_diagnostic_only: bool = False,
    force_veto_separability: bool = False,
) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--experiment-id", type=int, default=default_experiment_id)
    parser.add_argument("--agent-url", default="http://zepto-agent-memory:8123")
    parser.add_argument("--siglip-model", default=SIGLIP_MODEL)
    parser.add_argument("--vla-model", default=VLA_MODEL)
    parser.add_argument("--run-id", default=str(int(time.time())))
    parser.add_argument("--sample-manifest", type=Path)
    parser.add_argument("--memory-per-task", type=int, default=19)
    parser.add_argument("--query-per-task", type=int, default=10)
    parser.add_argument("--task-ids", default="0,5")
    parser.add_argument("--episode-length", type=int, default=520)
    parser.add_argument("--calibration-seed", type=int, default=28)
    parser.add_argument("--evaluation-seed", type=int, default=1028)
    parser.add_argument("--base-policy-seed", type=int, default=310000)
    parser.add_argument("--threshold-min", type=float, default=0.60)
    parser.add_argument("--threshold-max", type=float, default=0.90)
    parser.add_argument("--threshold-step", type=float, default=0.02)
    parser.add_argument(
        "--margin-candidates",
        type=lambda value: [float(item) for item in value.split(",")],
        default=[0.0, 0.005, 0.01],
    )
    parser.add_argument("--veto-margin", type=float, default=0.01)
    parser.add_argument("--gripper-guard-band", type=float, default=0.005)
    parser.add_argument("--max-gripper-delta", type=float, default=0.003)
    parser.add_argument("--translation-limit", type=float, default=0.75)
    parser.add_argument("--rotation-limit", type=float, default=0.15)
    parser.add_argument("--disagreement-limit", type=float, default=0.30)
    parser.add_argument("--state-z-limit", type=float, default=3.0)
    parser.add_argument("--hazard-state-z", type=float, default=4.0)
    parser.add_argument("--min-reuse-rate", type=float, default=0.20)
    parser.add_argument("--max-reuse-rate", type=float, default=0.35)
    parser.add_argument("--max-mean-action-mae", type=float, default=0.10)
    parser.add_argument("--max-p95-action-mae", type=float, default=0.15)
    parser.add_argument("--min-latency-reduction", type=float, default=0.15)
    parser.add_argument("--min-gpu-reduction", type=float, default=0.15)
    parser.add_argument("--top-k", type=int, default=5)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--download-workers", type=int, default=12)
    parser.add_argument("--http-timeout", type=float, default=15.0)
    parser.add_argument(
        "--diagnostic-only",
        action="store_true",
        help="stop after calibration shadow evaluation without routing actions",
    )
    parser.add_argument("--result", type=Path, default=Path("/dev/termination-log"))
    parser.add_argument("--render-json", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--detail-result", type=Path)
    args = parser.parse_args()
    args.entrypoint_path = str(Path(sys.argv[0]).resolve())
    args.veto_separability = bool(force_veto_separability)
    if force_experiment_id is not None:
        args.experiment_id = force_experiment_id
    if force_diagnostic_only:
        args.diagnostic_only = True
    if force_veto_separability:
        args.experiment_id = (
            force_experiment_id
            if force_experiment_id is not None
            else default_experiment_id
        )
        args.diagnostic_only = True
        args.task_ids = "0,5"
        args.episode_length = 520
        args.memory_per_task = 19
        args.query_per_task = 10
        args.calibration_seed = 28
        args.evaluation_seed = 1028
        args.base_policy_seed = 310000
        args.vla_model = VLA_MODEL
        args.siglip_model = SIGLIP_MODEL
        args.veto_margin = 0.01
        args.gripper_guard_band = 0.005
        args.max_gripper_delta = 0.003
        args.translation_limit = 0.75
        args.rotation_limit = 0.15
        args.disagreement_limit = 0.30
        args.state_z_limit = 3.0
        args.hazard_state_z = 4.0
        args.min_reuse_rate = 0.20
        args.max_p95_action_mae = 0.15
        args.min_latency_reduction = 0.15
        args.top_k = 5
        args.veto_min_group_samples = 20
    if args.render_json:
        result = json.loads(args.render_json.read_text())
        rendered = render_report(result)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(rendered)
        else:
            print(rendered, end="")
        return 0
    if force_veto_separability and args.experiment_id == 33:
        parser.error(
            "Experiment 033 is immutable and archived; use Experiment 034 for the correction run"
        )
    if args.sample_manifest is None:
        parser.error("--sample-manifest is required")
    if force_veto_separability and args.detail_result is None:
        parser.error(
            f"Experiment {args.experiment_id:03d} requires --detail-result"
        )
    try:
        result = run_experiment(args)
        if force_veto_separability:
            detail_sha = _write_detail_result(
                args.detail_result, args.veto_detail_payload
            )
            if detail_sha != result["detail"]["sha256"]:
                raise RuntimeError("veto detail artifact SHA-256 mismatch")
        _write_result(args.result, result)
        return 0 if result["status"] == "pass" else 2
    except Exception as exc:
        _write_result(args.result, _bounded_error_result(args.experiment_id, exc))
        raise


if __name__ == "__main__":
    raise SystemExit(main())
