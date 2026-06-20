#!/usr/bin/env python3
"""Signal ablation report for Action-Outcome Memory research."""

from __future__ import annotations

import argparse
import json
import math
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import action_outcome_guardrail_compare as guardrail
import action_outcome_replay as replay


ABLATIONS = {
    "full_guarded": set(),
    "no_symptom": {"symptom"},
    "no_temporal": {"temporal_motif"},
    "no_topology": {"topology"},
    "no_change": {"change"},
    "no_action_outcome": {"action_outcome"},
    "no_text": {"postmortem_text"},
    "no_recency": {"recency"},
    "no_risk": {"risk_penalty"},
    "no_cross_family_guardrail": {"cross_family_penalty"},
}

WEIGHTS = {
    "symptom": 0.20,
    "temporal_motif": 0.20,
    "topology": 0.15,
    "change": 0.15,
    "action_outcome": 0.15,
    "postmortem_text": 0.10,
    "recency": 0.05,
}


def ablated_components(
    query: dict[str, Any],
    candidate: dict[str, Any],
    removed: set[str],
) -> dict[str, float]:
    components = replay.action_outcome_component_scores(query, candidate)
    cross_penalty = guardrail.cross_family_penalty(query, candidate)
    if "cross_family_penalty" in removed:
        cross_penalty = 0.0

    total = 0.0
    active_weight = 0.0
    for name, weight in WEIGHTS.items():
        if name in removed:
            continue
        total += weight * components[name]
        active_weight += weight

    # Keep score ranges roughly comparable after removing positive components.
    if active_weight > 0:
        total *= sum(WEIGHTS.values()) / active_weight

    if "risk_penalty" not in removed:
        total -= components["risk_penalty"]
    total -= cross_penalty

    components["cross_family_penalty"] = cross_penalty
    components["ablated_total"] = total
    return components


def ablated_score(removed: set[str]):
    def scorer(query: dict[str, Any], candidate: dict[str, Any]) -> float:
        return ablated_components(query, candidate, removed)["ablated_total"]

    return scorer


def robust_actions_for_ablation(
    query: dict[str, Any],
    rows: list[tuple[float, dict[str, Any]]],
    removed: set[str],
    limit: int = 8,
) -> list[tuple[str, float]]:
    positive: dict[str, float] = defaultdict(float)
    negative: dict[str, float] = defaultdict(float)
    support: dict[str, float] = defaultdict(float)
    success_counts: dict[str, int] = defaultdict(int)
    failure_counts: dict[str, int] = defaultdict(int)

    for score, episode in rows[:limit]:
        penalty = guardrail.cross_family_penalty(query, episode)
        if "cross_family_penalty" in removed:
            penalty = 0.0
        if penalty >= 0.12:
            continue
        action = episode["action"]["action_class"]
        relevance = max(score, 0.0)
        if penalty > 0:
            relevance *= 0.5
        value = 1.0 if "action_outcome" in removed else replay.outcome_value(episode)
        support[action] += relevance
        if value > 0:
            positive[action] += relevance * value
            success_counts[action] += 1
        elif value < 0:
            negative[action] += relevance * abs(value)
            failure_counts[action] += 1

    ranked: list[tuple[str, float]] = []
    for action in support:
        if support[action] == 0:
            continue
        score = (positive[action] - 1.25 * negative[action]) / support[action]
        score += 0.04 * math.log1p(success_counts[action])
        score -= 0.08 * failure_counts[action]
        ranked.append((action, score))
    ranked.sort(key=lambda item: item[1], reverse=True)
    return ranked


def cross_family_stats(
    query: dict[str, Any],
    rows: list[tuple[float, dict[str, Any]]],
) -> tuple[int, int]:
    cross_family = 0
    weak_cross_family = 0
    for _, candidate in rows[:3]:
        if candidate["incident_type"] == query["incident_type"]:
            continue
        cross_family += 1
        if replay.topology_similarity(query, candidate) < 0.55:
            weak_cross_family += 1
    return cross_family, weak_cross_family


def load_episodes(fixture_path: Path, extra_fixture_paths: list[Path]) -> list[dict[str, Any]]:
    episodes: list[dict[str, Any]] = []
    for path in [fixture_path, *extra_fixture_paths]:
        data = json.loads(path.read_text())
        episodes.extend(data["episodes"])

    ids = [episode["episode_id"] for episode in episodes]
    duplicate_ids = sorted(episode_id for episode_id, count in Counter(ids).items() if count > 1)
    if duplicate_ids:
        raise SystemExit(f"Duplicate episode ids: {', '.join(duplicate_ids)}")
    return episodes


