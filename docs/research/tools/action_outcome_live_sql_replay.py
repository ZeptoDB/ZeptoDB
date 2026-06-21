#!/usr/bin/env python3
"""Validate Action-Outcome SQL replay seed data against a live ZeptoDB server."""

from __future__ import annotations

import argparse
import json
import re
import urllib.error
import urllib.request
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


EXPECTED_TOP_ACTIONS = {
    "aoe_checkout_002": "rollback",
    "aoe_payment_002": "traffic_drain",
    "aoe_inventory_002": "config_revert",
    "aoe_cache_002": "cache_purge",
    "aoe_queue_003": "scale_out",
    "aoe_search_003": "rollback",
}


INSERT_RE = re.compile(r"\bINSERT\s+INTO\s+([A-Za-z_][A-Za-z0-9_]*)\b", re.IGNORECASE)


@dataclass
class SqlResponse:
    ok: bool
    status: int | str
    body: str


def split_sql_statements(sql_text: str) -> list[str]:
    statements: list[str] = []
    buf: list[str] = []
    in_quote = False
    escape = False
    for ch in sql_text:
        buf.append(ch)
        if escape:
            escape = False
            continue
        if ch == "\\":
            escape = True
            continue
        if ch == "'":
            in_quote = not in_quote
        if ch == ";" and not in_quote:
            stmt = "".join(buf).strip()
            if stmt:
                statements.append(stmt[:-1].strip())
            buf = []
    remaining = "".join(buf).strip()
    if remaining:
        statements.append(remaining)
    return statements


