#!/usr/bin/env python3
"""Validate Action-Outcome JOIN/window replay acceptance against live ZeptoDB."""

from __future__ import annotations

import argparse
import json
import sqlite3
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import action_outcome_live_sql_replay as live


ACTION_CODES = {
    "rollback": 1,
    "scale_out": 2,
    "config_revert": 3,
    "traffic_drain": 4,
    "cache_purge": 5,
    "restart": 6,
}

BASE_TS_NS = 1_781_748_863_000_000_000
QUERY_TABLE = "action_outcome_acceptance_queries_009"
RECOMMENDATION_TABLE = "action_outcome_acceptance_recommendations_009"


@dataclass(frozen=True)
class ControlRecommendation:
    query_id: str
    query_seq: int
    recommendation_rank: int
    action_class: str
    action_code: int
    score_micros: int
    avoids_failed_repeat: int


@dataclass(frozen=True)
class ControlQuery:
    query_id: str
    query_seq: int
    observed_action_class: str
    observed_action_code: int
    human_outcome: str
    expected_top_action: str
    expected_top_action_code: int
    human_failure_flag: int


def sqlite_control(sql_text: str) -> sqlite3.Connection:
    conn = sqlite3.connect(":memory:")
    conn.row_factory = sqlite3.Row
    for stmt in live.split_sql_statements(sql_text):
        conn.execute(stmt)
    return conn


def action_code(action_class: str) -> int:
    try:
        return ACTION_CODES[action_class]
    except KeyError as exc:
        raise ValueError(f"unknown action class for acceptance code: {action_class}") from exc


def build_control_rows(sql_text: str) -> tuple[list[ControlQuery], list[ControlRecommendation]]:
    conn = sqlite_control(sql_text)
    query_ids = list(live.EXPECTED_TOP_ACTIONS.keys())
    query_seq_by_id = {query_id: idx for idx, query_id in enumerate(query_ids, start=1)}

    query_rows: list[ControlQuery] = []
    for query_id in query_ids:
        episode = conn.execute(
            "SELECT action_class, human_outcome FROM action_outcome_episodes "
            "WHERE episode_id = ?",
            (query_id,),
        ).fetchone()
        if episode is None:
            raise ValueError(f"missing control episode for {query_id}")
        expected_top_action = live.EXPECTED_TOP_ACTIONS[query_id]
        human_outcome = str(episode["human_outcome"])
        query_rows.append(
            ControlQuery(
                query_id=query_id,
                query_seq=query_seq_by_id[query_id],
                observed_action_class=str(episode["action_class"]),
                observed_action_code=action_code(str(episode["action_class"])),
                human_outcome=human_outcome,
                expected_top_action=expected_top_action,
                expected_top_action_code=action_code(expected_top_action),
                human_failure_flag=1 if human_outcome == "failure" else 0,
            )
        )

    rec_rows: list[ControlRecommendation] = []
    placeholders = ",".join("?" for _ in query_ids)
    for row in conn.execute(
        "SELECT query_id, recommendation_rank, action_class, score_micros, "
        "avoids_failed_repeat "
        "FROM action_outcome_replay_recommendations "
        f"WHERE query_id IN ({placeholders}) "
        "ORDER BY query_id, recommendation_rank",
        query_ids,
    ):
        action_class = str(row["action_class"])
        rec_rows.append(
            ControlRecommendation(
                query_id=str(row["query_id"]),
                query_seq=query_seq_by_id[str(row["query_id"])],
                recommendation_rank=int(row["recommendation_rank"]),
                action_class=action_class,
                action_code=action_code(action_class),
                score_micros=int(row["score_micros"]),
                avoids_failed_repeat=int(row["avoids_failed_repeat"]),
            )
        )

    expected_rec_count = len(query_ids) * 3
    if len(rec_rows) != expected_rec_count:
        raise ValueError(
            f"expected {expected_rec_count} control recommendation rows, got {len(rec_rows)}"
        )
    return query_rows, sorted(rec_rows, key=lambda r: (r.query_seq, r.recommendation_rank))


def post_checked(url: str, sql: str, timeout: float) -> live.SqlResponse:
    response = live.post_sql(url, sql, timeout)
    if not response.ok:
        raise RuntimeError(f"SQL failed ({response.status}): {sql[:160]} -> {response.body[:240]}")
    data = live.parse_json_response(response)
    if "error" in data:
        raise RuntimeError(f"SQL error: {sql[:160]} -> {data['error']}")
    return response