def load_quality_labels(path: Path | None) -> dict[tuple[str, str], str]:
    if path is None:
        return {}
    data = json.loads(path.read_text())
    labels: dict[tuple[str, str], str] = {}
    for row in data.get("labels", []):
        labels[(row["query_id"], row["candidate_id"])] = row["label"]
    return labels


def quality_label(
    labels: dict[tuple[str, str], str],
    query_id: str,
    candidate_id: str,
) -> str:
    return labels.get((query_id, candidate_id), "unlabeled")


def evaluate_variant(
    episodes: list[dict[str, Any]],
    queries: list[dict[str, Any]],
    variant: str,
    removed: set[str],
    quality_labels: dict[tuple[str, str], str],
) -> dict[str, Any]:
    per_query = []
    for query in queries:
        rows = replay.ranked(episodes, query, ablated_score(removed))
        actions = robust_actions_for_ablation(query, rows, removed)
        useful_actions = replay.successful_actions_for_type(episodes, query["incident_type"])
        top_action = actions[0][0] if actions else "none"
        top3_actions = {action for action, _ in actions[:3]}
        cross, weak_cross = cross_family_stats(query, rows)
        retrieval_top3 = []
        quality_counts: Counter[str] = Counter()
        for rank, (score, candidate) in enumerate(rows[:3], start=1):
            label = quality_label(quality_labels, query["episode_id"], candidate["episode_id"])
            quality_counts[label] += 1
            retrieval_top3.append(
                {
                    "rank": rank,
                    "episode_id": candidate["episode_id"],
                    "incident_type": candidate["incident_type"],
                    "action": candidate["action"]["action_class"],
                    "outcome": replay.outcome_label(candidate),
                    "score": score,
                    "quality_label": label,
                }
            )
        per_query.append(
            {
                "query_id": query["episode_id"],
                "top_action": top_action,
                "actions": actions[:3],
                "top3_hit": bool(useful_actions & top3_actions),
                "avoids_failed": not (
                    replay.outcome_value(query) < 0 and top_action == query["action"]["action_class"]
                ),
                "cross_family_top3": cross,
                "weak_cross_family_top3": weak_cross,
                "retrieval_top3": retrieval_top3,
                "quality_counts": dict(quality_counts),
            }
        )

    aggregate_quality: Counter[str] = Counter()
    for row in per_query:
        aggregate_quality.update(row["quality_counts"])

    return {
        "variant": variant,
        "removed": sorted(removed),
        "top3_hit_rate": sum(row["top3_hit"] for row in per_query) / len(per_query),
        "failed_avoidance_rate": sum(row["avoids_failed"] for row in per_query) / len(per_query),
        "cross_family_top3_count": sum(row["cross_family_top3"] for row in per_query),
        "weak_cross_family_top3_count": sum(row["weak_cross_family_top3"] for row in per_query),
        "quality_counts": dict(aggregate_quality),
        "per_query": per_query,
    }


def format_actions(actions: list[tuple[str, float]]) -> str:
    return ", ".join(f"{action}:{score:.2f}" for action, score in actions)


def format_quality_counts(counts: dict[str, int]) -> str:
    ordered = ["useful", "superficial", "misleading", "unlabeled"]
    parts = [f"{label}:{counts.get(label, 0)}" for label in ordered if counts.get(label, 0)]
    return ", ".join(parts) if parts else "none"


