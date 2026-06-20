#!/usr/bin/env python3
"""Deterministic replay evaluator for Action-Outcome Memory research.

The evaluator intentionally uses transparent fixture-level heuristics before any
embedding or LLM layer is introduced. Its job is to answer the first research
question: does action-outcome memory change the recommended action compared with
keyword, text-only, or time-series-only retrieval?
"""

from __future__ import annotations

import argparse
import json
import math
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


DEFAULT_QUERY_IDS = [
    "aoe_checkout_002",
    "aoe_payment_002",
    "aoe_inventory_002",
    "aoe_cache_002",
    "aoe_queue_003",
    "aoe_search_003",
]

OUTCOME_VALUE = {
    "success": 1.0,
    "partial_success": 0.45,
    "failure": -0.7,
    "rollback_required": -0.9,
    "unsafe": -1.0,
    "insufficient_evidence": -0.2,
}

RISK_PENALTY = {
    "low": 0.0,
    "medium": 0.04,
    "high": 0.10,
    "critical": 0.18,
}


def tokens(value: Any) -> set[str]:
    if value is None:
        return set()
    if isinstance(value, str):
        raw = value.replace(":", " ").replace("-", " ").replace("_", " ")
        return {part.lower() for part in raw.split() if part}
    if isinstance(value, list):
        result: set[str] = set()
        for item in value:
            result |= tokens(item)
        return result
    if isinstance(value, dict):
        result: set[str] = set()
        for key, item in value.items():
            result |= tokens(str(key))
            result |= tokens(item)
        return result
    return tokens(str(value))


def jaccard(left: set[str], right: set[str]) -> float:
    if not left and not right:
        return 1.0
    if not left or not right:
        return 0.0
    return len(left & right) / len(left | right)


def outcome_label(episode: dict[str, Any]) -> str:
    return episode["human_outcome_label"]["label"]


def outcome_value(episode: dict[str, Any]) -> float:
    return OUTCOME_VALUE.get(outcome_label(episode), 0.0)


