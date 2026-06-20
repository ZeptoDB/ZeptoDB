#!/usr/bin/env python3
"""Context-conditioned outcome gate for Action-Outcome Memory research."""

from __future__ import annotations

import argparse
import math
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import action_outcome_ablation as ablation
import action_outcome_guardrail_compare as guardrail
import action_outcome_replay as replay


def metric_value(episode: dict[str, Any], name: str) -> float | None:
    value = episode.get("symptoms", {}).get("metrics", {}).get(name)
    if isinstance(value, (int, float)):
        return float(value)
    return None


def change_type(episode: dict[str, Any]) -> str:
    return str(episode.get("change_context", {}).get("change_type") or "none")


def blast_radius_text(episode: dict[str, Any]) -> str:
    return str(episode.get("topology_context", {}).get("blast_radius") or "").lower()


def is_single_entity_context(episode: dict[str, Any]) -> bool:
    blast = blast_radius_text(episode)
    skew = metric_value(episode, "pod_error_skew_pct")
    heartbeat_gap = metric_value(episode, "worker_heartbeat_gap_s")
    return (
        "single" in blast
        or "one pod" in blast
        or "one consumer" in blast
        or (skew is not None and skew >= 50)
        or (heartbeat_gap is not None and heartbeat_gap >= 60)
    )


def high_metric(episode: dict[str, Any], name: str, threshold: float) -> bool:
    value = metric_value(episode, name)
    return value is not None and value >= threshold


def low_metric(episode: dict[str, Any], name: str, threshold: float) -> bool:
    value = metric_value(episode, name)
    return value is not None and value <= threshold


def metric_mismatch_reasons(query: dict[str, Any], candidate: dict[str, Any]) -> list[str]:
    reasons = []

    if not is_single_entity_context(query) and is_single_entity_context(candidate):
        reasons.append("single_entity_vs_service_wide")

    if low_metric(query, "cpu_pct", 60) and high_metric(candidate, "cpu_pct", 85):
        reasons.append("candidate_cpu_saturated_query_not")

    if high_metric(query, "db_conn_used_pct", 90) and low_metric(candidate, "db_conn_used_pct", 80):
        reasons.append("query_db_saturated_candidate_not")

    if low_metric(query, "consumer_error_pct", 2) and high_metric(candidate, "consumer_error_pct", 10):
        reasons.append("candidate_consumer_errors_query_healthy")

    if high_metric(query, "freshness_lag_s", 1000) and high_metric(candidate, "error_rate_pct", 1):
        if change_type(query) == "feature_flag" and change_type(candidate) == "deploy":
            reasons.append("cache_symptom_from_deploy_not_flag")

    query_change = change_type(query)
    candidate_change = change_type(candidate)
    if query_change != candidate_change:
        high_signal_changes = {"deploy", "feature_flag", "config", "schema_change", "index_refresh"}
        if query_change in high_signal_changes or candidate_change in high_signal_changes:
            reasons.append(f"change_type_mismatch:{query_change}->{candidate_change}")

    return reasons


def context_gate_details(query: dict[str, Any], candidate: dict[str, Any]) -> dict[str, Any]:
    components = replay.action_outcome_component_scores(query, candidate)
    base_context = (
        0.30 * components["symptom"]
        + 0.25 * components["temporal_motif"]
        + 0.20 * components["topology"]
        + 0.20 * components["change"]
        + 0.05 * components["postmortem_text"]
    )
    reasons = metric_mismatch_reasons(query, candidate)
    penalty = 0.0
    for reason in reasons:
        if reason == "single_entity_vs_service_wide":
            penalty += 0.40
        elif reason == "candidate_cpu_saturated_query_not":
            penalty += 0.35
        elif reason == "query_db_saturated_candidate_not":
            penalty += 0.35
        elif reason == "candidate_consumer_errors_query_healthy":
            penalty += 0.35
        elif reason == "cache_symptom_from_deploy_not_flag":
            penalty += 0.25
        elif reason.startswith("change_type_mismatch"):
            penalty += 0.20

    context_score = max(0.0, min(1.0, base_context - penalty))
    if context_score < 0.35 or penalty >= 0.40:
        multiplier = 0.15
    elif context_score < 0.50 or penalty >= 0.30:
        multiplier = 0.35
    elif context_score < 0.65 or penalty > 0:
        multiplier = 0.65
    else:
        multiplier = 1.0

    return {
        "base_context": base_context,
        "context_score": context_score,
        "multiplier": multiplier,
        "penalty": penalty,
        "reasons": reasons,
    }