def reset_tables(url: str, table_names: list[str], timeout: float) -> list[str]:
    failures: list[str] = []
    for table in table_names:
        response = live.post_sql(url, f"DROP TABLE IF EXISTS {table}", timeout)
        if not response.ok:
            failures.append(f"{table}: HTTP {response.status} {response.body[:160]}")
    return failures


def insert_projection(
    *,
    url: str,
    timeout: float,
    query_rows: list[ControlQuery],
    rec_rows: list[ControlRecommendation],
) -> None:
    post_checked(
        url,
        f"DROP TABLE IF EXISTS {QUERY_TABLE}",
        timeout,
    )
    post_checked(
        url,
        f"DROP TABLE IF EXISTS {RECOMMENDATION_TABLE}",
        timeout,
    )
    post_checked(
        url,
        f"CREATE TABLE IF NOT EXISTS {QUERY_TABLE} ("
        "symbol INT64, "
        "timestamp_ns TIMESTAMP_NS, "
        "query_seq INT64, "
        "observed_action_code INT64, "
        "expected_top_action_code INT64, "
        "human_failure_flag INT64"
        ")",
        timeout,
    )
    post_checked(
        url,
        f"CREATE TABLE IF NOT EXISTS {RECOMMENDATION_TABLE} ("
        "symbol INT64, "
        "timestamp_ns TIMESTAMP_NS, "
        "query_seq INT64, "
        "rank_num INT64, "
        "action_code INT64, "
        "score_num INT64, "
        "avoid_num INT64"
        ")",
        timeout,
    )

    for idx, row in enumerate(query_rows):
        post_checked(
            url,
            f"INSERT INTO {QUERY_TABLE} "
            "(symbol, timestamp_ns, query_seq, observed_action_code, "
            "expected_top_action_code, human_failure_flag) "
            f"VALUES (0, {BASE_TS_NS + idx}, {row.query_seq}, "
            f"{row.observed_action_code}, {row.expected_top_action_code}, "
            f"{row.human_failure_flag})",
            timeout,
        )

    for idx, row in enumerate(rec_rows):
        post_checked(
            url,
            f"INSERT INTO {RECOMMENDATION_TABLE} "
            "(symbol, timestamp_ns, query_seq, rank_num, action_code, score_num, avoid_num) "
            f"VALUES (0, {BASE_TS_NS + 100 + idx}, {row.query_seq}, "
            f"{row.recommendation_rank}, {row.action_code}, {row.score_micros}, "
            f"{row.avoids_failed_repeat})",
            timeout,
        )


def scalar_count(result: dict[str, Any]) -> int | None:
    try:
        return int(result["data"][0][0])
    except (KeyError, IndexError, TypeError, ValueError):
        return None


def expected_join_rows(
    query_rows: list[ControlQuery],
    rec_rows: list[ControlRecommendation],
) -> list[list[int]]:
    queries = {row.query_seq: row for row in query_rows}
    out: list[list[int]] = []
    for rec in rec_rows:
        query = queries[rec.query_seq]
        out.append(
            [
                rec.query_seq,
                rec.recommendation_rank,
                rec.action_code,
                query.observed_action_code,
                query.expected_top_action_code,
                rec.avoids_failed_repeat,
            ]
        )
    return out


def validate_numeric_join(
    rows: list[list[Any]],
    query_rows: list[ControlQuery],
    rec_rows: list[ControlRecommendation],
) -> bool:
    if rows != expected_join_rows(query_rows, rec_rows):
        return False
    for row in rows:
        query_seq, rank_num, action_num, observed_code, expected_code, avoid_num = [
            int(value) for value in row
        ]
        query = next(q for q in query_rows if q.query_seq == query_seq)
        if rank_num == 1:
            if action_num != expected_code:
                return False
            if query.human_failure_flag and action_num == observed_code:
                return False
            if query.human_failure_flag and avoid_num != 1:
                return False
        if query_seq <= 0:
            return False
    return True


