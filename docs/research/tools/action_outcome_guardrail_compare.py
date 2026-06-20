#!/usr/bin/env python3
"""Compare baseline and guarded Action-Outcome retrieval strategies."""

from __future__ import annotations

import argparse
import json
import math
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import action_outcome_replay as replay


def cross_family_penalty(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    if query["incident_type"] == candidate["incident_type"]:
        return 0.0
    topology = replay.topology_similarity(query, candidate)
    symptom = replay.symptom_similarity(query, candidate)
    if topology >= 0.70 and symptom >= 0.45:
        return 0.05
    if topology >= 0.55:
        return 0.12
    return 0.22


def guarded_component_scores(query: dict[str, Any], candidate: dict[str, Any]) -> dict[str, float]:
    components = replay.action_outcome_component_scores(query, candidate)
    components["cross_family_penalty"] = cross_family_penalty(query, candidate)
    components["guarded_total"] = components["total"] - components["cross_family_penalty"]
    return components


def guarded_score(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    return guarded_component_scores(query, candidate)["guarded_total"]


def robust_recommended_actions(
    query: dict[str, Any],
    rows: list[tuple[float, dict[str, Any]]],
    limit: int = 8,
) -> list[tuple[str, float]]:
    positive: dict[str, float] = defaultdict(float)
    negative: dict[str, float] = defaultdict(float)
    support: dict[str, float] = defaultdict(float)
    success_counts: dict[str, int] = defaultdict(int)
    failure_counts: dict[str, int] = defaultdict(int)

    for score, episode in rows[:limit]:
        penalty = cross_family_penalty(query, episode)
        if penalty >= 0.12:
            continue
        action = episode["action"]["action_class"]
        relevance = max(score, 0.0)
        if penalty > 0:
            relevance *= 0.5
        value = replay.outcome_value(episode)
        support[action] += relevance
        if value > 0:
            positive[action] += relevance * value
            success_counts[action] += 1
        elif value < 0:
            negative[action] += relevance * abs(value)
            failure_counts[action] += 1

    actions = set(support)
    ranked: list[tuple[str, float]] = []
    for action in actions:
        if support[action] == 0:
            continue
        score = (positive[action] - 1.25 * negative[action]) / support[action]
        score += 0.04 * math.log1p(success_counts[action])
        score -= 0.08 * failure_counts[action]
        ranked.append((action, score))

    ranked.sort(key=lambda item: item[1], reverse=True)
    return ranked


def ranked(
    episodes: list[dict[str, Any]],
    query: dict[str, Any],
    scorer,
) -> list[tuple[float, dict[str, Any]]]:
    return replay.ranked(episodes, query, scorer)


def top3_cross_family_stats(
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


def evaluate_query(episodes: list[dict[str, Any]], query: dict[str, Any]) -> dict[str, Any]:
    baseline_rows = ranked(episodes, query, replay.action_outcome_score)
    guarded_rows = ranked(episodes, query, guarded_score)
    baseline_actions = replay.recommended_actions(baseline_rows)
    guarded_actions = robust_recommended_actions(query, guarded_rows)
    useful_actions = replay.successful_actions_for_type(episodes, query["incident_type"])

    baseline_top_action = baseline_actions[0][0] if baseline_actions else "none"
    guarded_top_action = guarded_actions[0][0] if guarded_actions else "none"
    baseline_top3_actions = {action for action, _ in baseline_actions[:3]}
    guarded_top3_actions = {action for action, _ in guarded_actions[:3]}
    baseline_cross, baseline_weak_cross = top3_cross_family_stats(query, baseline_rows)
    guarded_cross, guarded_weak_cross = top3_cross_family_stats(query, guarded_rows)

    guarded_top3 = []
    for rank_index, (score, candidate) in enumerate(guarded_rows[:3], start=1):
        components = guarded_component_scores(query, candidate)
        guarded_top3.append(
            {
                "rank": rank_index,
                "episode_id": candidate["episode_id"],
                "incident_type": candidate["incident_type"],
                "action": candidate["action"]["action_class"],
                "outcome": replay.outcome_label(candidate),
                "score": score,
                "topology": components["topology"],
                "symptom": components["symptom"],
                "base_total": components["total"],
                "cross_family_penalty": components["cross_family_penalty"],
                "guarded_total": components["guarded_total"],
            }
        )

    return {
        "query_id": query["episode_id"],
        "incident_type": query["incident_type"],
        "true_action": query["action"]["action_class"],
        "true_outcome": replay.outcome_label(query),
        "useful_actions": sorted(useful_actions),
        "baseline_top_action": baseline_top_action,
        "guarded_top_action": guarded_top_action,
        "baseline_actions": baseline_actions[:3],
        "guarded_actions": guarded_actions[:3],
        "top_action_changed": baseline_top_action != guarded_top_action,
        "baseline_top3_hit": bool(useful_actions & baseline_top3_actions),
        "guarded_top3_hit": bool(useful_actions & guarded_top3_actions),
        "baseline_avoids_failed": not (
            replay.outcome_value(query) < 0 and baseline_top_action == query["action"]["action_class"]
        ),
        "guarded_avoids_failed": not (
            replay.outcome_value(query) < 0 and guarded_top_action == query["action"]["action_class"]
        ),
        "baseline_cross_family_top3": baseline_cross,
        "guarded_cross_family_top3": guarded_cross,
        "baseline_weak_cross_family_top3": baseline_weak_cross,
        "guarded_weak_cross_family_top3": guarded_weak_cross,
        "guarded_top3": guarded_top3,
    }


def format_actions(actions: list[tuple[str, float]]) -> str:
    return ", ".join(f"{action}:{score:.2f}" for action, score in actions)


def render_report(results: list[dict[str, Any]], fixture_path: Path) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    baseline_hit = sum(result["baseline_top3_hit"] for result in results) / len(results)
    guarded_hit = sum(result["guarded_top3_hit"] for result in results) / len(results)
    baseline_avoid = sum(result["baseline_avoids_failed"] for result in results) / len(results)
    guarded_avoid = sum(result["guarded_avoids_failed"] for result in results) / len(results)
    baseline_cross = sum(result["baseline_cross_family_top3"] for result in results)
    guarded_cross = sum(result["guarded_cross_family_top3"] for result in results)
    baseline_weak_cross = sum(result["baseline_weak_cross_family_top3"] for result in results)
    guarded_weak_cross = sum(result["guarded_weak_cross_family_top3"] for result in results)

    lines = [
        "# ActionOutcomeReplay Experiment 002 Guardrail Results",
        "",
        f"Generated at: {now}",
        f"Fixture: `{fixture_path}`",
        "",
        "## Summary",
        "",
        f"- Query episodes: {len(results)}",
        f"- Baseline top-3 successful action hit rate: {baseline_hit:.2f}",
        f"- Guarded top-3 successful action hit rate: {guarded_hit:.2f}",
        f"- Baseline failed-action avoidance rate: {baseline_avoid:.2f}",
        f"- Guarded failed-action avoidance rate: {guarded_avoid:.2f}",
        f"- Baseline cross-family top-3 candidates: {baseline_cross}",
        f"- Guarded cross-family top-3 candidates: {guarded_cross}",
        f"- Baseline weak cross-family top-3 candidates: {baseline_weak_cross}",
        f"- Guarded weak cross-family top-3 candidates: {guarded_weak_cross}",
        "",
        "## Recommendation Comparison",
        "",
        "| Query | True Action | True Outcome | Useful Actions | Baseline Recs | Guarded Recs | Top Action Changed | Baseline Cross Top3 | Guarded Cross Top3 |",
        "| --- | --- | --- | --- | --- | --- | --- | ---: | ---: |",
    ]

    for result in results:
        lines.append(
            "| {query_id} | {true_action} | {true_outcome} | {useful_actions} | "
            "{baseline_recs} | {guarded_recs} | {changed} | {baseline_cross} | {guarded_cross} |".format(
                query_id=result["query_id"],
                true_action=result["true_action"],
                true_outcome=result["true_outcome"],
                useful_actions=", ".join(result["useful_actions"]),
                baseline_recs=format_actions(result["baseline_actions"]),
                guarded_recs=format_actions(result["guarded_actions"]),
                changed="yes" if result["top_action_changed"] else "no",
                baseline_cross=result["baseline_cross_family_top3"],
                guarded_cross=result["guarded_cross_family_top3"],
            )
        )

    lines += [
        "",
        "## Guarded Top-3 Retrieval Details",
        "",
        "| Query | Rank | Episode | Candidate Family | Action | Outcome | Guarded Score | Base Total | Cross-Family Penalty | Symptom | Topology |",
        "| --- | ---: | --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: |",
    ]

    for result in results:
        for row in result["guarded_top3"]:
            lines.append(
                "| {query_id} | {rank} | {episode_id} | {family} | {action} | {outcome} | "
                "{guarded:.3f} | {base:.3f} | {penalty:.3f} | {symptom:.3f} | {topology:.3f} |".format(
                    query_id=result["query_id"],
                    rank=row["rank"],
                    episode_id=row["episode_id"],
                    family=row["incident_type"],
                    action=row["action"],
                    outcome=row["outcome"],
                    guarded=row["guarded_total"],
                    base=row["base_total"],
                    penalty=row["cross_family_penalty"],
                    symptom=row["symptom"],
                    topology=row["topology"],
                )
            )

    lines += [
        "",
        "## Interpretation",
        "",
        "Guarded V2 should be judged by whether it reduces weak cross-family retrieval",
        "without lowering successful-action hit rate or failed-action avoidance.",
        "If it changes top actions, inspect whether the change is safer or simply",
        "more conservative.",
        "Guarded recommendation scores are ranking utilities, not probabilities;",
        "their absolute scale is not comparable to baseline recommendation scores.",
        "",
        "## Next Steps",
        "",
        "1. Add an ablation report that removes one signal family at a time.",
        "2. Add noisy distractor episodes to test whether guardrails survive less-clean data.",
        "3. Map the fixture into ZeptoDB tables so replay can use SQL/time-window queries.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", type=Path, default=Path("docs/research/fixtures/action_outcome_episodes.json"))
    parser.add_argument("--output", type=Path, default=Path("docs/research/results/action_outcome_guardrail_002.md"))
    parser.add_argument("--query-id", action="append", dest="query_ids")
    args = parser.parse_args()

    data = json.loads(args.fixture.read_text())
    episodes = data["episodes"]
    by_id = {episode["episode_id"]: episode for episode in episodes}
    query_ids = args.query_ids or replay.DEFAULT_QUERY_IDS
    missing = [query_id for query_id in query_ids if query_id not in by_id]
    if missing:
        raise SystemExit(f"Unknown query ids: {', '.join(missing)}")

    results = [evaluate_query(episodes, by_id[query_id]) for query_id in query_ids]
    report = render_report(results, args.fixture)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)


if __name__ == "__main__":
    main()