def parse_ts(value: str) -> datetime:
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def recency_score(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    query_ts = parse_ts(query["action_ts"])
    candidate_ts = parse_ts(candidate["action_ts"])
    days = abs((query_ts - candidate_ts).total_seconds()) / 86_400
    return math.exp(-days / 14.0)


def symptom_similarity(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    return jaccard(tokens(query["symptoms"]), tokens(candidate["symptoms"]))


def temporal_motif_similarity(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    query_metrics = query["symptoms"].get("metrics", {})
    candidate_metrics = candidate["symptoms"].get("metrics", {})
    key_score = jaccard(set(query_metrics), set(candidate_metrics))
    if not query_metrics or not candidate_metrics:
        return key_score

    shared = set(query_metrics) & set(candidate_metrics)
    if not shared:
        return key_score

    severity_scores: list[float] = []
    for key in shared:
        left = float(query_metrics[key])
        right = float(candidate_metrics[key])
        denom = max(abs(left), abs(right), 1.0)
        severity_scores.append(max(0.0, 1.0 - abs(left - right) / denom))
    return 0.5 * key_score + 0.5 * (sum(severity_scores) / len(severity_scores))


def topology_similarity(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    query_tokens = tokens(query["entity_refs"]) | tokens(query["topology_context"]) | {query["service"]}
    candidate_tokens = (
        tokens(candidate["entity_refs"]) | tokens(candidate["topology_context"]) | {candidate["service"]}
    )
    return jaccard(query_tokens, candidate_tokens)


def change_similarity(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    query_change = query["change_context"]
    candidate_change = candidate["change_context"]
    score = 0.0
    if query_change.get("change_type") == candidate_change.get("change_type"):
        score += 0.55
    for key in ("deploy_id", "config_id", "flag_id"):
        if query_change.get(key) and query_change.get(key) == candidate_change.get(key):
            score += 0.30
        elif query_change.get(key) and candidate_change.get(key):
            score += 0.12
    return min(score, 1.0)


def text_similarity(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    query_text = tokens(query["symptoms"]) | tokens(query["candidate_root_causes"])
    candidate_text = (
        tokens(candidate["reflection"])
        | tokens(candidate["human_outcome_label"])
        | tokens(candidate["symptoms"])
        | tokens(candidate["candidate_root_causes"])
    )
    return jaccard(query_text, candidate_text)


def risk_penalty(candidate: dict[str, Any]) -> float:
    return RISK_PENALTY.get(candidate["policy_decision"].get("risk_tier"), 0.0)


def keyword_score(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    query_tokens = {query["service"], query["incident_type"]} | tokens(query["symptoms"].get("alerts", [])) | set(
        query["tags"]
    )
    candidate_tokens = (
        {candidate["service"], candidate["incident_type"]}
        | tokens(candidate["symptoms"].get("alerts", []))
        | set(candidate["tags"])
    )
    return jaccard(query_tokens, candidate_tokens)


def time_series_score(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    return (
        0.30 * symptom_similarity(query, candidate)
        + 0.30 * temporal_motif_similarity(query, candidate)
        + 0.20 * topology_similarity(query, candidate)
        + 0.20 * change_similarity(query, candidate)
    )


def action_outcome_component_scores(query: dict[str, Any], candidate: dict[str, Any]) -> dict[str, float]:
    same_action = query["action"]["action_class"] == candidate["action"]["action_class"]
    action_outcome = 0.5 + (outcome_value(candidate) / 2.0)
    if same_action and outcome_value(candidate) < 0:
        action_outcome -= 0.25
    if same_action and outcome_value(candidate) > 0:
        action_outcome += 0.10

    components = {
        "symptom": symptom_similarity(query, candidate),
        "temporal_motif": temporal_motif_similarity(query, candidate),
        "topology": topology_similarity(query, candidate),
        "change": change_similarity(query, candidate),
        "action_outcome": max(0.0, min(1.0, action_outcome)),
        "postmortem_text": text_similarity(query, candidate),
        "recency": recency_score(query, candidate),
        "risk_penalty": risk_penalty(candidate),
    }
    components["total"] = (
        0.20 * components["symptom"]
        + 0.20 * components["temporal_motif"]
        + 0.15 * components["topology"]
        + 0.15 * components["change"]
        + 0.15 * components["action_outcome"]
        + 0.10 * components["postmortem_text"]
        + 0.05 * components["recency"]
        - components["risk_penalty"]
    )
    return components


def action_outcome_score(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    return action_outcome_component_scores(query, candidate)["total"]


def ranked(
    episodes: list[dict[str, Any]],
    query: dict[str, Any],
    scorer,
) -> list[tuple[float, dict[str, Any]]]:
    rows = []
    for candidate in episodes:
        if candidate["episode_id"] == query["episode_id"]:
            continue
        rows.append((scorer(query, candidate), candidate))
    rows.sort(key=lambda item: (item[0], item[1]["action_ts"]), reverse=True)
    return rows


def successful_actions_for_type(episodes: list[dict[str, Any]], incident_type: str) -> set[str]:
    actions = set()
    for episode in episodes:
        if episode["incident_type"] != incident_type:
            continue
        if outcome_value(episode) > 0:
            actions.add(episode["action"]["action_class"])
    return actions


def recommended_actions(rows: list[tuple[float, dict[str, Any]]], limit: int = 5) -> list[tuple[str, float]]:
    action_scores: dict[str, float] = defaultdict(float)
    action_counts: dict[str, int] = defaultdict(int)
    for score, episode in rows[:limit]:
        action = episode["action"]["action_class"]
        action_scores[action] += score * outcome_value(episode)
        action_counts[action] += 1
    normalized = [
        (action, action_scores[action] / max(action_counts[action], 1))
        for action in action_scores
    ]
    normalized.sort(key=lambda item: item[1], reverse=True)
    return normalized


def evaluate_query(episodes: list[dict[str, Any]], query: dict[str, Any]) -> dict[str, Any]:
    keyword_rows = ranked(episodes, query, keyword_score)
    text_rows = ranked(episodes, query, text_similarity)
    ts_rows = ranked(episodes, query, time_series_score)
    ao_rows = ranked(episodes, query, action_outcome_score)
    keyword_actions = recommended_actions(keyword_rows)
    text_actions = recommended_actions(text_rows)
    ts_actions = recommended_actions(ts_rows)
    ao_actions = recommended_actions(ao_rows)
    useful_actions = successful_actions_for_type(episodes, query["incident_type"])
    top_action = ao_actions[0][0] if ao_actions else "none"
    top3_actions = {action for action, _ in ao_actions[:3]}
    top_ao_candidate = ao_rows[0][1]
    top3_breakdowns = []
    for rank_index, (score, candidate) in enumerate(ao_rows[:3], start=1):
        top3_breakdowns.append(
            {
                "rank": rank_index,
                "episode_id": candidate["episode_id"],
                "action": candidate["action"]["action_class"],
                "outcome": outcome_label(candidate),
                "score": score,
                "components": action_outcome_component_scores(query, candidate),
            }
        )

    return {
        "query_id": query["episode_id"],
        "incident_type": query["incident_type"],
        "true_action": query["action"]["action_class"],
        "true_outcome": outcome_label(query),
        "useful_actions": sorted(useful_actions),
        "keyword_top1": keyword_rows[0][1]["episode_id"],
        "text_top1": text_rows[0][1]["episode_id"],
        "timeseries_top1": ts_rows[0][1]["episode_id"],
        "action_outcome_top1": ao_rows[0][1]["episode_id"],
        "keyword_recommended_actions": keyword_actions[:3],
        "text_recommended_actions": text_actions[:3],
        "timeseries_recommended_actions": ts_actions[:3],
        "recommended_actions": ao_actions[:3],
        "action_outcome_top1_action": top_ao_candidate["action"]["action_class"],
        "action_outcome_top1_outcome": outcome_label(top_ao_candidate),
        "action_outcome_top1_breakdown": action_outcome_component_scores(query, top_ao_candidate),
        "action_outcome_top3_breakdowns": top3_breakdowns,
        "top3_successful_action_hit": bool(useful_actions & top3_actions),
        "avoids_repeating_failed_action": not (
            outcome_value(query) < 0 and top_action == query["action"]["action_class"]
        ),
    }


def render_report(results: list[dict[str, Any]], fixture_path: Path) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    hit_rate = sum(result["top3_successful_action_hit"] for result in results) / len(results)
    avoid_rate = sum(result["avoids_repeating_failed_action"] for result in results) / len(results)

    lines = [
        "# ActionOutcomeReplay Experiment 001 Results",
        "",
        f"Generated at: {now}",
        f"Fixture: `{fixture_path}`",
        "",
        "## Summary",
        "",
        f"- Query episodes: {len(results)}",
        f"- Top-3 successful action hit rate: {hit_rate:.2f}",
        f"- Failed-action avoidance rate: {avoid_rate:.2f}",
        "",
        "## Query Results",
        "",
        "| Query | Incident Type | True Action | True Outcome | Useful Actions | Keyword Recs | Text Recs | Time-Series Recs | Action-Outcome Recs | Hit | Avoid Failed Repeat |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]

    for result in results:
        keyword_recs = ", ".join(f"{action}:{score:.2f}" for action, score in result["keyword_recommended_actions"])
        text_recs = ", ".join(f"{action}:{score:.2f}" for action, score in result["text_recommended_actions"])
        ts_recs = ", ".join(f"{action}:{score:.2f}" for action, score in result["timeseries_recommended_actions"])
        ao_recs = ", ".join(f"{action}:{score:.2f}" for action, score in result["recommended_actions"])
        lines.append(
            "| {query_id} | {incident_type} | {true_action} | {true_outcome} | {useful_actions} | "
            "{keyword_recs} | {text_recs} | {ts_recs} | {ao_recs} | {hit} | {avoid} |".format(
                query_id=result["query_id"],
                incident_type=result["incident_type"],
                true_action=result["true_action"],
                true_outcome=result["true_outcome"],
                useful_actions=", ".join(result["useful_actions"]),
                keyword_recs=keyword_recs,
                text_recs=text_recs,
                ts_recs=ts_recs,
                ao_recs=ao_recs,
                hit="yes" if result["top3_successful_action_hit"] else "no",
                avoid="yes" if result["avoids_repeating_failed_action"] else "no",
            )
        )

    lines += [
        "",
        "## Top-3 Action-Outcome Retrieval Breakdowns",
        "",
        "| Query | Rank | Episode | Action | Outcome | Total | Symptom | Temporal | Topology | Change | Action Outcome | Text | Recency | Risk Penalty |",
        "| --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]

    for result in results:
        for row in result["action_outcome_top3_breakdowns"]:
            breakdown = row["components"]
            lines.append(
                "| {query_id} | {rank} | {episode_id} | {action} | {outcome} | {total:.3f} | "
                "{symptom:.3f} | {temporal:.3f} | {topology:.3f} | {change:.3f} | "
                "{action_outcome:.3f} | {text:.3f} | {recency:.3f} | {risk:.3f} |".format(
                    query_id=result["query_id"],
                    rank=row["rank"],
                    episode_id=row["episode_id"],
                    action=row["action"],
                    outcome=row["outcome"],
                    total=breakdown["total"],
                    symptom=breakdown["symptom"],
                    temporal=breakdown["temporal_motif"],
                    topology=breakdown["topology"],
                    change=breakdown["change"],
                    action_outcome=breakdown["action_outcome"],
                    text=breakdown["postmortem_text"],
                    recency=breakdown["recency"],
                    risk=breakdown["risk_penalty"],
                )
            )

    lines += [
        "",
        "## Interpretation",
        "",
        "This first result is a fixture-level sanity check, not a benchmark claim.",
        "A useful next step is to inspect cases where the recommended action is",
        "generic or where retrieval is dominated by incident type instead of",
        "specific symptoms, topology, change context, and recovery outcomes.",
        "",
        "## Next Steps",
        "",
        "1. Tune action aggregation so one weak positive candidate does not dominate several strong negative cases.",
        "2. Add noisy distractor episodes to reduce fixture cleanliness bias.",
        "3. Map fixture rows into ZeptoDB tables for replay through SQL.",
        "4. Add a result-diff report that shows how recommendations change when each signal family is removed.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", type=Path, default=Path("docs/research/fixtures/action_outcome_episodes.json"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--query-id", action="append", dest="query_ids")
    args = parser.parse_args()

    data = json.loads(args.fixture.read_text())
    episodes = data["episodes"]
    by_id = {episode["episode_id"]: episode for episode in episodes}
    query_ids = args.query_ids or DEFAULT_QUERY_IDS
    missing = [query_id for query_id in query_ids if query_id not in by_id]
    if missing:
        raise SystemExit(f"Unknown query ids: {', '.join(missing)}")

    results = [evaluate_query(episodes, by_id[query_id]) for query_id in query_ids]
    report = render_report(results, args.fixture)

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report)
    else:
        print(report, end="")


if __name__ == "__main__":
    main()
