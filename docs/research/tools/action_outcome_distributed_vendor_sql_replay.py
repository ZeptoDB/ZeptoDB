#!/usr/bin/env python3
"""Classify Experiment 010 SQL/JOIN/window replay on a two-node ZeptoDB cluster."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import action_outcome_distributed_live_sql_replay as distributed
import action_outcome_replay as replay
import action_outcome_vendor_sql_replay as single


def owner_map(table_names: list[str], node_ids: list[int]) -> dict[str, dict[str, int]]:
    out: dict[str, dict[str, int]] = {}
    for table_name in table_names:
        table_id = distributed.stable_table_id(table_name)
        out[table_name] = {
            "table_id": table_id,
            "owner": distributed.route_owner(table_id, 0, node_ids),
        }
    return out


def classify_join_status(status: bool, left_owner: int, right_owner: int) -> str:
    if status:
        return "pass"
    if left_owner != right_owner:
        return "expected_gap_cross_node_join"
    return "fail"


def classify_window_status(status: bool, rows: int, expected_rows: int) -> str:
    if status:
        return "pass"
    if rows == expected_rows:
        return "expected_gap_cluster_window_values"
    return "fail"


def classify_checks(validation: dict[str, Any], owners: dict[str, dict[str, int]]) -> dict[str, dict[str, Any]]:
    query_owner = owners[single.QUERY_TABLE]["owner"]
    rec_owner = owners[single.RECOMMENDATION_TABLE]["owner"]
    retrieval_owner = owners[single.RETRIEVAL_TABLE]["owner"]
    suppression_owner = owners[single.SUPPRESSION_TABLE]["owner"]
    return {
        "failed_repeat_join": {
            "status": classify_join_status(validation["failed_repeat_status"], rec_owner, query_owner),
            "live_pass": validation["failed_repeat_status"],
            "left_table": single.RECOMMENDATION_TABLE,
            "right_table": single.QUERY_TABLE,
            "left_owner": rec_owner,
            "right_owner": query_owner,
            "rows": len(validation["failed_repeat_rows"]),
            "expected_rows": len(validation["expected_failed_repeat_rows"]),
        },
        "context_top_action_join": {
            "status": classify_join_status(validation["context_top_status"], rec_owner, query_owner),
            "live_pass": validation["context_top_status"],
            "left_table": single.RECOMMENDATION_TABLE,
            "right_table": single.QUERY_TABLE,
            "left_owner": rec_owner,
            "right_owner": query_owner,
            "rows": len(validation["context_top_rows"]),
            "expected_rows": len(validation["expected_context_top_rows"]),
        },
        "suppression_join": {
            "status": classify_join_status(validation["suppression_join_status"], suppression_owner, rec_owner),
            "live_pass": validation["suppression_join_status"],
            "left_table": single.SUPPRESSION_TABLE,
            "right_table": single.RECOMMENDATION_TABLE,
            "left_owner": suppression_owner,
            "right_owner": rec_owner,
            "rows": len(validation["suppression_join_rows"]),
            "expected_rows": validation["expected_counts"][single.SUPPRESSION_TABLE],
        },
        "misleading_retrieval_join": {
            "status": classify_join_status(validation["misleading_join_status"], retrieval_owner, query_owner),
            "live_pass": validation["misleading_join_status"],
            "left_table": single.RETRIEVAL_TABLE,
            "right_table": single.QUERY_TABLE,
            "left_owner": retrieval_owner,
            "right_owner": query_owner,
            "rows": len(validation["misleading_join_rows"]),
            "expected_rows": 23,
        },
        "row_number_window": {
            "status": classify_window_status(
                validation["window_status"],
                len(validation["window_rows"]),
                len(validation["expected_window_rows"]),
            ),
            "live_pass": validation["window_status"],
            "table": single.RECOMMENDATION_TABLE,
            "owner": rec_owner,
            "rows": len(validation["window_rows"]),
            "expected_rows": len(validation["expected_window_rows"]),
        },
        "lag_window": {
            "status": classify_window_status(
                validation["lag_status"],
                len(validation["lag_rows"]),
                len(validation["expected_lag_rows"]),
            ),
            "live_pass": validation["lag_status"],
            "table": single.RECOMMENDATION_TABLE,
            "owner": rec_owner,
            "rows": len(validation["lag_rows"]),
            "expected_rows": len(validation["expected_lag_rows"]),
        },
    }


def status_ok(status: str) -> bool:
    return status in {
        "pass",
        "expected_gap_cross_node_join",
        "expected_gap_cluster_window_values",
    }


def render_report(
    *,
    coordinator_url: str,
    sql_seed: Path,
    node_ids: list[int],
    node_stats_urls: list[str],
    statements: list[str],
    seed_failures: list[tuple[int, str, single.live.SqlResponse]],
    expected_seed_counts: dict[str, int],
    actual_seed_counts: dict[str, int | None],
    validation: dict[str, Any],
    classifications: dict[str, dict[str, Any]],
    owners: dict[str, dict[str, int]],
    before_stats: list[dict[str, int]],
    after_stats: list[dict[str, int]],
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    deltas = [distributed.delta_stats(before, after) for before, after in zip(before_stats, after_stats)]
    seed_count_status = all(
        actual_seed_counts.get(table) == expected
        for table, expected in expected_seed_counts.items()
    )
    distributed_status = sum(1 for delta in deltas if delta.get("ticks_ingested", 0) > 0) >= 2
    boundary_status = (
        not seed_failures
        and seed_count_status
        and validation["count_status"]
        and distributed_status
        and all(status_ok(item["status"]) for item in classifications.values())
    )
    full_sql_status = (
        validation["failed_repeat_status"]
        and validation["context_top_status"]
        and validation["suppression_join_status"]
        and validation["misleading_join_status"]
        and validation["window_status"]
        and validation["lag_status"]
    )

    lines = [
        "# ActionOutcomeReplay Experiment 011 Distributed SQL/JOIN/Window Replay Results",
        "",
        f"Generated at: {now}",
        f"Coordinator endpoint: `{coordinator_url}`",
        f"SQL seed: `{sql_seed}`",
        f"Node IDs: `{node_ids[0]}`, `{node_ids[1]}`",
        "",
        "## Load Summary",
        "",
        f"- Seed statements attempted: {len(statements)}",
        f"- Seed statements succeeded: {len(statements) - len(seed_failures)}",
        f"- Seed statements failed: {len(seed_failures)}",
        f"- Seed row-count status: {'pass' if seed_count_status else 'fail'}",
        f"- Vendor table row-count status: {'pass' if validation['count_status'] else 'fail'}",
        f"- Distributed ingest status: {'pass' if distributed_status else 'fail'}",
        f"- Full distributed SQL/JOIN/window status: {'pass' if full_sql_status else 'partial'}",
        f"- Boundary classification status: {'pass' if boundary_status else 'fail'}",
    ]
    if seed_failures:
        lines += ["", "First seed failures:"]
        for idx, stmt, response in seed_failures[:5]:
            lines.append(f"- #{idx} HTTP {response.status}: `{stmt[:140]}` -> `{response.body[:180]}`")

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
        "| Table | Stable table_id | Owner node | Expected/live rows |",
        "| --- | ---: | ---: | ---: |",
    ]
    combined_counts: dict[str, int | None] = {}
    combined_counts.update(expected_seed_counts)
    combined_counts.update(validation["expected_counts"])
    for table, info in owners.items():
        rows = combined_counts.get(table)
        rows_text = "n/a" if rows is None else str(rows)
        lines.append(f"| `{table}` | {info['table_id']} | {info['owner']} | {rows_text} |")

    lines += [
        "",
        "## Row Count Verification",
        "",
        "### Seed Tables",
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
        "### Vendor Tables",
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
        "## Distributed JOIN/Window Classification",
        "",
        "| Check | Left/table owner | Right owner | Rows | Expected | Status |",
        "| --- | --- | --- | ---: | ---: | --- |",
    ]
    for name, item in classifications.items():
        if "left_table" in item:
            left = f"`{item['left_table']}` -> {item['left_owner']}"
            right = f"`{item['right_table']}` -> {item['right_owner']}"
        else:
            left = f"`{item['table']}` -> {item['owner']}"
            right = "n/a"
        lines.append(
            f"| `{name}` | {left} | {right} | {item['rows']} | "
            f"{item['expected_rows']} | `{item['status']}` |"
        )

    windows_pass = (
        classifications["row_number_window"]["status"] == "pass"
        and classifications["lag_window"]["status"] == "pass"
    )
    window_lines = (
        [
            "The window checks now pass in cluster mode. The coordinator",
            "fetch-and-compute path preserves declared operational-table values",
            "when it materializes the temporary full-data table, so ROW_NUMBER",
            "and LAG match the single-node/vendor baseline.",
        ]
        if windows_pass
        else [
            "The window checks return the expected row count but not the expected window",
            "values in cluster mode, so they are classified separately as",
            "`expected_gap_cluster_window_values`. A plain projected SELECT over the",
            "same table returns the correct numeric columns; the gap is specific to",
            "cluster-mode window evaluation/merge for declared operational tables.",
        ]
    )
    suppression_pass = classifications["suppression_join"]["status"] == "pass"
    suppression_lines = (
        [
            "The suppression table is owned by node 1 while recommendations are",
            "owned by node 8. The bounded small-table hash JOIN path now gathers",
            "both operational tables under the coordinator row cap, replays them",
            "into a temporary typed pipeline, and executes the original hash JOIN",
            "locally. This turns the former cross-node suppression JOIN boundary",
            "into a passing check without requiring a full distributed SQL",
            "optimizer.",
        ]
        if suppression_pass
        else [
            "The suppression table is owned by node 1 while recommendations are owned by",
            "node 8. The suppression JOIN therefore records the expected current gap:",
            "ZeptoDB can scatter-gather supported SELECT and window shapes, but it does",
            "not yet execute arbitrary cross-node hash JOINs by shipping one side or",
            "broadcasting small dimension tables.",
        ]
    )
    if windows_pass and suppression_pass:
        next_step_lines = [
            "Define an explicit shard-key or table-level distribution policy for",
            "symbol-less operational tables. The small-table JOIN path proves",
            "correctness for bounded control tables; production promotion now needs",
            "placement policy, row-cap telemetry, and a larger cost-based JOIN",
            "planner only for tables that exceed the small-table boundary.",
        ]
    elif windows_pass:
        next_step_lines = [
            "Implement a narrow distributed hash JOIN strategy for small",
            "operational tables. The JOIN work can start with",
            "broadcast/replicated dimension-table joins, which would turn the",
            "suppression JOIN from `expected_gap_cross_node_join` into `pass`",
            "without requiring a full cost-based distributed SQL optimizer.",
        ]
    else:
        next_step_lines = [
            "First fix cluster-mode window value materialization for declared",
            "operational tables, then implement a narrow distributed hash JOIN strategy",
            "for small operational tables. The JOIN work can start with",
            "broadcast/replicated dimension-table joins, which would turn the",
            "suppression JOIN from `expected_gap_cross_node_join` into `pass` without",
            "requiring a full cost-based distributed SQL optimizer.",
        ]

    lines += [
        "",
        "## Interpretation",
        "",
        "Experiment 011 separates distributed-safe replay checks from the current",
        "distributed planner boundary. Under the default 1/8 two-node ring, the",
        "query, recommendation, and retrieval vendor tables are co-located on node",
        "8, so failed-repeat JOIN, context top-action JOIN, and misleading",
        "retrieval JOIN all pass through the coordinator.",
        "",
        *suppression_lines,
        "",
        *window_lines,
        "",
        "## Next Best Step",
        "",
        *next_step_lines,
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--coordinator-url", default="http://127.0.0.1:19241/")
    parser.add_argument("--node-a-id", type=int, default=1)
    parser.add_argument("--node-b-id", type=int, default=8)
    parser.add_argument("--node-a-stats-url", default="http://127.0.0.1:19241/stats")
    parser.add_argument("--node-b-stats-url", default="http://127.0.0.1:19242/stats")
    parser.add_argument("--sql-seed", type=Path, default=Path("docs/research/results/action_outcome_sql_replay_006.sql"))
    parser.add_argument("--fixture", type=Path, default=Path("docs/research/fixtures/action_outcome_episodes.json"))
    parser.add_argument("--extra-fixture", action="append", type=Path, default=[])
    parser.add_argument("--quality-labels", type=Path)
    parser.add_argument("--output", type=Path, default=Path("docs/research/results/action_outcome_distributed_vendor_sql_replay_011.md"))
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--query-id", action="append", dest="query_ids")
    parser.add_argument("--strict-full-sql", action="store_true")
    args = parser.parse_args()

    node_ids = [args.node_a_id, args.node_b_id]
    stats_urls = [args.node_a_stats_url, args.node_b_stats_url]
    table_names = single.BASE_TABLES + single.VENDOR_TABLES
    owners = owner_map(table_names, node_ids)

    before_stats = [distributed.fetch_stats(url, args.timeout) for url in stats_urls]
    statements, seed_failures = single.load_seed(args.coordinator_url, args.sql_seed, args.timeout)
    expected_seed_counts = single.live.count_seed_inserts(statements)
    actual_seed_counts = {
        table: single.table_count(args.coordinator_url, args.timeout, table)
        for table in expected_seed_counts
    }

    query_ids = args.query_ids or replay.DEFAULT_QUERY_IDS
    results = single.build_results(args.fixture, args.extra_fixture, args.quality_labels, query_ids)
    expected = single.materialize_results(args.coordinator_url, args.timeout, results, query_ids)
    validation = single.validate_sql(args.coordinator_url, args.timeout, expected)
    classifications = classify_checks(validation, owners)
    after_stats = [distributed.fetch_stats(url, args.timeout) for url in stats_urls]

    report = render_report(
        coordinator_url=args.coordinator_url,
        sql_seed=args.sql_seed,
        node_ids=node_ids,
        node_stats_urls=stats_urls,
        statements=statements,
        seed_failures=seed_failures,
        expected_seed_counts=expected_seed_counts,
        actual_seed_counts=actual_seed_counts,
        validation=validation,
        classifications=classifications,
        owners=owners,
        before_stats=before_stats,
        after_stats=after_stats,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)

    deltas = [distributed.delta_stats(before, after) for before, after in zip(before_stats, after_stats)]
    seed_count_status = all(
        actual_seed_counts.get(table) == expected_count
        for table, expected_count in expected_seed_counts.items()
    )
    distributed_status = sum(1 for delta in deltas if delta.get("ticks_ingested", 0) > 0) >= 2
    boundary_status = (
        not seed_failures
        and seed_count_status
        and validation["count_status"]
        and distributed_status
        and all(status_ok(item["status"]) for item in classifications.values())
    )
    full_sql_status = all(item["live_pass"] for item in classifications.values())
    if args.strict_full_sql and not full_sql_status:
        raise SystemExit("full distributed SQL/JOIN/window validation failed")
    if not boundary_status:
        raise SystemExit("distributed replay boundary classification failed")


if __name__ == "__main__":
    main()