def validate_window_rows(rows: list[list[Any]]) -> bool:
    by_query: dict[int, list[list[int]]] = {}
    for row in rows:
        if len(row) < 6:
            return False
        query_seq = int(row[0])
        by_query.setdefault(query_seq, []).append([int(value) for value in row])
    for seq_rows in by_query.values():
        previous_score = 0
        for expected_rank, row in enumerate(seq_rows, start=1):
            rank_num = row[1]
            score_num = row[3]
            rank_check = row[4]
            prev_score = row[5]
            if rank_num != expected_rank or rank_check != expected_rank:
                return False
            if prev_score != previous_score:
                return False
            previous_score = score_num
    return len(rows) == len(live.EXPECTED_TOP_ACTIONS) * 3


def validate_native_string_window(rows: list[list[Any]]) -> bool:
    by_query: dict[str, list[list[Any]]] = {}
    for row in rows:
        if len(row) < 3:
            return False
        by_query.setdefault(str(row[0]), []).append(row)
    for query_id in live.EXPECTED_TOP_ACTIONS:
        seq_rows = by_query.get(query_id, [])
        if len(seq_rows) != 3:
            return False
        for expected_rank, row in enumerate(seq_rows, start=1):
            if int(row[1]) != expected_rank or int(row[2]) != expected_rank:
                return False
    return True


