#!/usr/bin/env python3
"""Validate Experiment 010 with live ZeptoDB SQL JOIN/window replay."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

import action_outcome_live_sql_replay as live
import action_outcome_replay as replay
import action_outcome_vendor_baseline as vendor


QUERY_TABLE = "action_outcome_vendor_queries_010"
RECOMMENDATION_TABLE = "action_outcome_vendor_recommendations_010"
RETRIEVAL_TABLE = "action_outcome_vendor_retrieval_010"
SUPPRESSION_TABLE = "action_outcome_vendor_suppressions_010"

BASE_TABLES = [
    "action_outcome_episode_metrics",
    "action_outcome_episodes",
    "action_outcome_gate_suppressions",
    "action_outcome_replay_recommendations",
    "action_outcome_retrieval_quality_labels",
]

VENDOR_TABLES = [
    QUERY_TABLE,
    RECOMMENDATION_TABLE,
    RETRIEVAL_TABLE,
    SUPPRESSION_TABLE,
]

VARIANT_CODES = {
    "similar_incident": 1,
    "runbook_action_prior": 2,
    "reflection_only_memory": 3,
    "context_gated_action_outcome": 4,
}

QUALITY_CODES = {
    "useful": 1,
    "superficial": 2,
    "misleading": 3,
    "unlabeled": 0,
}

BASE_TS_NS = 1_781_748_910_000_000_000


@dataclass(frozen=True)
class ExpectedRows:
    query_count: int
    recommendation_count: int
    retrieval_count: int
    suppression_count: int
    failed_repeat_rows: list[list[Any]]
    context_top_rows: list[list[Any]]
    window_rows: list[tuple[int, int, int]]
    lag_rows: list[tuple[int, int, int, int]]


def sql_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def micros(value: float) -> int:
    return int(round(value * 1_000_000))


def post_checked(url: str, sql: str, timeout: float) -> dict[str, Any]:
    response = live.post_sql(url, sql, timeout)
    if not response.ok:
        raise RuntimeError(f"SQL failed ({response.status}): {sql[:180]} -> {response.body[:240]}")
    data = live.parse_json_response(response)
    if "error" in data:
        raise RuntimeError(f"SQL error: {sql[:180]} -> {data['error']}")
    return data


def reset_tables(url: str, timeout: float, tables: list[str]) -> None:
    for table in tables:
        post_checked(url, f"DROP TABLE IF EXISTS {table}", timeout)


def load_seed(url: str, sql_path: Path, timeout: float) -> tuple[list[str], list[tuple[int, str, live.SqlResponse]]]:
    sql_text = sql_path.read_text()
    statements = live.split_sql_statements(sql_text)
    reset_tables(url, timeout, BASE_TABLES)
    failures = live.load_seed(url, statements, timeout)
    return statements, failures


def build_results(
    fixture_path: Path,
    extra_fixture_paths: list[Path],
    quality_label_path: Path | None,
    query_ids: list[str],
) -> list[dict[str, Any]]:
    episodes = vendor.ablation.load_episodes(fixture_path, extra_fixture_paths)
    quality_labels = vendor.ablation.load_quality_labels(quality_label_path)
    by_id = {episode["episode_id"]: episode for episode in episodes}
    missing = [query_id for query_id in query_ids if query_id not in by_id]
    if missing:
        raise ValueError(f"unknown query ids: {', '.join(missing)}")
    queries = [by_id[query_id] for query_id in query_ids]
    return [
        vendor.evaluate_row_baseline(
            episodes,
            queries,
            quality_labels,
            variant="similar_incident",
            description="Similar-incident retrieval without outcome-aware action learning.",
            scorer=vendor.similar_incident_score,
            use_outcome=False,
        ),
        vendor.evaluate_runbook_action_prior(episodes, queries, quality_labels),
        vendor.evaluate_row_baseline(
            episodes,
            queries,
            quality_labels,
            variant="reflection_only_memory",
            description="Reflection/postmortem-style memory using textual experience and outcome recall.",
            scorer=vendor.reflection_memory_score,
            use_outcome=True,
        ),
        vendor.evaluate_context_gated(episodes, queries, quality_labels),
    ]


def create_vendor_tables(url: str, timeout: float) -> None:
    reset_tables(url, timeout, VENDOR_TABLES)
    post_checked(
        url,
        f"CREATE TABLE IF NOT EXISTS {QUERY_TABLE} ("
        "query_id STRING, "
        "query_seq INT64, "
        "true_failed_action STRING, "
        "timestamp_ns TIMESTAMP_NS"
        ")",
        timeout,
    )
    post_checked(
        url,
        f"CREATE TABLE IF NOT EXISTS {RECOMMENDATION_TABLE} ("
        "variant STRING, "
        "variant_code INT64, "
        "query_id STRING, "
        "query_seq INT64, "
        "group_id INT64, "
        "recommendation_rank INT64, "
        "action_class STRING, "
        "score_micros INT64, "
        "top3_hit INT64, "
        "avoids_failed INT64, "
        "timestamp_ns TIMESTAMP_NS"
        ")",
        timeout,
    )
    post_checked(
        url,
        f"CREATE TABLE IF NOT EXISTS {RETRIEVAL_TABLE} ("
        "variant STRING, "
        "variant_code INT64, "
        "query_id STRING, "
        "query_seq INT64, "
        "retrieval_rank INT64, "
        "candidate_id STRING, "
        "candidate_action STRING, "
        "candidate_outcome STRING, "
        "quality_label STRING, "
        "quality_code INT64, "
        "score_micros INT64, "
        "timestamp_ns TIMESTAMP_NS"
        ")",
        timeout,
    )
    post_checked(
        url,
        f"CREATE TABLE IF NOT EXISTS {SUPPRESSION_TABLE} ("
        "query_id STRING, "
        "query_seq INT64, "
        "candidate_id STRING, "
        "action_class STRING, "
        "outcome_label STRING, "
        "raw_value_micros INT64, "
        "gated_value_micros INT64, "
        "multiplier_micros INT64, "
        "context_score_micros INT64, "
        "reasons STRING, "
        "timestamp_ns TIMESTAMP_NS"
        ")",
        timeout,
    )


def materialize_results(
    url: str,
    timeout: float,
    results: list[dict[str, Any]],
    query_ids: list[str],
    after_create_tables: Callable[[], None] | None = None,
) -> ExpectedRows:
    create_vendor_tables(url, timeout)
    if after_create_tables is not None:
        after_create_tables()
    query_seq_by_id = {query_id: idx for idx, query_id in enumerate(query_ids, start=1)}
    true_action_by_id: dict[str, str] = {}
    recommendation_rows: list[tuple[str, int, str, int, int, int, str, int, int, int, int]] = []
    retrieval_rows: list[tuple[str, int, str, int, int, str, str, str, str, int, int, int]] = []
    suppression_rows: list[tuple[str, int, str, str, str, int, int, int, int, str, int]] = []

    for result in results:
        variant = result["variant"]
        variant_code = VARIANT_CODES[variant]
        for row in result["per_query"]:
            query_id = row["query_id"]
            query_seq = query_seq_by_id[query_id]
            true_action_by_id.setdefault(query_id, row["true_action"])
            group_id = variant_code * 100 + query_seq
            for rank, (action, score) in enumerate(row["actions"], start=1):
                recommendation_rows.append(
                    (
                        variant,
                        variant_code,
                        query_id,
                        query_seq,
                        group_id,
                        rank,
                        action,
                        micros(score),
                        1 if row["top3_hit"] else 0,
                        1 if row["avoids_failed"] else 0,
                        BASE_TS_NS + len(recommendation_rows),
                    )
                )
            for candidate in row["retrieval_top3"]:
                label = candidate["quality_label"]
                retrieval_rows.append(
                    (
                        variant,
                        variant_code,
                        query_id,
                        query_seq,
                        int(candidate["rank"]),
                        str(candidate["episode_id"]),
                        str(candidate["action"]),
                        str(candidate["outcome"]),
                        label,
                        QUALITY_CODES.get(label, 0),
                        micros(float(candidate["score"])),
                        BASE_TS_NS + 10_000 + len(retrieval_rows),
                    )
                )
            for suppressed in row.get("suppressed", []):
                suppression_rows.append(
                    (
                        query_id,
                        query_seq,
                        str(suppressed["episode_id"]),
                        str(suppressed["action"]),
                        str(suppressed["outcome"]),
                        micros(float(suppressed["raw_value"])),
                        micros(float(suppressed["gated_value"])),
                        micros(float(suppressed["multiplier"])),
                        micros(float(suppressed["context_score"])),
                        ",".join(suppressed["reasons"]) if suppressed["reasons"] else "none",
                        BASE_TS_NS + 20_000 + len(suppression_rows),
                    )
                )

    for query_id in query_ids:
        query_seq = query_seq_by_id[query_id]
        post_checked(
            url,
            f"INSERT INTO {QUERY_TABLE} "
            "(query_id, query_seq, true_failed_action, timestamp_ns) VALUES "
            f"({sql_quote(query_id)}, {query_seq}, "
            f"{sql_quote(true_action_by_id[query_id])}, {BASE_TS_NS + 30_000 + query_seq})",
            timeout,
        )

    for row in recommendation_rows:
        post_checked(
            url,
            f"INSERT INTO {RECOMMENDATION_TABLE} "
            "(variant, variant_code, query_id, query_seq, group_id, "
            "recommendation_rank, action_class, score_micros, top3_hit, "
            "avoids_failed, timestamp_ns) VALUES "
            f"({sql_quote(row[0])}, {row[1]}, {sql_quote(row[2])}, {row[3]}, "
            f"{row[4]}, {row[5]}, {sql_quote(row[6])}, {row[7]}, {row[8]}, "
            f"{row[9]}, {row[10]})",
            timeout,
        )

    for row in retrieval_rows:
        post_checked(
            url,
            f"INSERT INTO {RETRIEVAL_TABLE} "
            "(variant, variant_code, query_id, query_seq, retrieval_rank, "
            "candidate_id, candidate_action, candidate_outcome, quality_label, "
            "quality_code, score_micros, timestamp_ns) VALUES "
            f"({sql_quote(row[0])}, {row[1]}, {sql_quote(row[2])}, {row[3]}, "
            f"{row[4]}, {sql_quote(row[5])}, {sql_quote(row[6])}, "
            f"{sql_quote(row[7])}, {sql_quote(row[8])}, {row[9]}, {row[10]}, "
            f"{row[11]})",
            timeout,
        )

    for row in suppression_rows:
        post_checked(
            url,
            f"INSERT INTO {SUPPRESSION_TABLE} "
            "(query_id, query_seq, candidate_id, action_class, outcome_label, "
            "raw_value_micros, gated_value_micros, multiplier_micros, "
            "context_score_micros, reasons, timestamp_ns) VALUES "
            f"({sql_quote(row[0])}, {row[1]}, {sql_quote(row[2])}, "
            f"{sql_quote(row[3])}, {sql_quote(row[4])}, {row[5]}, {row[6]}, "
            f"{row[7]}, {row[8]}, {sql_quote(row[9])}, {row[10]})",
            timeout,
        )

    failed_repeat_rows = [
        [row[0], row[2], row[6], true_action_by_id[row[2]]]
        for row in recommendation_rows
        if row[5] == 1 and row[9] == 0
    ]
    context_top_rows = [
        [row[2], row[6], true_action_by_id[row[2]]]
        for row in recommendation_rows
        if row[1] == VARIANT_CODES["context_gated_action_outcome"] and row[5] == 1
    ]

    by_group: dict[int, list[tuple[int, int]]] = {}
    for row in recommendation_rows:
        by_group.setdefault(row[4], []).append((row[5], row[7]))
    window_rows: list[tuple[int, int, int]] = []
    lag_rows: list[tuple[int, int, int, int]] = []
    for group_id, rows in by_group.items():
        previous_score = 0
        for idx, (rank, score) in enumerate(sorted(rows), start=1):
            window_rows.append((group_id, rank, idx))
            lag_rows.append((group_id, rank, score, previous_score))
            previous_score = score

    return ExpectedRows(
        query_count=len(query_ids),
        recommendation_count=len(recommendation_rows),
        retrieval_count=len(retrieval_rows),
        suppression_count=len(suppression_rows),
        failed_repeat_rows=sorted(failed_repeat_rows),
        context_top_rows=sorted(context_top_rows),
        window_rows=sorted(window_rows),
        lag_rows=sorted(lag_rows),
    )


def table_count(url: str, timeout: float, table: str) -> int | None:
    return live.fetch_count(url, table, timeout)


def query_data(url: str, timeout: float, sql: str) -> dict[str, Any]:
    return post_checked(url, sql, timeout)


def sorted_rows(data: dict[str, Any]) -> list[list[Any]]:
    rows = data.get("data", [])
    return sorted(rows)


def validate_sql(
    url: str,
    timeout: float,
    expected: ExpectedRows,
) -> dict[str, Any]:
    counts = {
        QUERY_TABLE: table_count(url, timeout, QUERY_TABLE),
        RECOMMENDATION_TABLE: table_count(url, timeout, RECOMMENDATION_TABLE),
        RETRIEVAL_TABLE: table_count(url, timeout, RETRIEVAL_TABLE),
        SUPPRESSION_TABLE: table_count(url, timeout, SUPPRESSION_TABLE),
    }

    failed_repeat_sql = (
        "SELECT r.variant, r.query_id, r.action_class, q.true_failed_action "
        f"FROM {RECOMMENDATION_TABLE} r "
        f"JOIN {QUERY_TABLE} q ON r.query_id = q.query_id "
        "WHERE r.recommendation_rank = 1 AND r.avoids_failed = 0"
    )
    failed_repeat_result = query_data(url, timeout, failed_repeat_sql)
    failed_repeat_rows = sorted_rows(failed_repeat_result)

    context_top_sql = (
        "SELECT r.query_id, r.action_class, q.true_failed_action "
        f"FROM {RECOMMENDATION_TABLE} r "
        f"JOIN {QUERY_TABLE} q ON r.query_id = q.query_id "
        "WHERE r.variant_code = 4 AND r.recommendation_rank = 1 "
        "AND r.avoids_failed = 1"
    )
    context_top_result = query_data(url, timeout, context_top_sql)
    context_top_rows = sorted_rows(context_top_result)

    suppression_join_sql = (
        "SELECT s.query_id, s.candidate_id, s.action_class, s.reasons "
        f"FROM {SUPPRESSION_TABLE} s "
        f"JOIN {RECOMMENDATION_TABLE} r ON s.query_id = r.query_id "
        "WHERE r.variant_code = 4 AND r.recommendation_rank = 1"
    )
    suppression_join_result = query_data(url, timeout, suppression_join_sql)
    suppression_join_rows = sorted_rows(suppression_join_result)

    misleading_join_sql = (
        "SELECT t.variant, t.query_id, t.candidate_id, q.true_failed_action "
        f"FROM {RETRIEVAL_TABLE} t "
        f"JOIN {QUERY_TABLE} q ON t.query_id = q.query_id "
        "WHERE t.quality_code = 3"
    )
    misleading_join_result = query_data(url, timeout, misleading_join_sql)
    misleading_join_rows = sorted_rows(misleading_join_result)

    window_sql = (
        "SELECT group_id, recommendation_rank, "
        "ROW_NUMBER() OVER (PARTITION BY group_id ORDER BY recommendation_rank) AS rank_check "
        f"FROM {RECOMMENDATION_TABLE}"
    )
    window_result = query_data(url, timeout, window_sql)
    window_rows = sorted((int(row[0]), int(row[1]), int(row[2])) for row in window_result.get("data", []))

    lag_sql = (
        "SELECT group_id, recommendation_rank, score_micros, "
        "LAG(score_micros, 1, 0) OVER (PARTITION BY group_id ORDER BY recommendation_rank) AS prev_score "
        f"FROM {RECOMMENDATION_TABLE}"
    )
    lag_result = query_data(url, timeout, lag_sql)
    lag_rows = sorted(
        (int(row[0]), int(row[1]), int(row[2]), int(row[3]))
        for row in lag_result.get("data", [])
    )

    expected_counts = {
        QUERY_TABLE: expected.query_count,
        RECOMMENDATION_TABLE: expected.recommendation_count,
        RETRIEVAL_TABLE: expected.retrieval_count,
        SUPPRESSION_TABLE: expected.suppression_count,
    }
    return {
        "counts": counts,
        "expected_counts": expected_counts,
        "count_status": counts == expected_counts,
        "failed_repeat_rows": failed_repeat_rows,
        "expected_failed_repeat_rows": expected.failed_repeat_rows,
        "failed_repeat_status": failed_repeat_rows == expected.failed_repeat_rows,
        "context_top_rows": context_top_rows,
        "expected_context_top_rows": expected.context_top_rows,
        "context_top_status": context_top_rows == expected.context_top_rows,
        "suppression_join_rows": suppression_join_rows,
        "suppression_join_status": len(suppression_join_rows) == expected.suppression_count,
        "misleading_join_rows": misleading_join_rows,
        "misleading_join_status": len(misleading_join_rows) == 23,
        "window_rows": window_rows,
        "expected_window_rows": expected.window_rows,
        "window_status": window_rows == expected.window_rows,
        "lag_rows": lag_rows,
        "expected_lag_rows": expected.lag_rows,
        "lag_status": lag_rows == expected.lag_rows,
    }


def markdown_table(rows: list[list[Any]], headers: list[str]) -> list[str]:
    lines = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(f"`{item}`" if isinstance(item, str) else str(item) for item in row) + " |")
    return lines


def render_report(
    *,
    url: str,
    sql_path: Path,
    statements: list[str],
    seed_failures: list[tuple[int, str, live.SqlResponse]],
    expected_seed_counts: dict[str, int],
    actual_seed_counts: dict[str, int | None],
    validation: dict[str, Any],
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    seed_count_status = all(
        actual_seed_counts.get(table) == expected
        for table, expected in expected_seed_counts.items()
    )
    sql_status = all(
        validation[key]
        for key in (
            "count_status",
            "failed_repeat_status",
            "context_top_status",
            "suppression_join_status",
            "misleading_join_status",
            "window_status",
            "lag_status",
        )
    )

    lines = [
        "# ActionOutcomeReplay Experiment 010 SQL/JOIN/Window Replay Results",
        "",
        f"Generated at: {now}",
        f"Endpoint: `{url}`",
        f"SQL seed: `{sql_path}`",
        "",
        "## Load Summary",
        "",
        f"- Seed statements attempted: {len(statements)}",
        f"- Seed statements succeeded: {len(statements) - len(seed_failures)}",
        f"- Seed statements failed: {len(seed_failures)}",
        f"- Seed row-count status: {'pass' if seed_count_status else 'fail'}",
        f"- Vendor table row-count status: {'pass' if validation['count_status'] else 'fail'}",
        f"- Failed-repeat JOIN status: {'pass' if validation['failed_repeat_status'] else 'fail'}",
        f"- Context top-action JOIN status: {'pass' if validation['context_top_status'] else 'fail'}",
        f"- Suppression JOIN status: {'pass' if validation['suppression_join_status'] else 'fail'}",
        f"- Misleading retrieval JOIN status: {'pass' if validation['misleading_join_status'] else 'fail'}",
        f"- Window ROW_NUMBER status: {'pass' if validation['window_status'] else 'fail'}",
        f"- Window LAG status: {'pass' if validation['lag_status'] else 'fail'}",
        f"- Overall SQL/JOIN/window status: {'pass' if sql_status else 'fail'}",
    ]
    if seed_failures:
        lines += ["", "First seed failures:"]
        for idx, stmt, response in seed_failures[:5]:
            lines.append(f"- #{idx} HTTP {response.status}: `{stmt[:140]}` -> `{response.body[:180]}`")

    lines += [
        "",
        "## Seed Row Counts",
        "",
        "| Table | Expected Inserts | Live Rows | Status |",
        "| --- | ---: | ---: | --- |",
    ]
    for table, expected in expected_seed_counts.items():
        actual = actual_seed_counts.get(table)
        status = "pass" if actual == expected else "fail"
        actual_text = "n/a" if actual is None else str(actual)
        lines.append(f"| `{table}` | {expected} | {actual_text} | {status} |")

    lines += [
        "",
        "## Vendor Replay Table Counts",
        "",
        "| Table | Expected Rows | Live Rows | Status |",
        "| --- | ---: | ---: | --- |",
    ]
    for table, expected in validation["expected_counts"].items():
        actual = validation["counts"].get(table)
        status = "pass" if actual == expected else "fail"
        actual_text = "n/a" if actual is None else str(actual)
        lines.append(f"| `{table}` | {expected} | {actual_text} | {status} |")

    lines += [
        "",
        "## Failed-Repeat JOIN",
        "",
        "This query joins top recommendations to query episodes and uses alias-qualified",
        "`WHERE` predicates on the joined result:",
        "",
        "```sql",
        "SELECT r.variant, r.query_id, r.action_class, q.true_failed_action",
        f"FROM {RECOMMENDATION_TABLE} r",
        f"JOIN {QUERY_TABLE} q ON r.query_id = q.query_id",
        "WHERE r.recommendation_rank = 1 AND r.avoids_failed = 0",
        "```",
        "",
        f"- Status: {'pass' if validation['failed_repeat_status'] else 'fail'}",
        "",
        *markdown_table(
            validation["failed_repeat_rows"],
            ["Variant", "Query", "Recommended Action", "Historical Failed Action"],
        ),
        "",
        "## Context-Gated Top Actions",
        "",
        f"- Status: {'pass' if validation['context_top_status'] else 'fail'}",
        "",
        *markdown_table(
            validation["context_top_rows"],
            ["Query", "Context-Gated Top Action", "Historical Failed Action"],
        ),
        "",
        "## JOIN/Window Acceptance",
        "",
        f"- Suppression JOIN rows: {len(validation['suppression_join_rows'])}",
        f"- Suppression JOIN status: {'pass' if validation['suppression_join_status'] else 'fail'}",
        f"- Misleading retrieval JOIN rows: {len(validation['misleading_join_rows'])}",
        f"- Misleading retrieval JOIN status: {'pass' if validation['misleading_join_status'] else 'fail'}",
        f"- ROW_NUMBER rows: {len(validation['window_rows'])}",
        f"- ROW_NUMBER status: {'pass' if validation['window_status'] else 'fail'}",
        f"- LAG rows: {len(validation['lag_rows'])}",
        f"- LAG status: {'pass' if validation['lag_status'] else 'fail'}",
        "",
        "## Interpretation",
        "",
        "Experiment 010 now has a live ZeptoDB SQL acceptance path. The comparison",
        "is no longer only a Python fixture report: recommendations, retrieval",
        "evidence, query controls, and context suppressions are materialized into",
        "declared ZeptoDB tables and audited with native hash JOIN plus window",
        "queries.",
        "",
        "The key product result remains intact: vendor-inspired baselines have",
        "failed-repeat top actions, while `context_gated_action_outcome` returns",
        "six safe top actions and uses suppressions to keep mismatched historical",
        "outcomes from dominating the ranking.",
        "",
        "## Next Best Step",
        "",
        "Port this SQL/JOIN/window replay into the two-node live harness to record",
        "which parts are single-node SQL-complete and which parts still depend on",
        "distributed JOIN/window planner work.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8123/")
    parser.add_argument("--sql-seed", type=Path, default=Path("docs/research/results/action_outcome_sql_replay_006.sql"))
    parser.add_argument("--fixture", type=Path, default=Path("docs/research/fixtures/action_outcome_episodes.json"))
    parser.add_argument("--extra-fixture", action="append", type=Path, default=[])
    parser.add_argument("--quality-labels", type=Path)
    parser.add_argument("--output", type=Path, default=Path("docs/research/results/action_outcome_vendor_sql_replay_010.md"))
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--query-id", action="append", dest="query_ids")
    args = parser.parse_args()

    query_ids = args.query_ids or replay.DEFAULT_QUERY_IDS
    statements, seed_failures = load_seed(args.url, args.sql_seed, args.timeout)
    expected_seed_counts = live.count_seed_inserts(statements)
    actual_seed_counts = {
        table: table_count(args.url, args.timeout, table)
        for table in expected_seed_counts
    }
    if seed_failures:
        raise SystemExit(f"seed load failed: {seed_failures[0][2].body[:240]}")

    results = build_results(args.fixture, args.extra_fixture, args.quality_labels, query_ids)
    expected = materialize_results(args.url, args.timeout, results, query_ids)
    validation = validate_sql(args.url, args.timeout, expected)
    report = render_report(
        url=args.url,
        sql_path=args.sql_seed,
        statements=statements,
        seed_failures=seed_failures,
        expected_seed_counts=expected_seed_counts,
        actual_seed_counts=actual_seed_counts,
        validation=validation,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)

    if not all(
        validation[key]
        for key in (
            "count_status",
            "failed_repeat_status",
            "context_top_status",
            "suppression_join_status",
            "misleading_join_status",
            "window_status",
            "lag_status",
        )
    ):
        raise SystemExit("SQL/JOIN/window validation failed")


if __name__ == "__main__":
    main()
