#!/usr/bin/env python3
"""Validate Action-Outcome replay against a two-node live ZeptoDB cluster."""

from __future__ import annotations

import argparse
import json
import urllib.request
from bisect import bisect_left
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import action_outcome_live_sql_replay as live


def stable_table_id(name: str) -> int:
    h = 2166136261
    for b in name.encode():
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return (h % 65535) + 1


def symbol_hash(symbol: int) -> int:
    h = 14695981039346656037
    for b in (symbol & 0xFFFFFFFF).to_bytes(4, "little"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def vnode_hash(node_id: int, idx: int) -> int:
    key = ((node_id & 0xFFFFFFFF) << 32) | (idx & 0xFFFFFFFF)
    key = (key + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    key = ((key ^ (key >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    key = ((key ^ (key >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    return key ^ (key >> 31)


def route_owner(table_id: int, symbol_id: int, node_ids: list[int]) -> int:
    ring = sorted(
        (vnode_hash(node_id, vnode_idx), node_id)
        for node_id in node_ids
        for vnode_idx in range(128)
    )
    combined = ((symbol_id & 0xFFFFFFFF) ^ ((table_id << 16) & 0xFFFFFFFF)) & 0xFFFFFFFF
    h = symbol_hash(combined)
    idx = bisect_left(ring, (h, -1))
    if idx == len(ring):
        idx = 0
    return ring[idx][1]


def fetch_stats(url: str, timeout: float) -> dict[str, int]:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            data = json.loads(resp.read().decode())
    except Exception:
        return {}
    out: dict[str, int] = {}
    for key in ("ticks_ingested", "ticks_stored", "ticks_dropped", "partitions_created"):
        try:
            out[key] = int(data.get(key, 0))
        except (TypeError, ValueError):
            out[key] = 0
    return out


def delta_stats(before: dict[str, int], after: dict[str, int]) -> dict[str, int]:
    keys = set(before) | set(after)
    return {key: after.get(key, 0) - before.get(key, 0) for key in sorted(keys)}


def render_report(
    *,
    coordinator_url: str,
    sql_path: Path,
    statements: list[str],
    failures: list[tuple[int, str, live.SqlResponse]],
    expected_counts: dict[str, int],
    actual_counts: dict[str, int | None],
    top_action_result: dict[str, Any],
    top_action_match: bool,
    top_actions: dict[str, str],
    failed_avoidance_count: int | None,
    remote_string_result: dict[str, Any],
    remote_string_ok: bool,
    node_ids: list[int],
    node_stats_urls: list[str],
    before_stats: list[dict[str, int]],
    after_stats: list[dict[str, int]],
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    deltas = [delta_stats(before, after) for before, after in zip(before_stats, after_stats)]
    row_counts_pass = all(actual_counts.get(table) == count for table, count in expected_counts.items())
    semantic_pass = top_action_match and failed_avoidance_count == 6
    distributed_pass = sum(1 for delta in deltas if delta.get("ticks_ingested", 0) > 0) >= 2

    owner_rows: list[tuple[str, int, int, int]] = []
    for table, count in expected_counts.items():
        table_id = stable_table_id(table)
        owner = route_owner(table_id, 0, node_ids)
        owner_rows.append((table, table_id, owner, count))

    lines = [
        "# ActionOutcomeReplay Experiment 008 Distributed Live ZeptoDB Results",
        "",
        f"Generated at: {now}",
        f"Coordinator endpoint: `{coordinator_url}`",
        f"SQL seed: `{sql_path}`",
        f"Node IDs: `{node_ids[0]}`, `{node_ids[1]}`",
        "",
        "## Load Summary",
        "",
        f"- Statements attempted: {len(statements)}",
        f"- Statements succeeded: {len(statements) - len(failures)}",
        f"- Statements failed: {len(failures)}",
        f"- Load status: {'pass' if not failures else 'fail'}",
        f"- Row-count status: {'pass' if row_counts_pass else 'fail'}",
        f"- Semantic status: {'pass' if semantic_pass else 'fail'}",
        f"- Distributed ingest status: {'pass' if distributed_pass else 'fail'}",
        f"- Remote decoded string status: {'pass' if remote_string_ok else 'fail'}",
    ]
    if failures:
        lines += ["", "First failures:"]
        for idx, stmt, response in failures[:5]:
            lines.append(
                f"- #{idx} HTTP {response.status}: `{stmt[:140]}` -> `{response.body[:180]}`"
            )

    lines += [
        "",
        "## Node-Local Stats Delta",
        "",
        "| Node ID | Stats URL | ticks_ingested | ticks_stored | ticks_dropped | partitions_created |",
        "| ---: | --- | ---: | ---: | ---: | ---: |",
    ]
    for node_id, stats_url, delta in zip(node_ids, node_stats_urls, deltas):
        lines.append(
            f"| {node_id} | `{stats_url}` | {delta.get('ticks_ingested', 0)} | "
            f"{delta.get('ticks_stored', 0)} | {delta.get('ticks_dropped', 0)} | "
            f"{delta.get('partitions_created', 0)} |"
        )

    lines += [
        "",
        "## Owner Split",
        "",
        "| Table | Stable table_id | Owner node | Expected inserts |",
        "| --- | ---: | ---: | ---: |",
    ]
    for table, table_id, owner, count in owner_rows:
        lines.append(f"| `{table}` | {table_id} | {owner} | {count} |")

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
        lines.append(f"| `{table}` | {expected} | {actual_text} | {status} |")

    lines += [
        "",
        "## Semantic Query Verification",
        "",
        f"- Expected top-action rows: {len(live.EXPECTED_TOP_ACTIONS)}",
        f"- Live top-action rows: {len(top_action_result.get('data', [])) if top_action_result else 0}",
        f"- Failed-action avoidance rows: {failed_avoidance_count}",
        "",
        "| Query | Expected Top Action | Live Top Action |",
        "| --- | --- | --- |",
    ]
    for query_id, expected_action in live.EXPECTED_TOP_ACTIONS.items():
        lines.append(f"| `{query_id}` | `{expected_action}` | `{top_actions.get(query_id, '')}` |")

    lines += [
        "",
        "## Remote String Query Verification",
        "",
        "`action_outcome_episodes` is owned by node 8 in the 1/8 ring, so this",
        "query verifies that remote RPC results preserve decoded `STRING` values:",
        "",
        "```json",
        json.dumps(remote_string_result, indent=2, sort_keys=True)[:2000],
        "```",
        "",
        f"- Remote decoded string status: {'pass' if remote_string_ok else 'fail'}",
    ]

    lines += [
        "",
        "## Interpretation",
        "",
        "Experiment 008 validates the Action-Outcome replay seed through a real",
        "two-node ZeptoDB HTTP/RPC topology. The node-local stats prove rows landed",
        "on both nodes, while cluster SELECT row counts and semantic top-action",
        "queries still match the single-node replay contract.",
        "",
        "This run also covers the distributed string-result boundary: `STRING` and",
        "`SYMBOL` columns must survive RPC serialization and concat merge as decoded",
        "values, not node-local dictionary codes. Devlog 188 adds the CI-sized C++",
        "regression and typed-row string-payload hardening for this boundary.",
        "",
        "## Next Steps",
        "",
        "1. Extend Experiment 008 with JOIN/window replay queries across",
        "   `action_outcome_episodes` and recommendation tables.",
        "2. Add a shard-key policy for symbol-less operational tables so production",
        "   two-node splits do not depend on node-id choice.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--coordinator-url", default="http://127.0.0.1:19241/")
    parser.add_argument("--node-a-id", type=int, default=1)
    parser.add_argument("--node-b-id", type=int, default=8)
    parser.add_argument("--node-a-stats-url", default="http://127.0.0.1:19241/stats")
    parser.add_argument("--node-b-stats-url", default="http://127.0.0.1:19242/stats")
    parser.add_argument("--sql-file", type=Path, default=Path("docs/research/results/action_outcome_sql_replay_006.sql"))
    parser.add_argument("--output", type=Path, default=Path("docs/research/results/action_outcome_distributed_live_sql_replay_008.md"))
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    node_ids = [args.node_a_id, args.node_b_id]
    stats_urls = [args.node_a_stats_url, args.node_b_stats_url]
    before_stats = [fetch_stats(url, args.timeout) for url in stats_urls]

    statements = live.split_sql_statements(args.sql_file.read_text())
    expected_counts = live.count_seed_inserts(statements)
    failures = live.load_seed(args.coordinator_url, statements, args.timeout)
    actual_counts = {
        table: live.fetch_count(args.coordinator_url, table, args.timeout)
        for table in expected_counts
    }

    top_action_sql = (
        "SELECT query_id, action_class FROM action_outcome_replay_recommendations "
        "WHERE top_action = 1 ORDER BY query_id"
    )
    top_action_result = live.fetch_query(args.coordinator_url, top_action_sql, args.timeout)
    top_action_match, top_actions = live.compare_top_actions(top_action_result.get("data", []))

    failed_count_sql = (
        "SELECT count(*) FROM action_outcome_replay_recommendations "
        "WHERE top_action = 1 AND avoids_failed_repeat = 1"
    )
    failed_count_result = live.fetch_query(args.coordinator_url, failed_count_sql, args.timeout)
    try:
        failed_avoidance_count = int(failed_count_result["data"][0][0])
    except (KeyError, IndexError, TypeError, ValueError):
        failed_avoidance_count = None

    remote_string_sql = (
        "SELECT episode_id, action_class FROM action_outcome_episodes "
        "ORDER BY episode_id LIMIT 3"
    )
    remote_string_result = live.fetch_query(args.coordinator_url, remote_string_sql, args.timeout)
    remote_string_rows = remote_string_result.get("data", [])
    remote_string_ok = (
        len(remote_string_rows) == 3
        and remote_string_rows[0] == ["aoe_checkout_001", "rollback"]
        and isinstance(remote_string_rows[1][0], str)
        and isinstance(remote_string_rows[1][1], str)
    )

    after_stats = [fetch_stats(url, args.timeout) for url in stats_urls]

    report = render_report(
        coordinator_url=args.coordinator_url,
        sql_path=args.sql_file,
        statements=statements,
        failures=failures,
        expected_counts=expected_counts,
        actual_counts=actual_counts,
        top_action_result=top_action_result,
        top_action_match=top_action_match,
        top_actions=top_actions,
        failed_avoidance_count=failed_avoidance_count,
        remote_string_result=remote_string_result,
        remote_string_ok=remote_string_ok,
        node_ids=node_ids,
        node_stats_urls=stats_urls,
        before_stats=before_stats,
        after_stats=after_stats,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)

    deltas = [delta_stats(before, after) for before, after in zip(before_stats, after_stats)]
    distributed_ok = sum(1 for delta in deltas if delta.get("ticks_ingested", 0) > 0) >= 2
    count_ok = all(actual_counts.get(table) == count for table, count in expected_counts.items())
    semantic_ok = top_action_match and failed_avoidance_count == 6
    if failures or not count_ok or not semantic_ok or not distributed_ok or not remote_string_ok:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