def render_report(
    results: list[dict[str, Any]],
    fixture_path: Path,
    extra_fixture_paths: list[Path],
    quality_label_path: Path | None,
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    baseline = next(result for result in results if result["variant"] == "full_guarded")
    baseline_actions = {
        row["query_id"]: row["top_action"]
        for row in baseline["per_query"]
    }
    all_fixture_paths = [fixture_path, *extra_fixture_paths]

    lines = [
        "# ActionOutcomeReplay Ablation Results",
        "",
        f"Generated at: {now}",
        "Fixtures:",
        *[f"- `{path}`" for path in all_fixture_paths],
        f"- Quality labels: `{quality_label_path}`" if quality_label_path else "- Quality labels: none",
        "",
        "## Summary",
        "",
        "| Variant | Removed Signals | Top-3 Hit Rate | Failed-Action Avoidance | Cross-Family Top3 | Weak Cross-Family Top3 | Top Action Changes | Labeled Top3 Quality |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |",
    ]

    for result in results:
        changes = sum(
            row["top_action"] != baseline_actions[row["query_id"]]
            for row in result["per_query"]
        )
        removed = ", ".join(result["removed"]) if result["removed"] else "none"
        lines.append(
            "| {variant} | {removed} | {hit:.2f} | {avoid:.2f} | {cross} | {weak_cross} | {changes} |".format(
                variant=result["variant"],
                removed=removed,
                hit=result["top3_hit_rate"],
                avoid=result["failed_avoidance_rate"],
                cross=result["cross_family_top3_count"],
                weak_cross=result["weak_cross_family_top3_count"],
                changes=changes,
            )
            + f" {format_quality_counts(result['quality_counts'])} |"
        )

    lines += [
        "",
        "## Per-Query Top Action Changes",
        "",
        "| Query | Full Guarded | no_symptom | no_temporal | no_topology | no_change | no_action_outcome | no_text | no_recency | no_risk | no_cross_family_guardrail |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]

    result_by_variant = {result["variant"]: result for result in results}
    query_ids = [row["query_id"] for row in baseline["per_query"]]
    for query_id in query_ids:
        row_values = [query_id]
        for variant in ABLATIONS:
            variant_rows = {
                row["query_id"]: row
                for row in result_by_variant[variant]["per_query"]
            }
            row_values.append(variant_rows[query_id]["top_action"])
        lines.append("| " + " | ".join(row_values) + " |")

    lines += [
        "",
        "## Per-Variant Recommendations",
        "",
    ]

    for result in results:
        lines += [
            f"### {result['variant']}",
            "",
            "| Query | Top Actions | Hit | Avoids Failed Repeat | Cross-Family Top3 | Labeled Top3 Quality |",
            "| --- | --- | --- | --- | ---: | --- |",
        ]
        for row in result["per_query"]:
            lines.append(
                "| {query_id} | {actions} | {hit} | {avoid} | {cross} | {quality} |".format(
                    query_id=row["query_id"],
                    actions=format_actions(row["actions"]),
                    hit="yes" if row["top3_hit"] else "no",
                    avoid="yes" if row["avoids_failed"] else "no",
                    cross=row["cross_family_top3"],
                    quality=format_quality_counts(row["quality_counts"]),
                )
            )
        lines.append("")

    if quality_label_path:
        lines += [
            "## Labeled Top-3 Retrieval Details",
            "",
            "| Variant | Query | Rank | Candidate | Candidate Family | Action | Outcome | Score | Quality Label |",
            "| --- | --- | ---: | --- | --- | --- | --- | ---: | --- |",
        ]
        for result in results:
            for row in result["per_query"]:
                for candidate in row["retrieval_top3"]:
                    lines.append(
                        "| {variant} | {query} | {rank} | {candidate} | {family} | {action} | {outcome} | {score:.3f} | {label} |".format(
                            variant=result["variant"],
                            query=row["query_id"],
                            rank=candidate["rank"],
                            candidate=candidate["episode_id"],
                            family=candidate["incident_type"],
                            action=candidate["action"],
                            outcome=candidate["outcome"],
                            score=candidate["score"],
                            label=candidate["quality_label"],
                        )
                    )
        lines.append("")

    lines += [
        "## Interpretation",
        "",
        "Ablation is useful only as a directional fixture-level signal. The current",
        "fixture is synthetic and clean, so any variant that performs perfectly",
        "still needs to be challenged with noisy distractor episodes and public or",
        "lab-generated data.",
        "",
        "The most important comparison is `full_guarded` versus",
        "`no_action_outcome`. If removing action-outcome evidence does not change",
        "recommendations or safety metrics, the fixture is not yet proving the core",
        "research claim strongly enough.",
        "",
        "## Next Steps",
        "",
        "1. Add or refine a context-conditioned outcome gate for noisy same-family distractors.",
        "2. Map fixture episodes into ZeptoDB tables for SQL-backed replay.",
        "3. Add operator-facing explanations for misleading or suppressed historical evidence.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", type=Path, default=Path("docs/research/fixtures/action_outcome_episodes.json"))
    parser.add_argument("--extra-fixture", action="append", type=Path, default=[])
    parser.add_argument("--quality-labels", type=Path)
    parser.add_argument("--output", type=Path, default=Path("docs/research/results/action_outcome_ablation_003.md"))
    parser.add_argument("--query-id", action="append", dest="query_ids")
    args = parser.parse_args()

    episodes = load_episodes(args.fixture, args.extra_fixture)
    quality_labels = load_quality_labels(args.quality_labels)
    by_id = {episode["episode_id"]: episode for episode in episodes}
    query_ids = args.query_ids or replay.DEFAULT_QUERY_IDS
    missing = [query_id for query_id in query_ids if query_id not in by_id]
    if missing:
        raise SystemExit(f"Unknown query ids: {', '.join(missing)}")

    queries = [by_id[query_id] for query_id in query_ids]
    results = [
        evaluate_variant(episodes, queries, variant, removed, quality_labels)
        for variant, removed in ABLATIONS.items()
    ]
    report = render_report(results, args.fixture, args.extra_fixture, args.quality_labels)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)


if __name__ == "__main__":
    main()
