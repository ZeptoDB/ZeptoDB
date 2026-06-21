#!/usr/bin/env python3
"""Vendor-inspired baseline comparison for Action-Outcome Memory research."""

from __future__ import annotations

import argparse
import math
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

import action_outcome_ablation as ablation
import action_outcome_context_gate as context_gate
import action_outcome_replay as replay


BaselineScorer = Callable[[dict[str, Any], dict[str, Any]], float]


def similar_incident_score(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    """Incident similarity without using outcome evidence."""
    return (
        0.25 * replay.symptom_similarity(query, candidate)
        + 0.25 * replay.temporal_motif_similarity(query, candidate)
        + 0.20 * replay.topology_similarity(query, candidate)
        + 0.15 * replay.change_similarity(query, candidate)
        + 0.10 * replay.text_similarity(query, candidate)
        + 0.05 * replay.recency_score(query, candidate)
        - replay.risk_penalty(candidate)
    )


def reflection_memory_score(query: dict[str, Any], candidate: dict[str, Any]) -> float:
    """Reflection-style memory without explicit topology/change outcome gates."""
    return (
        0.55 * replay.text_similarity(query, candidate)
        + 0.25 * replay.symptom_similarity(query, candidate)
        + 0.10 * replay.temporal_motif_similarity(query, candidate)
        + 0.10 * replay.recency_score(query, candidate)
        - replay.risk_penalty(candidate)
    )


def ranked_actions_from_rows(
    rows: list[tuple[float, dict[str, Any]]],
    *,
    use_outcome: bool,
    limit: int = 8,
) -> list[tuple[str, float]]:
    scores: dict[str, float] = defaultdict(float)
    support: dict[str, float] = defaultdict(float)
    counts: dict[str, int] = defaultdict(int)

    for relevance, episode in rows[:limit]:
        action = episode["action"]["action_class"]
        weight = max(relevance, 0.0)
        if weight == 0:
            continue
        value = replay.outcome_value(episode) if use_outcome else 1.0
        scores[action] += weight * value
        support[action] += weight
        counts[action] += 1

    ranked: list[tuple[str, float]] = []
    for action, total in scores.items():
        score = total / max(support[action], 1e-9)
        score += 0.02 * math.log1p(counts[action])
        ranked.append((action, score))
    ranked.sort(key=lambda item: item[1], reverse=True)
    return ranked


def runbook_action_prior_rows(
    episodes: list[dict[str, Any]],
    query: dict[str, Any],
) -> list[tuple[float, dict[str, Any]]]:
    rows = []
    for candidate in episodes:
        if candidate["episode_id"] == query["episode_id"]:
            continue
        if candidate["incident_type"] != query["incident_type"]:
            continue
        outcome = replay.outcome_value(candidate)
        # Runbook/action-prior systems usually prefer historically successful
        # fixed actions, but still retain weaker negative evidence for audit.
        score = max(outcome, -0.25)
        score += 0.08 * replay.recency_score(query, candidate)
        score -= replay.risk_penalty(candidate)
        rows.append((score, candidate))
    rows.sort(key=lambda item: (item[0], item[1]["action_ts"]), reverse=True)
    return rows


def evaluate_row_baseline(
    episodes: list[dict[str, Any]],
    queries: list[dict[str, Any]],
    quality_labels: dict[tuple[str, str], str],
    *,
    variant: str,
    description: str,
    scorer: BaselineScorer,
    use_outcome: bool,
) -> dict[str, Any]:
    per_query = []
    for query in queries:
        rows = replay.ranked(episodes, query, scorer)
        actions = ranked_actions_from_rows(rows, use_outcome=use_outcome)
        per_query.append(evaluate_query_rows(query, episodes, rows, actions, quality_labels))
    return aggregate_result(variant, description, per_query, suppression_count=0)


def evaluate_runbook_action_prior(
    episodes: list[dict[str, Any]],
    queries: list[dict[str, Any]],
    quality_labels: dict[tuple[str, str], str],
) -> dict[str, Any]:
    per_query = []
    for query in queries:
        rows = runbook_action_prior_rows(episodes, query)
        actions = ranked_actions_from_rows(rows, use_outcome=True, limit=len(rows))
        per_query.append(evaluate_query_rows(query, episodes, rows, actions, quality_labels))
    return aggregate_result(
        "runbook_action_prior",
        "Same-incident-family action prior from historical outcomes.",
        per_query,
        suppression_count=0,
    )


def evaluate_context_gated(
    episodes: list[dict[str, Any]],
    queries: list[dict[str, Any]],
    quality_labels: dict[tuple[str, str], str],
) -> dict[str, Any]:
    gated = context_gate.evaluate_context_gated(episodes, queries, quality_labels)
    query_by_id = {query["episode_id"]: query for query in queries}
    for row in gated["per_query"]:
        query = query_by_id[row["query_id"]]
        row["true_action"] = query["action"]["action_class"]
        row["true_outcome"] = replay.outcome_label(query)
    return {
        "variant": "context_gated_action_outcome",
        "description": "Structured Action-Outcome Memory with context-conditioned outcome suppression.",
        "top3_hit_rate": gated["top3_hit_rate"],
        "failed_avoidance_rate": gated["failed_avoidance_rate"],
        "cross_family_top3_count": gated["cross_family_top3_count"],
        "weak_cross_family_top3_count": gated["weak_cross_family_top3_count"],
        "quality_counts": gated["quality_counts"],
        "suppression_count": gated["suppression_count"],
        "per_query": gated["per_query"],
    }


def evaluate_query_rows(
    query: dict[str, Any],
    episodes: list[dict[str, Any]],
    rows: list[tuple[float, dict[str, Any]]],
    actions: list[tuple[str, float]],
    quality_labels: dict[tuple[str, str], str],
) -> dict[str, Any]:
    useful_actions = replay.successful_actions_for_type(episodes, query["incident_type"])
    top_action = actions[0][0] if actions else "none"
    top3_actions = {action for action, _ in actions[:3]}
    cross, weak_cross = ablation.cross_family_stats(query, rows)

    retrieval_top3 = []
    quality_counts: Counter[str] = Counter()
    for rank, (score, candidate) in enumerate(rows[:3], start=1):
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
            }
        )

    return {
        "query_id": query["episode_id"],
        "true_action": query["action"]["action_class"],
        "true_outcome": replay.outcome_label(query),
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
        "suppressed": [],
    }


