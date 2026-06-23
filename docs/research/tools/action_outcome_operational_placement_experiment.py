#!/usr/bin/env python3
"""Run Experiment 012 explicit operational placement + JOIN telemetry replay."""

from __future__ import annotations

import argparse
import json
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import action_outcome_distributed_live_sql_replay as distributed
import action_outcome_distributed_vendor_sql_replay as exp011
import action_outcome_replay as replay
import action_outcome_vendor_sql_replay as single


PLACEMENTS = [
    (
        single.QUERY_TABLE,
        "pinned_node",
        "node_b",
        "query controls co-located with recommendations",
    ),
    (
        single.RECOMMENDATION_TABLE,
        "pinned_node",
        "node_b",
        "action priors and recommendations",
    ),
    (single.RETRIEVAL_TABLE, "pinned_node", "node_b", "retrieval evidence"),
    (
        single.SUPPRESSION_TABLE,
        "pinned_node",
        "node_a",
        "bounded suppression/control table",
    ),
]

JOIN_METRIC_NAMES = [
    "zepto_small_table_join_candidates_total",
    "zepto_small_table_join_accepted_total",
    "zepto_small_table_join_row_cap_rejections_total",
    "zepto_small_table_join_errors_total",
    "zepto_small_table_join_rows_materialized_total",
    "zepto_small_table_join_last_left_rows",
    "zepto_small_table_join_last_right_rows",
]


def endpoint(base: str, path: str) -> str:
    return urllib.parse.urljoin(base.rstrip("/") + "/", path.lstrip("/"))


def request_json(
    url: str,
    *,
    method: str = "GET",
    body: dict[str, Any] | None = None,
    timeout: float,
    admin_token: str | None = None,
) -> tuple[int | str, dict[str, Any]]:
    data = None if body is None else json.dumps(body).encode()
    headers = {"Content-Type": "application/json"} if body is not None else {}
    if admin_token:
        headers["Authorization"] = f"Bearer {admin_token}"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode()
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            parsed = {"error": raw}
        return exc.code, parsed
    except Exception as exc:  # pragma: no cover - live harness failure path
        return type(exc).__name__, {"error": repr(exc)}


def request_text(url: str, timeout: float) -> str:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            return resp.read().decode()
    except Exception:
        return ""


def fetch_stats_full(url: str, timeout: float) -> dict[str, Any]:
    status, data = request_json(url, timeout=timeout)
    if status != 200:
        return {}
    return data


def numeric_stats(data: dict[str, Any]) -> dict[str, int]:
    out: dict[str, int] = {}
    for key in ("ticks_ingested", "ticks_stored", "ticks_dropped", "partitions_created"):
        try:
            out[key] = int(data.get(key, 0))
        except (TypeError, ValueError):
            out[key] = 0
    return out


def small_join_stats(data: dict[str, Any]) -> dict[str, int]:
    raw = data.get("small_table_join", {}) if isinstance(data, dict) else {}
    out: dict[str, int] = {}
    for key in (
        "candidates",
        "accepted",
        "rejected_row_cap",
        "errors",
        "rows_materialized",
        "last_left_rows",
        "last_right_rows",
    ):
        try:
            out[key] = int(raw.get(key, 0))
        except (AttributeError, TypeError, ValueError):
            out[key] = 0
    return out


def delta(before: dict[str, int], after: dict[str, int]) -> dict[str, int]:
    keys = set(before) | set(after)
    return {key: after.get(key, 0) - before.get(key, 0) for key in sorted(keys)}


def parse_prometheus_metrics(text: str) -> dict[str, float]:
    values: dict[str, float] = {}
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        name = parts[0]
        if name not in JOIN_METRIC_NAMES:
            continue
        try:
            values[name] = float(parts[1])
        except ValueError:
            continue
    return values


def placement_node(role: str, node_ids: list[int]) -> int:
    return node_ids[0] if role == "node_a" else node_ids[1]


