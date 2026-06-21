#!/usr/bin/env python3
"""Validate the Action-Outcome Memory research fixture."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


REQUIRED_FIELDS = {
    "episode_id",
    "incident_id",
    "incident_type",
    "service",
    "environment",
    "entity_refs",
    "pre_action_window",
    "action_ts",
    "post_action_window",
    "symptoms",
    "topology_context",
    "change_context",
    "candidate_root_causes",
    "action",
    "policy_decision",
    "rollback_plan",
    "machine_outcome_label",
    "human_outcome_label",
    "recovery_curve",
    "evidence_refs",
    "reflection",
    "tags",
}

ACTION_CLASSES = {
    "restart",
    "scale_out",
    "rollback",
    "traffic_drain",
    "cache_purge",
    "config_revert",
    "queue_reset",
    "no_action",
}

POLICY_DECISIONS = {"allow", "deny", "escalate", "shadow_only"}
RISK_TIERS = {"low", "medium", "high", "critical"}
OUTCOME_LABELS = {
    "success",
    "partial_success",
    "failure",
    "rollback_required",
    "unsafe",
    "insufficient_evidence",
}


def parse_ts(value: str) -> datetime:
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def check(condition: bool, errors: list[str], message: str) -> None:
    if not condition:
        errors.append(message)


def validate_window(episode_id: str, name: str, window: Any, errors: list[str]) -> None:
    check(isinstance(window, dict), errors, f"{episode_id}: {name} must be an object")
    if not isinstance(window, dict):
        return
    check("start" in window, errors, f"{episode_id}: {name}.start missing")
    check("end" in window, errors, f"{episode_id}: {name}.end missing")
    if "start" in window and "end" in window:
        try:
            start = parse_ts(window["start"])
            end = parse_ts(window["end"])
            check(start <= end, errors, f"{episode_id}: {name}.start must be <= end")
        except ValueError as exc:
            errors.append(f"{episode_id}: invalid {name} timestamp: {exc}")


def validate_episode(episode: dict[str, Any], errors: list[str]) -> None:
    episode_id = episode.get("episode_id", "<missing episode_id>")
    missing = sorted(REQUIRED_FIELDS - set(episode))
    for field in missing:
        errors.append(f"{episode_id}: missing required field {field}")
    if missing:
        return

    check(isinstance(episode["entity_refs"], list) and episode["entity_refs"], errors, f"{episode_id}: entity_refs must be non-empty list")
    check(isinstance(episode["candidate_root_causes"], list) and episode["candidate_root_causes"], errors, f"{episode_id}: candidate_root_causes must be non-empty list")
    check(isinstance(episode["evidence_refs"], list) and episode["evidence_refs"], errors, f"{episode_id}: evidence_refs must be non-empty list")
    check(isinstance(episode["tags"], list) and episode["tags"], errors, f"{episode_id}: tags must be non-empty list")
    check(bool(episode["reflection"].strip()), errors, f"{episode_id}: reflection must be non-empty")

    validate_window(episode_id, "pre_action_window", episode["pre_action_window"], errors)
    validate_window(episode_id, "post_action_window", episode["post_action_window"], errors)

    try:
        action_ts = parse_ts(episode["action_ts"])
        pre_end = parse_ts(episode["pre_action_window"]["end"])
        post_start = parse_ts(episode["post_action_window"]["start"])
        check(pre_end <= action_ts, errors, f"{episode_id}: pre_action_window.end must be <= action_ts")
        check(action_ts <= post_start, errors, f"{episode_id}: action_ts must be <= post_action_window.start")
    except (KeyError, TypeError, ValueError) as exc:
        errors.append(f"{episode_id}: invalid action/window timestamp ordering: {exc}")

    action = episode["action"]
    check(action.get("action_class") in ACTION_CLASSES, errors, f"{episode_id}: invalid action_class {action.get('action_class')}")
    check(bool(action.get("target")), errors, f"{episode_id}: action.target missing")
    check(bool(action.get("actor")), errors, f"{episode_id}: action.actor missing")
    check(bool(action.get("tool")), errors, f"{episode_id}: action.tool missing")

    policy = episode["policy_decision"]
    check(policy.get("decision") in POLICY_DECISIONS, errors, f"{episode_id}: invalid policy decision {policy.get('decision')}")
    check(policy.get("risk_tier") in RISK_TIERS, errors, f"{episode_id}: invalid risk tier {policy.get('risk_tier')}")
    check(bool(policy.get("approval")), errors, f"{episode_id}: policy approval missing")

    for label_field in ("machine_outcome_label", "human_outcome_label"):
        label = episode[label_field]
        check(label.get("label") in OUTCOME_LABELS, errors, f"{episode_id}: invalid {label_field}.label {label.get('label')}")
        check(bool(label.get("assigned_by")), errors, f"{episode_id}: {label_field}.assigned_by missing")
        confidence = label.get("confidence")
        check(isinstance(confidence, (int, float)) and 0 <= confidence <= 1, errors, f"{episode_id}: {label_field}.confidence must be 0..1")

    recovery = episode["recovery_curve"]
    check(bool(recovery.get("primary_metric")), errors, f"{episode_id}: recovery_curve.primary_metric missing")
    check("before" in recovery, errors, f"{episode_id}: recovery_curve.before missing")
    check("after_5m" in recovery, errors, f"{episode_id}: recovery_curve.after_5m missing")
    check("after_15m" in recovery, errors, f"{episode_id}: recovery_curve.after_15m missing")
    check(isinstance(recovery.get("slo_restored"), bool), errors, f"{episode_id}: recovery_curve.slo_restored must be boolean")


def validate_fixture(data: dict[str, Any], min_family_size: int) -> tuple[list[str], dict[str, Any]]:
    errors: list[str] = []
    episodes = data.get("episodes")
    check(isinstance(episodes, list), errors, "top-level episodes must be a list")
    if not isinstance(episodes, list):
        return errors, {}

    ids = [episode.get("episode_id") for episode in episodes if isinstance(episode, dict)]
    duplicate_ids = [episode_id for episode_id, count in Counter(ids).items() if count > 1]
    for episode_id in duplicate_ids:
        errors.append(f"duplicate episode_id {episode_id}")

    for episode in episodes:
        if not isinstance(episode, dict):
            errors.append("episode must be an object")
            continue
        validate_episode(episode, errors)

    family_counts = Counter(episode.get("incident_type") for episode in episodes if isinstance(episode, dict))
    action_counts = Counter(episode.get("action", {}).get("action_class") for episode in episodes if isinstance(episode, dict))
    outcome_counts = Counter(episode.get("human_outcome_label", {}).get("label") for episode in episodes if isinstance(episode, dict))
    for family, count in family_counts.items():
        check(
            count >= min_family_size,
            errors,
            f"incident family {family} has {count} episodes; expected at least {min_family_size}",
        )

    summary = {
        "episodes": len(episodes),
        "incident_families": dict(sorted(family_counts.items())),
        "action_classes": dict(sorted(action_counts.items())),
        "human_outcomes": dict(sorted(outcome_counts.items())),
        "min_family_size": min_family_size,
    }
    return errors, summary


def render_report(fixture_path: Path, errors: list[str], summary: dict[str, Any]) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    lines = [
        "# Action-Outcome Fixture Validation 001",
        "",
        f"Generated at: {now}",
        f"Fixture: `{fixture_path}`",
        "",
        "## Summary",
        "",
        f"- Status: {'pass' if not errors else 'fail'}",
        f"- Episodes: {summary.get('episodes', 0)}",
        f"- Incident families: {len(summary.get('incident_families', {}))}",
        f"- Minimum family size: {summary.get('min_family_size', 0)}",
        "",
        "## Incident Family Counts",
        "",
        "| Incident Type | Episodes |",
        "| --- | ---: |",
    ]
    for family, count in summary.get("incident_families", {}).items():
        lines.append(f"| {family} | {count} |")

    lines += [
        "",
        "## Action Class Counts",
        "",
        "| Action Class | Episodes |",
        "| --- | ---: |",
    ]
    for action, count in summary.get("action_classes", {}).items():
        lines.append(f"| {action} | {count} |")

    lines += [
        "",
        "## Human Outcome Counts",
        "",
        "| Outcome | Episodes |",
        "| --- | ---: |",
    ]
    for outcome, count in summary.get("human_outcomes", {}).items():
        lines.append(f"| {outcome} | {count} |")

    lines += ["", "## Errors", ""]
    if errors:
        lines.extend(f"- {error}" for error in errors)
    else:
        lines.append("- None.")

    lines += [
        "",
        "## Next Steps",
        "",
        "1. Use validated fixtures in the context-gated outcome replay.",
        "2. Map the fixture into ZeptoDB SQL tables for replay through the database.",
        "3. Add real or lab-generated incident traces to replace synthetic-only claims.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", type=Path, default=Path("docs/research/fixtures/action_outcome_episodes.json"))
    parser.add_argument("--min-family-size", type=int, default=4)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    data = json.loads(args.fixture.read_text())
    errors, summary = validate_fixture(data, args.min_family_size)
    report = render_report(args.fixture, errors, summary)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report)
    else:
        print(report, end="")
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