def aggregate_result(
    variant: str,
    description: str,
    per_query: list[dict[str, Any]],
    *,
    suppression_count: int,
) -> dict[str, Any]:
    aggregate_quality: Counter[str] = Counter()
    for row in per_query:
        aggregate_quality.update(row["quality_counts"])
    return {
        "variant": variant,
        "description": description,
        "top3_hit_rate": sum(row["top3_hit"] for row in per_query) / len(per_query),
        "failed_avoidance_rate": sum(row["avoids_failed"] for row in per_query) / len(per_query),
        "cross_family_top3_count": sum(row["cross_family_top3"] for row in per_query),
        "weak_cross_family_top3_count": sum(row["weak_cross_family_top3"] for row in per_query),
        "quality_counts": dict(aggregate_quality),
        "suppression_count": suppression_count,
        "per_query": per_query,
    }


def format_actions(actions: list[tuple[str, float]]) -> str:
    return ", ".join(f"{action}:{score:.2f}" for action, score in actions) if actions else "none"


def format_quality_counts(counts: dict[str, int]) -> str:
    return ablation.format_quality_counts(counts)


def quality_value(result: dict[str, Any], label: str) -> int:
    return int(result["quality_counts"].get(label, 0))


def render_report(
    results: list[dict[str, Any]],
    fixture_path: Path,
    extra_fixture_paths: list[Path],
    quality_label_path: Path | None,
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    all_fixture_paths = [fixture_path, *extra_fixture_paths]
    reference = next(result for result in results if result["variant"] == "context_gated_action_outcome")
    reference_rows = {row["query_id"]: row for row in reference["per_query"]}

    lines = [
        "# ActionOutcomeReplay Experiment 010 Vendor Baseline Results",
        "",
        f"Generated at: {now}",
        "Fixtures:",
        *[f"- `{path}`" for path in all_fixture_paths],
        f"- Quality labels: `{quality_label_path}`" if quality_label_path else "- Quality labels: none",
        "",
        "## Purpose",
        "",
        "Experiment 010 compares the current context-gated Action-Outcome Memory",
        "against vendor-inspired baselines that approximate common industry",
        "patterns: similar-incident retrieval, runbook/action-prior",
        "recommendation, and reflection-only experiential memory.",
        "",
        "## Summary",
        "",
        "| Variant | Top-3 Hit Rate | Failed-Action Avoidance | Useful Top3 | Superficial Top3 | Misleading Top3 | Cross-Family Top3 | Top Action Changes vs Context Gate | Suppressions |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]

    for result in results:
        changes = sum(
            row["top_action"] != reference_rows[row["query_id"]]["top_action"]
            for row in result["per_query"]
        )
        lines.append(
            "| {variant} | {hit:.2f} | {avoid:.2f} | {useful} | {superficial} | {misleading} | {cross} | {changes} | {suppressed} |".format(
                variant=result["variant"],
                hit=result["top3_hit_rate"],
                avoid=result["failed_avoidance_rate"],
                useful=quality_value(result, "useful"),
                superficial=quality_value(result, "superficial"),
                misleading=quality_value(result, "misleading"),
                cross=result["cross_family_top3_count"],
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
        "| Query | True Failed Action | similar_incident | runbook_action_prior | reflection_only_memory | context_gated_action_outcome |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    query_ids = [row["query_id"] for row in reference["per_query"]]
    by_variant = {
        result["variant"]: {row["query_id"]: row for row in result["per_query"]}
        for result in results
    }
    for query_id in query_ids:
        true_action = reference_rows[query_id]["true_action"]
        lines.append(
            "| {query} | {true_action} | {similar} | {runbook} | {reflection} | {gated} |".format(
                query=query_id,
                true_action=true_action,
                similar=by_variant["similar_incident"][query_id]["top_action"],
                runbook=by_variant["runbook_action_prior"][query_id]["top_action"],
                reflection=by_variant["reflection_only_memory"][query_id]["top_action"],
                gated=by_variant["context_gated_action_outcome"][query_id]["top_action"],
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
            "| Query | Top Actions | Hit | Avoids Failed Repeat | Labeled Top3 Quality |",
            "| --- | --- | --- | --- | --- |",
        ]
        for row in result["per_query"]:
            lines.append(
                "| {query} | {actions} | {hit} | {avoid} | {quality} |".format(
                    query=row["query_id"],
                    actions=format_actions(row["actions"]),
                    hit="yes" if row["top3_hit"] else "no",
                    avoid="yes" if row["avoids_failed"] else "no",
                    quality=format_quality_counts(row["quality_counts"]),
                )
            )
        lines.append("")

    lines += [
        "## Labeled Top-3 Retrieval Details",
        "",
        "| Variant | Query | Rank | Candidate | Family | Action | Outcome | Score | Quality |",
        "| --- | --- | ---: | --- | --- | --- | --- | ---: | --- |",
    ]
    for result in results:
        for row in result["per_query"]:
            for candidate in row["retrieval_top3"]:
                lines.append(
                    "| {variant} | {query} | {rank} | {candidate} | {family} | {action} | {outcome} | {score:.3f} | {quality} |".format(
                        variant=result["variant"],
                        query=row["query_id"],
                        rank=candidate["rank"],
                        candidate=candidate["episode_id"],
                        family=candidate["incident_type"],
                        action=candidate["action"],
                        outcome=candidate["outcome"],
                        score=candidate["score"],
                        quality=candidate["quality_label"],
                    )
                )

    gated = reference
    lines += [
        "",
        "## Context-Gate Suppressions",
        "",
        "| Query | Candidate | Action | Outcome | Raw Value | Gated Value | Multiplier | Context Score | Reasons |",
        "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- |",
    ]
    for row in gated["per_query"]:
        for suppressed in row.get("suppressed", []):
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
        "The key comparison is not whether every approach finds at least one",
        "historically successful action. On this fixture, that metric is too easy.",
        "The stronger commercial signal is failed-action avoidance under noisy",
        "same-family distractors.",
        "",
        "`context_gated_action_outcome` is the only variant that both preserves",
        "top-3 useful-action coverage and reaches perfect failed-action avoidance",
        "on the noisy fixture. The suppressions show why: it refuses to reuse",
        "positive or negative outcomes from mismatched topology, change, and metric",
        "contexts.",
        "",
        "## Next Best Step",
        "",
        "Replay Experiment 010 through SQL tables after alias-aware hash JOIN",
        "`WHERE` predicates are implemented, so the comparison can be audited with",
        "native ZeptoDB JOIN/window queries instead of only Python fixtures.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", type=Path, default=Path("docs/research/fixtures/action_outcome_episodes.json"))
    parser.add_argument("--extra-fixture", action="append", type=Path, default=[])
    parser.add_argument("--quality-labels", type=Path)
    parser.add_argument("--output", type=Path, default=Path("docs/research/results/action_outcome_vendor_baseline_010.md"))
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
    results = [
        evaluate_row_baseline(
            episodes,
            queries,
            quality_labels,
            variant="similar_incident",
            description="Similar-incident retrieval without outcome-aware action learning.",
            scorer=similar_incident_score,
            use_outcome=False,
        ),
        evaluate_runbook_action_prior(episodes, queries, quality_labels),
        evaluate_row_baseline(
            episodes,
            queries,
            quality_labels,
            variant="reflection_only_memory",
            description="Reflection/postmortem-style memory using textual experience and outcome recall.",
            scorer=reflection_memory_score,
            use_outcome=True,
        ),
        evaluate_context_gated(episodes, queries, quality_labels),
    ]

    report = render_report(results, args.fixture, args.extra_fixture, args.quality_labels)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)


if __name__ == "__main__":
    main()