def context_gated_components(query: dict[str, Any], candidate: dict[str, Any]) -> dict[str, Any]:
    components = replay.action_outcome_component_scores(query, candidate)
    gate = context_gate_details(query, candidate)
    cross_penalty = guardrail.cross_family_penalty(query, candidate)
    neutral = 0.5
    gated_action_outcome = neutral + (components["action_outcome"] - neutral) * gate["multiplier"]
    total = (
        0.20 * components["symptom"]
        + 0.20 * components["temporal_motif"]
        + 0.15 * components["topology"]
        + 0.15 * components["change"]
        + 0.15 * gated_action_outcome
        + 0.10 * components["postmortem_text"]
        + 0.05 * components["recency"]
        - components["risk_penalty"]
        - cross_penalty
    )
    components.update(
        {
            "cross_family_penalty": cross_penalty,
            "context_base": gate["base_context"],
            "context_score": gate["context_score"],
            "context_multiplier": gate["multiplier"],
            "context_penalty": gate["penalty"],
            "context_reasons": gate["reasons"],
            "gated_action_outcome": gated_action_outcome,
            "context_gated_total": total,
        }
    )
    return components


def context_gated_score(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    return context_gated_components(query, candidate)["context_gated_total"]


def context_gated_actions(
    query: dict[str, Any],
    rows: list[tuple[float, dict[str, Any]]],
    limit: int = 8,
) -> tuple[list[tuple[str, float]], list[dict[str, Any]]]:
    positive: dict[str, float] = defaultdict(float)
    negative: dict[str, float] = defaultdict(float)
    support: dict[str, float] = defaultdict(float)
    success_counts: dict[str, float] = defaultdict(float)
    failure_counts: dict[str, float] = defaultdict(float)
    suppressed: list[dict[str, Any]] = []

    for score, episode in rows[:limit]:
        components = context_gated_components(query, episode)
        penalty = components["cross_family_penalty"]
        if penalty >= 0.12:
            continue
        action = episode["action"]["action_class"]
        relevance = max(score, 0.0)
        if penalty > 0:
            relevance *= 0.5

        multiplier = components["context_multiplier"]
        raw_value = replay.outcome_value(episode)
        gated_value = raw_value * multiplier
        support[action] += relevance
        if raw_value > 0:
            positive[action] += relevance * gated_value
            success_counts[action] += multiplier
        elif raw_value < 0:
            negative[action] += relevance * abs(gated_value)
            failure_counts[action] += multiplier

        if multiplier < 1.0 and raw_value != 0:
            suppressed.append(
                {
                    "episode_id": episode["episode_id"],
                    "action": action,
                    "outcome": replay.outcome_label(episode),
                    "raw_value": raw_value,
                    "gated_value": gated_value,
                    "multiplier": multiplier,
                    "context_score": components["context_score"],
                    "reasons": components["context_reasons"],
                }
            )

    ranked: list[tuple[str, float]] = []
    for action in support:
        if support[action] == 0:
            continue
        action_score = (positive[action] - 1.25 * negative[action]) / support[action]
        action_score += 0.04 * math.log1p(success_counts[action])
        action_score -= 0.08 * failure_counts[action]
        ranked.append((action, action_score))
    ranked.sort(key=lambda item: item[1], reverse=True)
    return ranked, suppressed


def evaluate_context_gated(
    episodes: list[dict[str, Any]],
    queries: list[dict[str, Any]],
    quality_labels: dict[tuple[str, str], str],
) -> dict[str, Any]:
    per_query = []
    for query in queries:
        rows = replay.ranked(episodes, query, context_gated_score)
        actions, suppressed = context_gated_actions(query, rows)
        useful_actions = replay.successful_actions_for_type(episodes, query["incident_type"])
        top_action = actions[0][0] if actions else "none"
        top3_actions = {action for action, _ in actions[:3]}
        cross, weak_cross = ablation.cross_family_stats(query, rows)
        retrieval_top3 = []
        quality_counts: Counter[str] = Counter()
        for rank, (score, candidate) in enumerate(rows[:3], start=1):
            components = context_gated_components(query, candidate)
            label = ablation.quality_label(quality_labels, query["episode_id"], candidate["episode_id"])
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
                    "context_multiplier": components["context_multiplier"],
                    "context_score": components["context_score"],
                    "reasons": components["context_reasons"],
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
                "quality_counts": dict(quality_counts),
                "retrieval_top3": retrieval_top3,
                "suppressed": suppressed,
            }
        )

    aggregate_quality: Counter[str] = Counter()
    for row in per_query:
        aggregate_quality.update(row["quality_counts"])

    return {
        "variant": "context_gated",
        "top3_hit_rate": sum(row["top3_hit"] for row in per_query) / len(per_query),
        "failed_avoidance_rate": sum(row["avoids_failed"] for row in per_query) / len(per_query),
        "cross_family_top3_count": sum(row["cross_family_top3"] for row in per_query),
        "weak_cross_family_top3_count": sum(row["weak_cross_family_top3"] for row in per_query),
        "quality_counts": dict(aggregate_quality),
        "suppression_count": sum(len(row["suppressed"]) for row in per_query),
        "per_query": per_query,
    }