def apply_placements(
    *,
    coordinator_url: str,
    node_ids: list[int],
    timeout: float,
    admin_token: str | None,
) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    url = endpoint(coordinator_url, "/admin/table-placement")
    for table, policy, node_role, purpose in PLACEMENTS:
        node_id = placement_node(node_role, node_ids)
        body = {"table": table, "policy": policy, "node_id": node_id}
        status, response = request_json(
            url,
            method="POST",
            body=body,
            timeout=timeout,
            admin_token=admin_token,
        )
        results.append(
            {
                "table": table,
                "policy": policy,
                "node_id": node_id,
                "purpose": purpose,
                "status": status,
                "response": response,
                "ok": status == 200 and bool(response.get("ok")),
            }
        )
    return results


def policy_owner_map(table_names: list[str], node_ids: list[int]) -> dict[str, dict[str, int]]:
    owners = exp011.owner_map(table_names, node_ids)
    for table, _policy, node_role, _purpose in PLACEMENTS:
        owners[table]["owner"] = placement_node(node_role, node_ids)
    return owners


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
    placement_results: list[dict[str, Any]],
    before_stats: list[dict[str, Any]],
    after_stats: list[dict[str, Any]],
    before_join_stats: dict[str, int],
    after_join_stats: dict[str, int],
    prometheus_metrics: dict[str, float],
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    stat_deltas = [
        distributed.delta_stats(numeric_stats(before), numeric_stats(after))
        for before, after in zip(before_stats, after_stats)
    ]
    join_delta = delta(before_join_stats, after_join_stats)
    seed_count_status = all(
        actual_seed_counts.get(table) == expected
        for table, expected in expected_seed_counts.items()
    )
    distributed_status = sum(
        1 for item in stat_deltas if item.get("ticks_ingested", 0) > 0
    ) >= 2
    placement_status = all(item["ok"] for item in placement_results)
    full_sql_status = all(item["live_pass"] for item in classifications.values())
    telemetry_status = (
        join_delta.get("candidates", 0) >= 4
        and join_delta.get("accepted", 0) >= 4
        and join_delta.get("rejected_row_cap", 0) == 0
        and join_delta.get("errors", 0) == 0
        and join_delta.get("rows_materialized", 0) > 0
    )
    metrics_status = all(name in prometheus_metrics for name in JOIN_METRIC_NAMES)
    overall_status = (
        not seed_failures
        and seed_count_status
        and validation["count_status"]
        and placement_status
        and distributed_status
        and full_sql_status
        and telemetry_status
        and metrics_status
    )

    lines = [
        "# ActionOutcomeReplay Experiment 012 Operational Placement Results",
        "",
        f"Generated at: {now}",
        f"Coordinator endpoint: `{coordinator_url}`",
        f"SQL seed: `{sql_seed}`",
        f"Node IDs: `{node_ids[0]}`, `{node_ids[1]}`",
        "",
        "## Status",
        "",
        f"- Explicit placement policy status: {'pass' if placement_status else 'fail'}",
        f"- Seed row-count status: {'pass' if seed_count_status else 'fail'}",
        f"- Vendor table row-count status: {'pass' if validation['count_status'] else 'fail'}",
        f"- Distributed ingest status: {'pass' if distributed_status else 'fail'}",
        f"- Full SQL/JOIN/window status: {'pass' if full_sql_status else 'fail'}",
        f"- Small-table JOIN telemetry status: {'pass' if telemetry_status else 'fail'}",
        f"- Prometheus telemetry status: {'pass' if metrics_status else 'fail'}",
        f"- Overall Experiment 012 status: {'pass' if overall_status else 'fail'}",
    ]
    if seed_failures:
        lines += ["", "First seed failures:"]
        for idx, stmt, response in seed_failures[:5]:
            lines.append(
                f"- #{idx} HTTP {response.status}: `{stmt[:140]}` -> `{response.body[:180]}`"
            )

    lines += [
        "",
        "## Placement Policy",
        "",
        "| Table | Policy | Node | Purpose | API Status |",
        "| --- | --- | ---: | --- | --- |",
    ]
    for item in placement_results:
        response_json = json.dumps(item["response"], sort_keys=True)
        lines.append(
            f"| `{item['table']}` | `{item['policy']}` | {item['node_id']} | "
            f"{item['purpose']} | `{item['status']}` / `{response_json}` |"
        )

    lines += [
        "",
        "## Node-Local Stats Delta",
        "",
        "| Node ID | Stats URL | ticks_ingested | ticks_stored | ticks_dropped | partitions_created |",
        "| ---: | --- | ---: | ---: | ---: | ---: |",
    ]
    for node_id, stats_url, item in zip(node_ids, node_stats_urls, stat_deltas):
        lines.append(
            f"| {node_id} | `{stats_url}` | {item.get('ticks_ingested', 0)} | "
            f"{item.get('ticks_stored', 0)} | {item.get('ticks_dropped', 0)} | "
            f"{item.get('partitions_created', 0)} |"
        )

    lines += [
        "",
        "## Owner Map After Policy",
        "",
        "| Table | Stable table_id | Owner node | Rows |",
        "| --- | ---: | ---: | ---: |",
    ]
    combined_counts: dict[str, int | None] = {}
    combined_counts.update(expected_seed_counts)
    combined_counts.update(validation["expected_counts"])
    for table, info in owners.items():
        rows = combined_counts.get(table)
        rows_text = "n/a" if rows is None else str(rows)
        lines.append(
            f"| `{table}` | {info['table_id']} | {info['owner']} | {rows_text} |"
        )

    lines += [
        "",
        "## JOIN/Window Checks",
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

    lines += [
        "",
        "## Small-Table JOIN Telemetry",
        "",
        "| Counter/Gauge | Before | After | Delta |",
        "| --- | ---: | ---: | ---: |",
    ]
    for key in sorted(set(before_join_stats) | set(after_join_stats)):
        lines.append(
            f"| `{key}` | {before_join_stats.get(key, 0)} | "
            f"{after_join_stats.get(key, 0)} | {join_delta.get(key, 0)} |"
        )

    lines += [
        "",
        "## Prometheus Metrics",
        "",
        "| Metric | Value |",
        "| --- | ---: |",
    ]
    for name in JOIN_METRIC_NAMES:
        value = prometheus_metrics.get(name)
        value_text = "missing" if value is None else f"{value:g}"
        lines.append(f"| `{name}` | {value_text} |")

    lines += [
        "",
        "## Interpretation",
        "",
        "Experiment 012 turns operational-table distribution from an implicit",
        "table-id hash side effect into an explicit control-plane policy. The",
        "Action-Outcome query, recommendation, and retrieval tables are pinned to",
        "node 8 while the bounded suppression/control table is pinned to node 1.",
        "This deliberately keeps the suppression JOIN cross-node while preserving",
        "correctness through the bounded small-table hash JOIN path.",
        "",
        "The commercial value is observability plus a narrow guarantee: operators",
        "can place small Action-Outcome control tables intentionally, then verify",
        "that coordinator-local JOIN replay stayed inside the row cap and did not",
        "silently fall back to an unbounded distributed planner.",
        "",
        "## Next Best Step",
        "",
        "Promote placement policy from an admin-only runtime knob to a persisted",
        "table option or catalog record, then add a row-cap alerting example for",
        "`zepto_small_table_join_row_cap_rejections_total`.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--coordinator-url", default="http://127.0.0.1:19241/")
    parser.add_argument("--node-a-id", type=int, default=1)
    parser.add_argument("--node-b-id", type=int, default=8)
    parser.add_argument("--node-a-stats-url", default="http://127.0.0.1:19241/stats")
    parser.add_argument("--node-b-stats-url", default="http://127.0.0.1:19242/stats")
    parser.add_argument("--metrics-url", default="http://127.0.0.1:19241/metrics")
    parser.add_argument("--sql-seed", type=Path, default=Path("docs/research/results/action_outcome_sql_replay_006.sql"))
    parser.add_argument("--fixture", type=Path, default=Path("docs/research/fixtures/action_outcome_episodes.json"))
    parser.add_argument("--extra-fixture", action="append", type=Path, default=[])
    parser.add_argument("--quality-labels", type=Path)
    parser.add_argument("--output", type=Path, default=Path("docs/research/results/action_outcome_operational_placement_012.md"))
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--query-id", action="append", dest="query_ids")
    parser.add_argument("--admin-token")
    args = parser.parse_args()

    node_ids = [args.node_a_id, args.node_b_id]
    stats_urls = [args.node_a_stats_url, args.node_b_stats_url]
    table_names = single.BASE_TABLES + single.VENDOR_TABLES
    owners = policy_owner_map(table_names, node_ids)

    before_stats = [fetch_stats_full(url, args.timeout) for url in stats_urls]
    statements, seed_failures = single.load_seed(
        args.coordinator_url, args.sql_seed, args.timeout
    )
    expected_seed_counts = single.live.count_seed_inserts(statements)
    actual_seed_counts = {
        table: single.table_count(args.coordinator_url, args.timeout, table)
        for table in expected_seed_counts
    }

    query_ids = args.query_ids or replay.DEFAULT_QUERY_IDS
    results = single.build_results(
        args.fixture,
        args.extra_fixture,
        args.quality_labels,
        query_ids,
    )
    placement_results: list[dict[str, Any]] = []

    def placement_callback() -> None:
        nonlocal placement_results
        placement_results = apply_placements(
            coordinator_url=args.coordinator_url,
            node_ids=node_ids,
            timeout=args.timeout,
            admin_token=args.admin_token,
        )
        failed = [item for item in placement_results if not item["ok"]]
        if failed:
            raise RuntimeError(f"placement policy update failed: {failed}")

    expected = single.materialize_results(
        args.coordinator_url,
        args.timeout,
        results,
        query_ids,
        after_create_tables=placement_callback,
    )
    before_join_stats = small_join_stats(
        fetch_stats_full(args.node_a_stats_url, args.timeout)
    )
    validation = single.validate_sql(args.coordinator_url, args.timeout, expected)
    classifications = exp011.classify_checks(validation, owners)
    after_join_stats = small_join_stats(
        fetch_stats_full(args.node_a_stats_url, args.timeout)
    )
    after_stats = [fetch_stats_full(url, args.timeout) for url in stats_urls]
    prometheus_metrics = parse_prometheus_metrics(
        request_text(args.metrics_url, args.timeout)
    )

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
        placement_results=placement_results,
        before_stats=before_stats,
        after_stats=after_stats,
        before_join_stats=before_join_stats,
        after_join_stats=after_join_stats,
        prometheus_metrics=prometheus_metrics,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)

    stat_deltas = [
        distributed.delta_stats(numeric_stats(before), numeric_stats(after))
        for before, after in zip(before_stats, after_stats)
    ]
    join_delta = delta(before_join_stats, after_join_stats)
    seed_count_status = all(
        actual_seed_counts.get(table) == expected_count
        for table, expected_count in expected_seed_counts.items()
    )
    distributed_status = sum(
        1 for item in stat_deltas if item.get("ticks_ingested", 0) > 0
    ) >= 2
    full_sql_status = all(item["live_pass"] for item in classifications.values())
    telemetry_status = (
        join_delta.get("candidates", 0) >= 4
        and join_delta.get("accepted", 0) >= 4
        and join_delta.get("rejected_row_cap", 0) == 0
        and join_delta.get("errors", 0) == 0
        and join_delta.get("rows_materialized", 0) > 0
    )
    metrics_status = all(name in prometheus_metrics for name in JOIN_METRIC_NAMES)
    if not (
        not seed_failures
        and seed_count_status
        and validation["count_status"]
        and all(item["ok"] for item in placement_results)
        and distributed_status
        and full_sql_status
        and telemetry_status
        and metrics_status
    ):
        raise SystemExit("Experiment 012 operational placement validation failed")


if __name__ == "__main__":
    main()