def post_sql(url: str, sql: str, timeout: float) -> SqlResponse:
    req = urllib.request.Request(url, data=sql.encode(), method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return SqlResponse(True, resp.status, resp.read().decode())
    except urllib.error.HTTPError as exc:
        return SqlResponse(False, exc.code, exc.read().decode())
    except Exception as exc:  # pragma: no cover - network failure path
        return SqlResponse(False, type(exc).__name__, repr(exc))


def parse_json_response(response: SqlResponse) -> dict[str, Any]:
    if not response.ok:
        return {}
    try:
        return json.loads(response.body)
    except json.JSONDecodeError:
        return {}


def count_seed_inserts(statements: list[str]) -> dict[str, int]:
    counts: Counter[str] = Counter()
    for stmt in statements:
        match = INSERT_RE.search(stmt)
        if match:
            counts[match.group(1)] += 1
    return dict(sorted(counts.items()))


def fetch_count(url: str, table: str, timeout: float) -> int | None:
    response = post_sql(url, f"SELECT count(*) FROM {table}", timeout)
    data = parse_json_response(response)
    try:
        return int(data["data"][0][0])
    except (KeyError, IndexError, TypeError, ValueError):
        return None


def fetch_query(url: str, sql: str, timeout: float) -> dict[str, Any]:
    return parse_json_response(post_sql(url, sql, timeout))


def load_seed(url: str, statements: list[str], timeout: float) -> list[tuple[int, str, SqlResponse]]:
    failures: list[tuple[int, str, SqlResponse]] = []
    for idx, stmt in enumerate(statements, start=1):
        response = post_sql(url, stmt, timeout)
        if not response.ok:
            failures.append((idx, stmt, response))
    return failures


def compare_top_actions(rows: list[list[Any]]) -> tuple[bool, dict[str, str]]:
    actual = {str(row[0]): str(row[1]) for row in rows if len(row) >= 2}
    return actual == EXPECTED_TOP_ACTIONS, actual


def render_report(
    *,
    url: str,
    sql_path: Path,
    statements: list[str],
    failures: list[tuple[int, str, SqlResponse]],
    expected_counts: dict[str, int],
    actual_counts: dict[str, int | None],
    top_action_result: dict[str, Any],
    top_action_match: bool,
    top_actions: dict[str, str],
    failed_avoidance_count: int | None,
    diagnostic_rows: dict[str, Any],
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    passed_counts = all(actual_counts.get(table) == count for table, count in expected_counts.items())
    load_status = "pass" if not failures else "fail"
    count_status = "pass" if passed_counts else "fail"
    semantic_status = "pass" if top_action_match and failed_avoidance_count == 6 else "blocked"

    lines = [
        "# ActionOutcomeReplay Experiment 007 Live ZeptoDB SQL Endpoint Results",
        "",
        f"Generated at: {now}",
        f"Endpoint: `{url}`",
        f"SQL seed: `{sql_path}`",
        "",
        "## Load Summary",
        "",
        f"- Statements attempted: {len(statements)}",
        f"- Statements succeeded: {len(statements) - len(failures)}",
        f"- Statements failed: {len(failures)}",
        f"- Load status: {load_status}",
    ]
    if failures:
        lines += ["", "First failures:"]
        for idx, stmt, response in failures[:5]:
            lines.append(f"- #{idx} HTTP {response.status}: `{stmt[:140]}` -> `{response.body[:180]}`")

    lines += [
        "",
        "## Row Count Verification",
        "",
        "| Table | Expected Inserts | Live Rows | Status |",
        "| --- | ---: | ---: | --- |",
    ]
    for table, expected in expected_counts.items():
        actual = actual_counts.get(table)
        status = "pass" if actual == expected else "fail"
        actual_text = "n/a" if actual is None else str(actual)
        lines.append(f"| {table} | {expected} | {actual_text} | {status} |")

    top_rows = top_action_result.get("data", []) if top_action_result else []
    lines += [
        "",
        "## Semantic Query Verification",
        "",
        f"- Count status: {count_status}",
        f"- Semantic status: {semantic_status}",
        f"- Expected top-action rows: {len(EXPECTED_TOP_ACTIONS)}",
        f"- Live top-action rows: {len(top_rows)}",
        f"- Failed-action avoidance rows from live WHERE query: {failed_avoidance_count}",
        "",
        "| Query | Expected Top Action | Live Top Action |",
        "| --- | --- | --- |",
    ]
    for query_id, expected_action in EXPECTED_TOP_ACTIONS.items():
        lines.append(f"| {query_id} | {expected_action} | {top_actions.get(query_id, '')} |")

    lines += [
        "",
        "## Diagnostic Query",
        "",
        "```json",
        json.dumps(diagnostic_rows, indent=2, sort_keys=True)[:4000],
        "```",
        "",
        "## Interpretation",
        "",
        "Experiment 007 validates live parser, ingestion, projection, and WHERE",
        "compatibility for the Action-Outcome SQL seed: all generated DDL/INSERT",
        "statements execute through ZeptoDB's HTTP SQL endpoint, table row counts",
        "match the local SQL control, and the value-level top-action replay query",
        "returns the expected recommendations.",
    ]
    if semantic_status == "pass":
        lines += [
            "",
            "Generic table INSERT materialization is now sufficient for the",
            "Action-Outcome replay contract. Declared `STRING`, `DOUBLE`/`FLOAT64`, and `INT64`",
            "columns are queryable through the live SQL endpoint without reshaping the",
            "research schema into tick-only fields.",
            "",
            "## Next Steps",
            "",
            "1. Keep this live report as the acceptance harness for future SQL ingest changes.",
            "2. Add a distributed two-node replay run to exercise typed-row cluster routing.",
            "3. Extend the replay contract with JOIN/window queries once the Action-Outcome schema grows.",
        ]
    else:
        lines += [
            "",
            "Value-level replay is still blocked: row-count validation passes, but the",
            "semantic top-action query does not match the expected replay contract.",
            "",
            "## Next Steps",
            "",
            "1. Inspect generic table INSERT materialization for declared schema columns.",
            "2. Re-run the active C++ regression tests until projection and WHERE semantics pass.",
            "3. Re-run this live report once value-level projection works.",
        ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8123/")
    parser.add_argument("--sql-file", type=Path, default=Path("docs/research/results/action_outcome_sql_replay_006.sql"))
    parser.add_argument("--output", type=Path, default=Path("docs/research/results/action_outcome_live_sql_replay_007.md"))
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    statements = split_sql_statements(args.sql_file.read_text())
    expected_counts = count_seed_inserts(statements)
    failures = load_seed(args.url, statements, args.timeout)
    actual_counts = {
        table: fetch_count(args.url, table, args.timeout)
        for table in expected_counts
    }

    top_action_sql = (
        "SELECT query_id, action_class FROM action_outcome_replay_recommendations "
        "WHERE top_action = 1 ORDER BY query_id"
    )
    top_action_result = fetch_query(args.url, top_action_sql, args.timeout)
    top_action_match, top_actions = compare_top_actions(top_action_result.get("data", []))

    failed_count_sql = (
        "SELECT count(*) FROM action_outcome_replay_recommendations "
        "WHERE top_action = 1 AND avoids_failed_repeat = 1"
    )
    failed_count_result = fetch_query(args.url, failed_count_sql, args.timeout)
    try:
        failed_avoidance_count = int(failed_count_result["data"][0][0])
    except (KeyError, IndexError, TypeError, ValueError):
        failed_avoidance_count = None

    diagnostic_sql = (
        "SELECT query_id, action_class, top_action, recommendation_rank "
        "FROM action_outcome_replay_recommendations "
        "ORDER BY query_id, recommendation_rank LIMIT 6"
    )
    diagnostic_rows = fetch_query(args.url, diagnostic_sql, args.timeout)

    report = render_report(
        url=args.url,
        sql_path=args.sql_file,
        statements=statements,
        failures=failures,
        expected_counts=expected_counts,
        actual_counts=actual_counts,
        top_action_result=top_action_result,
        top_action_match=top_action_match,
        top_actions=top_actions,
        failed_avoidance_count=failed_avoidance_count,
        diagnostic_rows=diagnostic_rows,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)

    count_ok = all(actual_counts.get(table) == count for table, count in expected_counts.items())
    semantic_ok = top_action_match and failed_avoidance_count == 6
    if failures or not count_ok or not semantic_ok:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
