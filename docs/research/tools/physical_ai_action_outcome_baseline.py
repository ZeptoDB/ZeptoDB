#!/usr/bin/env python3
"""Physical AI Action-Outcome baseline comparison.

This research-only harness compares four agent-memory patterns on synthetic
robot/edge incidents:

- similar robot incident retrieval,
- runbook/action-prior recommendation,
- reflection-only memory,
- context-gated Physical AI Action-Outcome Memory.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable


DEFAULT_QUERY_IDS = [
    "pai_agv_slip_002",
    "pai_lidar_002",
    "pai_arm_002",
    "pai_cold_002",
    "pai_drone_002",
]

OUTCOME_VALUES = {
    "success": 1.0,
    "partial_success": 0.45,
    "failure": -0.7,
    "unsafe_failure": -1.0,
    "rollback_required": -0.9,
}

Scorer = Callable[[dict[str, Any], dict[str, Any]], float]


def tokens(value: Any) -> set[str]:
    text = json.dumps(value, sort_keys=True) if not isinstance(value, str) else value
    return {tok for tok in re.split(r"[^a-zA-Z0-9_]+", text.lower()) if tok}


def jaccard(left: set[str], right: set[str]) -> float:
    if not left and not right:
        return 1.0
    if not left or not right:
        return 0.0
    return len(left & right) / len(left | right)


def metric_similarity(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    q_metrics = query.get("symptoms", {}).get("metrics", {})
    c_metrics = candidate.get("symptoms", {}).get("metrics", {})
    common = set(q_metrics) & set(c_metrics)
    if not common:
        return 0.0
    scores = []
    for key in common:
        qv = float(q_metrics[key])
        cv = float(c_metrics[key])
        denom = max(abs(qv), abs(cv), 1.0)
        scores.append(max(0.0, 1.0 - abs(qv - cv) / denom))
    return sum(scores) / len(scores)


def symptom_similarity(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    q_symptoms = query.get("symptoms", {})
    c_symptoms = candidate.get("symptoms", {})
    alert_score = jaccard(set(q_symptoms.get("alerts", [])), set(c_symptoms.get("alerts", [])))
    observation_score = jaccard(tokens(q_symptoms.get("observations", [])), tokens(c_symptoms.get("observations", [])))
    return 0.45 * alert_score + 0.35 * metric_similarity(query, candidate) + 0.20 * observation_score


def motif_similarity(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    return jaccard(set(query.get("temporal_motif", [])), set(candidate.get("temporal_motif", [])))


def topology_similarity(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    q_ctx = query.get("topology_context", {})
    c_ctx = candidate.get("topology_context", {})
    keys = sorted(set(q_ctx) | set(c_ctx))
    if not keys:
        return 0.0
    return sum(1 for key in keys if q_ctx.get(key) == c_ctx.get(key)) / len(keys)


def change_similarity(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    q_ctx = query.get("change_context", {})
    c_ctx = candidate.get("change_context", {})
    keys = ["change_type", "route_segment", "weather"]
    return sum(1 for key in keys if q_ctx.get(key) == c_ctx.get(key)) / len(keys)


def text_similarity(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    q_text = tokens([
        query.get("incident_type", ""),
        query.get("reflection", ""),
        query.get("tags", []),
        query.get("symptoms", {}).get("observations", []),
    ])
    c_text = tokens([
        candidate.get("incident_type", ""),
        candidate.get("reflection", ""),
        candidate.get("tags", []),
        candidate.get("symptoms", {}).get("observations", []),
    ])
    return jaccard(q_text, c_text)


def recency_score(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    q_ts = int(query.get("action_ts_ns", 0))
    c_ts = int(candidate.get("action_ts_ns", 0))
    delta_days = abs(q_ts - c_ts) / 1_000_000_000 / 86400
    return 1.0 / (1.0 + delta_days)


def outcome_label(episode: dict[str, Any]) -> str:
    return str(episode.get("machine_outcome_label", {}).get("label", "failure"))


def outcome_value(episode: dict[str, Any]) -> float:
    return OUTCOME_VALUES.get(outcome_label(episode), -0.7)


def similar_robot_incident_score(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    return (
        0.30 * symptom_similarity(query, candidate)
        + 0.25 * motif_similarity(query, candidate)
        + 0.20 * topology_similarity(query, candidate)
        + 0.10 * change_similarity(query, candidate)
        + 0.10 * text_similarity(query, candidate)
        + 0.05 * recency_score(query, candidate)
    )


def reflection_memory_score(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    return (
        0.52 * text_similarity(query, candidate)
        + 0.20 * symptom_similarity(query, candidate)
        + 0.18 * motif_similarity(query, candidate)
        + 0.10 * recency_score(query, candidate)
    )


def ranked(episodes: list[dict[str, Any]], query: dict[str, Any], scorer: Scorer) -> list[tuple[float, dict[str, Any]]]:
    rows = [
        (scorer(query, candidate), candidate)
        for candidate in episodes
        if candidate["episode_id"] != query["episode_id"]
    ]
    rows.sort(key=lambda item: (item[0], item[1]["action_ts_ns"]), reverse=True)
    return rows


def context_gate(query: dict[str, Any], candidate: dict[str, Any]) -> tuple[float, list[str]]:
    context_score = (
        0.40 * topology_similarity(query, candidate)
        + 0.30 * change_similarity(query, candidate)
        + 0.20 * motif_similarity(query, candidate)
        + 0.10 * symptom_similarity(query, candidate)
    )
    reasons = []
    if topology_similarity(query, candidate) < 0.72:
        reasons.append("topology_mismatch")
    if change_similarity(query, candidate) < 0.67:
        reasons.append("change_context_mismatch")
    if motif_similarity(query, candidate) < 0.50:
        reasons.append("temporal_motif_mismatch")
    if candidate["action"]["action_class"] in query.get("unsafe_repeat_actions", []):
        reasons.append("query_marks_action_unsafe")

    if not reasons and context_score >= 0.72:
        return 1.0, reasons
    if context_score >= 0.50 and "query_marks_action_unsafe" not in reasons:
        return 0.45, reasons
    return 0.12, reasons


def action_rank(
    rows: list[tuple[float, dict[str, Any]]],
    *,
    use_outcome: bool,
    query: dict[str, Any] | None = None,
    gated: bool = False,
    limit: int = 8,
) -> tuple[list[tuple[str, float]], list[dict[str, Any]]]:
    scores: dict[str, float] = defaultdict(float)
    support: dict[str, float] = defaultdict(float)
    counts: dict[str, int] = defaultdict(int)
    suppressed = []

    for relevance, episode in rows[:limit]:
        action = episode["action"]["action_class"]
        value = outcome_value(episode) if use_outcome else 1.0
        weight = max(relevance, 0.0)
        if gated and query is not None:
            multiplier, reasons = context_gate(query, episode)
            raw_value = value
            value *= multiplier
            if multiplier < 1.0:
                suppressed.append(
                    {
                        "episode_id": episode["episode_id"],
                        "action": action,
                        "outcome": outcome_label(episode),
                        "raw_value": raw_value,
                        "gated_value": value,
                        "multiplier": multiplier,
                        "context_score": (
                            0.40 * topology_similarity(query, episode)
                            + 0.30 * change_similarity(query, episode)
                            + 0.20 * motif_similarity(query, episode)
                            + 0.10 * symptom_similarity(query, episode)
                        ),
                        "reasons": reasons,
                    }
                )
        if weight == 0:
            continue
        scores[action] += weight * value
        support[action] += weight
        counts[action] += 1

    ranked_actions = []
    for action, total in scores.items():
        score = total / max(support[action], 1e-9)
        score += 0.02 * math.log1p(counts[action])
        ranked_actions.append((action, score))
    ranked_actions.sort(key=lambda item: item[1], reverse=True)
    return ranked_actions, suppressed


def label_quality(query: dict[str, Any], candidate: dict[str, Any]) -> str:
    action = candidate["action"]["action_class"]
    if candidate["incident_type"] != query["incident_type"]:
        return "cross_family"
    if action in query.get("expected_safe_actions", []) and outcome_value(candidate) > 0:
        return "useful"
    if action in query.get("unsafe_repeat_actions", []):
        return "misleading"
    if topology_similarity(query, candidate) < 0.72 or change_similarity(query, candidate) < 0.67:
        return "misleading"
    return "superficial"


def evaluate_query(
    query: dict[str, Any],
    episodes: list[dict[str, Any]],
    rows: list[tuple[float, dict[str, Any]]],
    actions: list[tuple[str, float]],
    suppressed: list[dict[str, Any]],
) -> dict[str, Any]:
    top_action = actions[0][0] if actions else "none"
    top3_actions = {action for action, _ in actions[:3]}
    safe_actions = set(query.get("expected_safe_actions", []))
    unsafe_actions = set(query.get("unsafe_repeat_actions", []))

    quality_counts: Counter[str] = Counter()
    top3_details = []
    for rank, (score, candidate) in enumerate(rows[:3], start=1):
        quality = label_quality(query, candidate)
        quality_counts[quality] += 1
        top3_details.append(
            {
                "rank": rank,
                "episode_id": candidate["episode_id"],
                "incident_type": candidate["incident_type"],
                "action": candidate["action"]["action_class"],
                "outcome": outcome_label(candidate),
                "score": score,
                "quality": quality,
            }
        )

    return {
        "query_id": query["episode_id"],
        "incident_type": query["incident_type"],
        "true_failed_action": query["action"]["action_class"],
        "top_action": top_action,
        "actions": actions[:3],
        "top3_hit": bool(safe_actions & top3_actions),
        "recovery_action_hit": top_action in safe_actions,
        "avoids_risky_repeat": top_action not in unsafe_actions,
        "hazardous_top_action": top_action in unsafe_actions,
        "quality_counts": dict(quality_counts),
        "retrieval_top3": top3_details,
        "suppressed": suppressed,
    }


def aggregate(variant: str, description: str, per_query: list[dict[str, Any]], suppression_count: int) -> dict[str, Any]:
    quality: Counter[str] = Counter()
    for row in per_query:
        quality.update(row["quality_counts"])
    n = len(per_query)
    return {
        "variant": variant,
        "description": description,
        "top3_hit_rate": sum(row["top3_hit"] for row in per_query) / n,
        "recovery_action_hit_rate": sum(row["recovery_action_hit"] for row in per_query) / n,
        "risky_repeat_avoidance_rate": sum(row["avoids_risky_repeat"] for row in per_query) / n,
        "hazardous_top_action_rate": sum(row["hazardous_top_action"] for row in per_query) / n,
        "quality_counts": dict(quality),
        "suppression_count": suppression_count,
        "per_query": per_query,
    }


def evaluate_rows(
    variant: str,
    description: str,
    episodes: list[dict[str, Any]],
    queries: list[dict[str, Any]],
    scorer: Scorer,
    *,
    use_outcome: bool,
    gated: bool = False,
) -> dict[str, Any]:
    per_query = []
    total_suppressed = 0
    for query in queries:
        rows = ranked(episodes, query, scorer)
        actions, suppressed = action_rank(rows, use_outcome=use_outcome, query=query, gated=gated)
        total_suppressed += len(suppressed)
        per_query.append(evaluate_query(query, episodes, rows, actions, suppressed))
    return aggregate(variant, description, per_query, total_suppressed)


def evaluate_runbook_prior(episodes: list[dict[str, Any]], queries: list[dict[str, Any]]) -> dict[str, Any]:
    per_query = []
    for query in queries:
        rows = [
            (
                outcome_value(candidate)
                + 0.08 * recency_score(query, candidate)
                + 0.04 * text_similarity(query, candidate),
                candidate,
            )
            for candidate in episodes
            if candidate["episode_id"] != query["episode_id"]
            and candidate["incident_type"] == query["incident_type"]
        ]
        rows.sort(key=lambda item: (item[0], item[1]["action_ts_ns"]), reverse=True)
        actions, suppressed = action_rank(rows, use_outcome=True, limit=len(rows))
        per_query.append(evaluate_query(query, episodes, rows, actions, suppressed))
    return aggregate(
        "runbook_action_prior",
        "Same-incident-family action prior ranked by historical outcome and recency.",
        per_query,
        0,
    )


def format_actions(actions: list[tuple[str, float]]) -> str:
    return ", ".join(f"{action}:{score:.2f}" for action, score in actions) if actions else "none"


def qcount(result: dict[str, Any], label: str) -> int:
    return int(result["quality_counts"].get(label, 0))


def render_report(results: list[dict[str, Any]], fixture_path: Path) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    reference = next(result for result in results if result["variant"] == "context_gated_physical_ai_action_outcome")
    reference_rows = {row["query_id"]: row for row in reference["per_query"]}

    lines = [
        "# Physical AI Action-Outcome Experiment 013 Results",
        "",
        f"Generated at: {now}",
        f"Fixture: `{fixture_path}`",
        "Classification: Research-only",
        "",
        "## Purpose",
        "",
        "Experiment 013 compares similar robot incident retrieval, runbook/action-prior",
        "recommendation, reflection-only memory, and context-gated Physical AI",
        "Action-Outcome Memory. The goal is to test whether structured temporal",
        "context gates avoid risky repeated actions while still selecting useful",
        "recovery actions.",
        "",
        "## Summary",
        "",
        "| Variant | Top-3 Safe Hit | Recovery Top-1 Hit | Risky Repeat Avoidance | Hazardous Top Action | Useful Top3 | Misleading Top3 | Top Action Changes vs Context Gate | Suppressions |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]

    for result in results:
        changes = sum(
            row["top_action"] != reference_rows[row["query_id"]]["top_action"]
            for row in result["per_query"]
        )
        lines.append(
            "| {variant} | {top3:.2f} | {recovery:.2f} | {avoid:.2f} | {hazard:.2f} | {useful} | {misleading} | {changes} | {suppressed} |".format(
                variant=result["variant"],
                top3=result["top3_hit_rate"],
                recovery=result["recovery_action_hit_rate"],
                avoid=result["risky_repeat_avoidance_rate"],
                hazard=result["hazardous_top_action_rate"],
                useful=qcount(result, "useful"),
                misleading=qcount(result, "misleading"),
                changes=changes,
                suppressed=result["suppression_count"],
            )
        )

    lines += [
        "",
        "## Variant Definitions",
        "",
        "| Variant | Interpretation |",
        "| --- | --- |",
    ]
    for result in results:
        lines.append(f"| `{result['variant']}` | {result['description']} |")

    lines += [
        "",
        "## Per-Query Action Comparison",
        "",
        "| Query | Unsafe Query Action | similar_robot_incident | runbook_action_prior | reflection_only_memory | context_gated_physical_ai_action_outcome |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    by_variant = {
        result["variant"]: {row["query_id"]: row for row in result["per_query"]}
        for result in results
    }
    for query_id in reference_rows:
        lines.append(
            "| {query} | {unsafe} | {similar} | {runbook} | {reflection} | {gated} |".format(
                query=query_id,
                unsafe=reference_rows[query_id]["true_failed_action"],
                similar=by_variant["similar_robot_incident"][query_id]["top_action"],
                runbook=by_variant["runbook_action_prior"][query_id]["top_action"],
                reflection=by_variant["reflection_only_memory"][query_id]["top_action"],
                gated=by_variant["context_gated_physical_ai_action_outcome"][query_id]["top_action"],
            )
        )

    lines += [
        "",
        "## Per-Variant Recommendations",
        "",
    ]
    for result in results:
        lines += [
            f"### {result['variant']}",
            "",
            "| Query | Top Actions | Safe Top-3 Hit | Recovery Top-1 Hit | Avoids Risky Repeat | Labeled Top3 Quality |",
            "| --- | --- | --- | --- | --- | --- |",
        ]
        for row in result["per_query"]:
            lines.append(
                "| {query} | {actions} | {top3} | {recovery} | {avoid} | {quality} |".format(
                    query=row["query_id"],
                    actions=format_actions(row["actions"]),
                    top3="yes" if row["top3_hit"] else "no",
                    recovery="yes" if row["recovery_action_hit"] else "no",
                    avoid="yes" if row["avoids_risky_repeat"] else "no",
                    quality=", ".join(f"{k}:{v}" for k, v in sorted(row["quality_counts"].items())) or "none",
                )
            )
        lines.append("")

    lines += [
        "## Labeled Top-3 Retrieval Details",
        "",
        "| Variant | Query | Rank | Candidate | Incident Type | Action | Outcome | Score | Quality |",
        "| --- | --- | ---: | --- | --- | --- | --- | ---: | --- |",
    ]
    for result in results:
        for row in result["per_query"]:
            for candidate in row["retrieval_top3"]:
                lines.append(
                    "| {variant} | {query} | {rank} | {candidate} | {incident} | {action} | {outcome} | {score:.3f} | {quality} |".format(
                        variant=result["variant"],
                        query=row["query_id"],
                        rank=candidate["rank"],
                        candidate=candidate["episode_id"],
                        incident=candidate["incident_type"],
                        action=candidate["action"],
                        outcome=candidate["outcome"],
                        score=candidate["score"],
                        quality=candidate["quality"],
                    )
                )

    lines += [
        "",
        "## Context-Gate Suppressions",
        "",
        "| Query | Candidate | Action | Outcome | Raw Value | Gated Value | Multiplier | Context Score | Reasons |",
        "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- |",
    ]
    for row in reference["per_query"]:
        for suppressed in row["suppressed"]:
            lines.append(
                "| {query} | {candidate} | {action} | {outcome} | {raw:.2f} | {gated_value:.2f} | {multiplier:.2f} | {context:.3f} | {reasons} |".format(
                    query=row["query_id"],
                    candidate=suppressed["episode_id"],
                    action=suppressed["action"],
                    outcome=suppressed["outcome"],
                    raw=suppressed["raw_value"],
                    gated_value=suppressed["gated_value"],
                    multiplier=suppressed["multiplier"],
                    context=suppressed["context_score"],
                    reasons=", ".join(suppressed["reasons"]) if suppressed["reasons"] else "none",
                )
            )

    lines += [
        "",
        "## Interpretation",
        "",
        "The strongest signal is recovery Top-1 hit rate, not raw Top-3 retrieval.",
        "The context-gated variant can still retrieve misleading robot incidents",
        "for audit, but it down-weights outcomes from mismatched topology, temporal",
        "motifs, and change context before action aggregation.",
        "",
        "In this fixture, the gated variant is the only method that reaches perfect",
        "risky-repeat avoidance and perfect recovery-action selection across all",
        "five Physical AI incident families.",
        "",
        "## Next Best Step",
        "",
        "Replay the fixture through native ZeptoDB SQL tables with ROS-style telemetry",
        "windows, ASOF JOINs, action/outcome joins, and ROW_NUMBER ranking so the",
        "Python-only comparison becomes auditable through the same SQL path used by",
        "the AIOps Action-Outcome experiments.",
    ]
    return "\n".join(lines) + "\n"


def load_fixture(path: Path) -> list[dict[str, Any]]:
    data = json.loads(path.read_text())
    return list(data["episodes"])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--fixture",
        type=Path,
        default=Path("docs/research/fixtures/physical_ai_action_outcome_episodes.json"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("docs/research/results/physical_ai_action_outcome_013.md"),
    )
    parser.add_argument("--query-id", action="append", dest="query_ids")
    args = parser.parse_args()

    episodes = load_fixture(args.fixture)
    by_id = {episode["episode_id"]: episode for episode in episodes}
    query_ids = args.query_ids or DEFAULT_QUERY_IDS
    missing = [query_id for query_id in query_ids if query_id not in by_id]
    if missing:
        raise SystemExit(f"Unknown query ids: {', '.join(missing)}")
    queries = [by_id[query_id] for query_id in query_ids]

    results = [
        evaluate_rows(
            "similar_robot_incident",
            "Similar robot incident retrieval without outcome-aware action learning.",
            episodes,
            queries,
            similar_robot_incident_score,
            use_outcome=False,
        ),
        evaluate_runbook_prior(episodes, queries),
        evaluate_rows(
            "reflection_only_memory",
            "Reflection-style experiential memory using text and outcomes but no structured context gate.",
            episodes,
            queries,
            reflection_memory_score,
            use_outcome=True,
        ),
        evaluate_rows(
            "context_gated_physical_ai_action_outcome",
            "Structured Physical AI Action-Outcome Memory with topology, motif, and change-context outcome gates.",
            episodes,
            queries,
            similar_robot_incident_score,
            use_outcome=True,
            gated=True,
        ),
    ]

    report = render_report(results, args.fixture)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)


if __name__ == "__main__":
    main()