def format_actions(actions: list[tuple[str, float]]) -> str:
    return ", ".join(f"{action}:{score:.2f}" for action, score in actions)


def format_reasons(reasons: list[str]) -> str:
    return ", ".join(reasons) if reasons else "none"


def render_report(
    baseline: dict[str, Any],
    gated: dict[str, Any],
    fixture_path: Path,
    extra_fixture_paths: list[Path],
    quality_label_path: Path | None,
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    baseline_rows = {row["query_id"]: row for row in baseline["per_query"]}
    gated_rows = {row["query_id"]: row for row in gated["per_query"]}
    all_fixture_paths = [fixture_path, *extra_fixture_paths]

    lines = [
        "# ActionOutcomeReplay Experiment 005 Context Gate Results",
        "",
        f"Generated at: {now}",
        "Fixtures:",
        *[f"- `{path}`" for path in all_fixture_paths],
        f"- Quality labels: `{quality_label_path}`" if quality_label_path else "- Quality labels: none",
        "",
        "## Summary",
        "",
        "| Variant | Top-3 Hit Rate | Failed-Action Avoidance | Cross-Family Top3 | Weak Cross-Family Top3 | Labeled Top3 Quality | Gate Suppressions |",
        "| --- | ---: | ---: | ---: | ---: | --- | ---: |",
        "| {variant} | {hit:.2f} | {avoid:.2f} | {cross} | {weak} | {quality} | {suppressed} |".format(
            variant=baseline["variant"],
            hit=baseline["top3_hit_rate"],
            avoid=baseline["failed_avoidance_rate"],
            cross=baseline["cross_family_top3_count"],
            weak=baseline["weak_cross_family_top3_count"],
            quality=ablation.format_quality_counts(baseline["quality_counts"]),
            suppressed=0,
        ),
        "| {variant} | {hit:.2f} | {avoid:.2f} | {cross} | {weak} | {quality} | {suppressed} |".format(
            variant=gated["variant"],
            hit=gated["top3_hit_rate"],
            avoid=gated["failed_avoidance_rate"],
            cross=gated["cross_family_top3_count"],
            weak=gated["weak_cross_family_top3_count"],
            quality=ablation.format_quality_counts(gated["quality_counts"]),
            suppressed=gated["suppression_count"],
        ),
        "",
        "## Per-Query Comparison",
        "",
        "| Query | Full Guarded Top Action | Context Gated Top Action | Changed | Full Avoids Failed Repeat | Gated Avoids Failed Repeat | Gated Top Actions |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]

    for query_id in gated_rows:
        base = baseline_rows[query_id]
        row = gated_rows[query_id]
        lines.append(
            "| {query} | {base_top} | {gated_top} | {changed} | {base_avoid} | {gated_avoid} | {actions} |".format(
                query=query_id,
                base_top=base["top_action"],
                gated_top=row["top_action"],
                changed="yes" if base["top_action"] != row["top_action"] else "no",
                base_avoid="yes" if base["avoids_failed"] else "no",
                gated_avoid="yes" if row["avoids_failed"] else "no",
                actions=format_actions(row["actions"]),
            )
        )

    lines += [
        "",
        "## Context-Gated Top-3 Retrieval Details",
        "",
        "| Query | Rank | Candidate | Action | Outcome | Score | Quality | Context Multiplier | Context Score | Reasons |",
        "| --- | ---: | --- | --- | --- | ---: | --- | ---: | ---: | --- |",
    ]
    for row in gated["per_query"]:
        for candidate in row["retrieval_top3"]:
            lines.append(
                "| {query} | {rank} | {candidate} | {action} | {outcome} | {score:.3f} | {quality} | {multiplier:.2f} | {context:.3f} | {reasons} |".format(
                    query=row["query_id"],
                    rank=candidate["rank"],
                    candidate=candidate["episode_id"],
                    action=candidate["action"],
                    outcome=candidate["outcome"],
                    score=candidate["score"],
                    quality=candidate["quality_label"],
                    multiplier=candidate["context_multiplier"],
                    context=candidate["context_score"],
                    reasons=format_reasons(candidate["reasons"]),
                )
            )

    lines += [
        "",
        "## Suppressed Outcome Evidence",
        "",
        "| Query | Candidate | Action | Outcome | Raw Value | Gated Value | Multiplier | Context Score | Reasons |",
        "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- |",
    ]
    for row in gated["per_query"]:
        for suppressed in row["suppressed"]:
            lines.append(
                "| {query} | {candidate} | {action} | {outcome} | {raw:.2f} | {gated:.2f} | {multiplier:.2f} | {context:.3f} | {reasons} |".format(
                    query=row["query_id"],
                    candidate=suppressed["episode_id"],
                    action=suppressed["action"],
                    outcome=suppressed["outcome"],
                    raw=suppressed["raw_value"],
                    gated=suppressed["gated_value"],
                    multiplier=suppressed["multiplier"],
                    context=suppressed["context_score"],
                    reasons=format_reasons(suppressed["reasons"]),
                )
            )

    lines += [
        "",
        "## Interpretation",
        "",
        "The context gate should be judged by whether it improves failed-action",
        "avoidance under noisy same-family distractors without losing top-3",
        "successful-action coverage. Gate suppressions are expected: they show that",
        "the engine is refusing to reuse outcomes from incompatible contexts.",
        "",
        "## Next Steps",
        "",
        "1. Add a SQL-backed episode table and replay this comparison through ZeptoDB.",
        "2. Add a learned or calibrated gate after collecting real incident traces.",
        "3. Add operator-facing explanations for suppressed historical outcomes.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", type=Path, default=Path("docs/research/fixtures/action_outcome_episodes.json"))
    parser.add_argument("--extra-fixture", action="append", type=Path, default=[])
    parser.add_argument("--quality-labels", type=Path)
    parser.add_argument("--output", type=Path, default=Path("docs/research/results/action_outcome_context_gate_005.md"))
    parser.add_argument("--query-id", action="append", dest="query_ids")
    args = parser.parse_args()

    episodes = ablation.load_episodes(args.fixture, args.extra_fixture)
    quality_labels = ablation.load_quality_labels(args.quality_labels)
    by_id = {episode["episode_id"]: episode for episode in episodes}
    query_ids = args.query_ids or replay.DEFAULT_QUERY_IDS
    missing = [query_id for query_id in query_ids if query_id not in by_id]
    if missing:
        raise SystemExit(f"Unknown query ids: {', '.join(missing)}")

    queries = [by_id[query_id] for query_id in query_ids]
    baseline = ablation.evaluate_variant(episodes, queries, "full_guarded", set(), quality_labels)
    gated = evaluate_context_gated(episodes, queries, quality_labels)
    report = render_report(baseline, gated, args.fixture, args.extra_fixture, args.quality_labels)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)


if __name__ == "__main__":
    main()