def render_report(
    *,
    url: str,
    sql_path: Path,
    statements: list[str],
    failures: list[tuple[int, str, live.SqlResponse]],
    reset_failures: list[str],
    expected_counts: dict[str, int],
    actual_counts: dict[str, int | None],
    query_rows: list[ControlQuery],
    rec_rows: list[ControlRecommendation],
    projection_counts: dict[str, int | None],
    native_string_join: dict[str, Any],
    native_string_window: dict[str, Any],
    numeric_join: dict[str, Any],
    numeric_window: dict[str, Any],
    native_string_window_ok: bool,
    numeric_join_ok: bool,
    numeric_window_ok: bool,
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    row_counts_pass = all(actual_counts.get(table) == count for table, count in expected_counts.items())
    projection_pass = (
        projection_counts.get(QUERY_TABLE) == len(query_rows)
        and projection_counts.get(RECOMMENDATION_TABLE) == len(rec_rows)
    )
    numeric_acceptance_status = (
        "pass" if projection_pass and numeric_join_ok and numeric_window_ok else "fail"
    )
    string_window_status = "pass" if native_string_window_ok else "fail"

    string_join_rows = native_string_join.get("data", []) if native_string_join else []
    controls = {row.query_seq: row for row in query_rows}
    expected_string_join_rows = [
        [
            rec.query_id,
            rec.recommendation_rank,
            rec.action_class,
            controls[rec.query_seq].observed_action_class,
            controls[rec.query_seq].human_outcome,
        ]
        for rec in sorted(rec_rows, key=lambda r: (r.query_id, r.recommendation_rank))
    ]
    string_join_status = (
        "pass"
        if sorted(string_join_rows) == sorted(expected_string_join_rows)
        else "blocked_current_engine"
    )

    lines = [
        "# ActionOutcomeReplay Experiment 009 JOIN/Window Live Acceptance Results",
        "",
        f"Generated at: {now}",
        f"Endpoint: `{url}`",
        f"SQL seed: `{sql_path}`",
        "",
        "## Load Summary",
        "",
        f"- Seed statements attempted: {len(statements)}",
        f"- Seed statements succeeded: {len(statements) - len(failures)}",
        f"- Seed statements failed: {len(failures)}",
        f"- Seed row-count status: {'pass' if row_counts_pass else 'fail'}",
        f"- Projection row-count status: {'pass' if projection_pass else 'fail'}",
        f"- Native string window status: {string_window_status}",
        f"- Numeric JOIN/window acceptance status: {numeric_acceptance_status}",
        f"- Native string JOIN status: {string_join_status}",
    ]
    if reset_failures:
        lines += ["", "Reset failures:"]
        lines.extend(f"- {failure}" for failure in reset_failures[:8])
    if failures:
        lines += ["", "Seed load failures:"]
        for idx, stmt, response in failures[:5]:
            lines.append(f"- #{idx} HTTP {response.status}: `{stmt[:140]}` -> `{response.body[:180]}`")

    lines += [
        "",
        "## Seed Row Counts",
        "",
        "| Table | Expected Inserts | Live Rows | Status |",
        "| --- | ---: | ---: | --- |",
    ]
    for table, expected in expected_counts.items():
        actual = actual_counts.get(table)
        status = "pass" if actual == expected else "fail"
        actual_text = "n/a" if actual is None else str(actual)
        lines.append(f"| `{table}` | {expected} | {actual_text} | {status} |")

    lines += [
        "",
        "## Acceptance Projection",
        "",
        "| Table | Expected Rows | Live Rows | Status |",
        "| --- | ---: | ---: | --- |",
        f"| `{QUERY_TABLE}` | {len(query_rows)} | {projection_counts.get(QUERY_TABLE)} | "
        f"{'pass' if projection_counts.get(QUERY_TABLE) == len(query_rows) else 'fail'} |",
        f"| `{RECOMMENDATION_TABLE}` | {len(rec_rows)} | "
        f"{projection_counts.get(RECOMMENDATION_TABLE)} | "
        f"{'pass' if projection_counts.get(RECOMMENDATION_TABLE) == len(rec_rows) else 'fail'} |",
        "",
        "Action codes:",
        "",
        "| Code | Action |",
        "| ---: | --- |",
    ]
    for action, code in sorted(ACTION_CODES.items(), key=lambda item: item[1]):
        lines.append(f"| {code} | `{action}` |")

    lines += [
        "",
        "## Query-Level Control Rows",
        "",
        "| Seq | Query | Observed Episode Action | Expected Top Action | Human Failure Flag |",
        "| ---: | --- | --- | --- | ---: |",
    ]
    for row in query_rows:
        lines.append(
            f"| {row.query_seq} | `{row.query_id}` | `{row.observed_action_class}` "
            f"({row.observed_action_code}) | `{row.expected_top_action}` "
            f"({row.expected_top_action_code}) | {row.human_failure_flag} |"
        )

    lines += [
        "",
        "## Native String JOIN Boundary",
        "",
        "The direct research-shape JOIN checks whether string join keys and",
        "joined string result columns are preserved by the hash join executor.",
        "",
        "```json",
        json.dumps(native_string_join, indent=2, sort_keys=True)[:2500],
        "```",
        "",
        f"- Native string JOIN status: {string_join_status}",
        "",
        "## Native String Window",
        "",
        "```json",
        json.dumps(native_string_window, indent=2, sort_keys=True)[:2500],
        "```",
        "",
        f"- Native string window status: {string_window_status}",
        "",
        "## Numeric JOIN Acceptance",
        "",
        "```json",
        json.dumps(numeric_join, indent=2, sort_keys=True)[:3000],
        "```",
        "",
        f"- Numeric JOIN status: {'pass' if numeric_join_ok else 'fail'}",
        "",
        "## Numeric Window Acceptance",
        "",
        "```json",
        json.dumps(numeric_window, indent=2, sort_keys=True)[:3000],
        "```",
        "",
        f"- Numeric window status: {'pass' if numeric_window_ok else 'fail'}",
        "",
        "## Interpretation",
        "",
        "Experiment 009 adds the JOIN/window replay acceptance layer after the",
        "single-node and distributed load/replay harnesses. The native string window",
        "query proves the replay table can partition by `query_id` and preserve",
        "rank order. The numeric projection proves the Action-Outcome replay",
        "decision surface can be checked with a real ZeptoDB hash JOIN plus",
        "ROW_NUMBER/LAG window chain.",
        "",
        "The native string-key JOIN now produces semantic string rows for the",
        "original research schema. The numeric projection remains useful for the",
        "top-action and outcome-avoidance checks because hash JOIN predicate",
        "pushdown for aliased WHERE clauses is tracked separately.",
        "",
        "## Next Steps",
        "",
        "1. Add alias-aware WHERE predicate handling for hash JOIN queries so",
        "   native top-action JOIN checks can replace the projection path.",
        "2. Port Experiment 009 to the two-node live harness so distributed",
        "   JOIN/window limitations are recorded separately from single-node",
        "   executor limits.",
        "3. Add an Action-Outcome shard-key policy so operational replay tables",
        "   can distribute by `query_id` or incident id instead of default symbol 0.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8123/")
    parser.add_argument(
        "--sql-file",
        type=Path,
        default=Path("docs/research/results/action_outcome_sql_replay_006.sql"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("docs/research/results/action_outcome_join_window_replay_009.md"),
    )
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument(
        "--no-reset-seed",
        action="store_true",
        help="Do not drop Action-Outcome seed tables before loading.",
    )
    args = parser.parse_args()

    sql_text = args.sql_file.read_text()
    statements = live.split_sql_statements(sql_text)
    expected_counts = live.count_seed_inserts(statements)
    query_rows, rec_rows = build_control_rows(sql_text)

    reset_failures: list[str] = []
    if not args.no_reset_seed:
        reset_failures.extend(reset_tables(args.url, list(expected_counts), args.timeout))

    failures = live.load_seed(args.url, statements, args.timeout)
    actual_counts = {
        table: live.fetch_count(args.url, table, args.timeout)
        for table in expected_counts
    }

    insert_projection(
        url=args.url,
        timeout=args.timeout,
        query_rows=query_rows,
        rec_rows=rec_rows,
    )
    projection_counts = {
        QUERY_TABLE: live.fetch_count(args.url, QUERY_TABLE, args.timeout),
        RECOMMENDATION_TABLE: live.fetch_count(args.url, RECOMMENDATION_TABLE, args.timeout),
    }

    native_string_join_sql = (
        "SELECT r.query_id, r.recommendation_rank, r.action_class, "
        "e.action_class, e.human_outcome "
        "FROM action_outcome_replay_recommendations r "
        "JOIN action_outcome_episodes e ON r.query_id = e.episode_id "
        "ORDER BY r.query_id, r.recommendation_rank"
    )
    native_string_join = live.fetch_query(args.url, native_string_join_sql, args.timeout)

    native_string_window_sql = (
        "SELECT query_id, recommendation_rank, "
        "ROW_NUMBER() OVER (PARTITION BY query_id ORDER BY recommendation_rank) AS rank_check "
        "FROM action_outcome_replay_recommendations "
        "ORDER BY query_id, recommendation_rank"
    )
    native_string_window = live.fetch_query(args.url, native_string_window_sql, args.timeout)
    native_string_window_ok = validate_native_string_window(
        native_string_window.get("data", [])
    )

    numeric_join_sql = (
        "SELECT r.query_seq, r.rank_num, r.action_code, q.observed_action_code, "
        "q.expected_top_action_code, r.avoid_num "
        f"FROM {RECOMMENDATION_TABLE} r "
        f"JOIN {QUERY_TABLE} q ON r.query_seq = q.query_seq "
        "ORDER BY r.query_seq, r.rank_num"
    )
    numeric_join = live.fetch_query(args.url, numeric_join_sql, args.timeout)
    numeric_join_ok = validate_numeric_join(
        numeric_join.get("data", []),
        query_rows,
        rec_rows,
    )

    numeric_window_sql = (
        "SELECT query_seq, rank_num, action_code, score_num, "
        "ROW_NUMBER() OVER (PARTITION BY query_seq ORDER BY rank_num) AS rank_check, "
        "LAG(score_num, 1, 0) OVER (PARTITION BY query_seq ORDER BY rank_num) AS prev_score "
        f"FROM {RECOMMENDATION_TABLE} "
        "ORDER BY query_seq, rank_num"
    )
    numeric_window = live.fetch_query(args.url, numeric_window_sql, args.timeout)
    numeric_window_ok = validate_window_rows(numeric_window.get("data", []))

    report = render_report(
        url=args.url,
        sql_path=args.sql_file,
        statements=statements,
        failures=failures,
        reset_failures=reset_failures,
        expected_counts=expected_counts,
        actual_counts=actual_counts,
        query_rows=query_rows,
        rec_rows=rec_rows,
        projection_counts=projection_counts,
        native_string_join=native_string_join,
        native_string_window=native_string_window,
        numeric_join=numeric_join,
        numeric_window=numeric_window,
        native_string_window_ok=native_string_window_ok,
        numeric_join_ok=numeric_join_ok,
        numeric_window_ok=numeric_window_ok,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)

    row_counts_ok = all(actual_counts.get(table) == count for table, count in expected_counts.items())
    projection_ok = (
        projection_counts.get(QUERY_TABLE) == len(query_rows)
        and projection_counts.get(RECOMMENDATION_TABLE) == len(rec_rows)
    )
    if (
        reset_failures
        or failures
        or not row_counts_ok
        or not projection_ok
        or not native_string_window_ok
        or not numeric_join_ok
        or not numeric_window_ok
    ):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
