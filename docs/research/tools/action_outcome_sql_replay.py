#!/usr/bin/env python3
"""SQL-backed replay harness for Action-Outcome Memory research.

The generated SQL uses ZeptoDB-compatible types. The local executor uses sqlite3
only to validate the SQL-shaped schema and replay contract without requiring a
running ZeptoDB server.
"""

from __future__ import annotations

import argparse
import json
import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

import action_outcome_ablation as ablation
import action_outcome_context_gate as context_gate
import action_outcome_replay as replay


DDL = {
    "action_outcome_episodes": """
CREATE TABLE IF NOT EXISTS action_outcome_episodes (
    episode_id STRING,
    incident_id STRING,
    incident_type STRING,
    service STRING,
    environment STRING,
    entity_refs_json STRING,
    pre_start_ns INT64,
    pre_end_ns INT64,
    action_ts_ns INT64,
    post_start_ns INT64,
    post_end_ns INT64,
    alerts_json STRING,
    logs_json STRING,
    traces_json STRING,
    topology_upstream_json STRING,
    topology_downstream_json STRING,
    blast_radius STRING,
    change_type STRING,
    deploy_id STRING,
    config_id STRING,
    flag_id STRING,
    minutes_before_alert DOUBLE,
    candidate_root_causes_json STRING,
    action_class STRING,
    action_target STRING,
    action_parameters_json STRING,
    action_actor STRING,
    action_tool STRING,
    policy_decision STRING,
    policy_risk_tier STRING,
    policy_approval STRING,
    policy_approver STRING,
    policy_reason STRING,
    rollback_available INT64,
    rollback_plan STRING,
    rollback_executed INT64,
    machine_outcome STRING,
    machine_assigned_by STRING,
    machine_confidence DOUBLE,
    machine_notes STRING,
    human_outcome STRING,
    human_assigned_by STRING,
    human_confidence DOUBLE,
    human_notes STRING,
    recovery_primary_metric STRING,
    recovery_before DOUBLE,
    recovery_after_5m DOUBLE,
    recovery_after_15m DOUBLE,
    recovery_unit STRING,
    recovery_slo_restored INT64,
    recovery_side_effects_json STRING,
    evidence_refs_json STRING,
    reflection STRING,
    tags_json STRING
)
""".strip(),
    "action_outcome_episode_metrics": """
CREATE TABLE IF NOT EXISTS action_outcome_episode_metrics (
    episode_id STRING,
    metric_name STRING,
    metric_value DOUBLE
)
""".strip(),
    "action_outcome_retrieval_quality_labels": """
CREATE TABLE IF NOT EXISTS action_outcome_retrieval_quality_labels (
    query_id STRING,
    candidate_id STRING,
    quality_label STRING,
    reason STRING
)
""".strip(),
    "action_outcome_replay_recommendations": """
CREATE TABLE IF NOT EXISTS action_outcome_replay_recommendations (
    run_id STRING,
    variant STRING,
    query_id STRING,
    recommendation_rank INT64,
    action_class STRING,
    score_micros INT64,
    top_action INT64,
    top3_hit INT64,
    avoids_failed_repeat INT64,
    timestamp_ns INT64
)
""".strip(),
    "action_outcome_gate_suppressions": """
CREATE TABLE IF NOT EXISTS action_outcome_gate_suppressions (
    run_id STRING,
    query_id STRING,
    candidate_id STRING,
    action_class STRING,
    outcome_label STRING,
    raw_value_micros INT64,
    gated_value_micros INT64,
    multiplier_micros INT64,
    context_score_micros INT64,
    reasons STRING,
    timestamp_ns INT64
)
""".strip(),
}


EPISODE_COLUMNS = [
    "episode_id",
    "incident_id",
    "incident_type",
    "service",
    "environment",
    "entity_refs_json",
    "pre_start_ns",
    "pre_end_ns",
    "action_ts_ns",
    "post_start_ns",
    "post_end_ns",
    "alerts_json",
    "logs_json",
    "traces_json",
    "topology_upstream_json",
    "topology_downstream_json",
    "blast_radius",
    "change_type",
    "deploy_id",
    "config_id",
    "flag_id",
    "minutes_before_alert",
    "candidate_root_causes_json",
    "action_class",
    "action_target",
    "action_parameters_json",
    "action_actor",
    "action_tool",
    "policy_decision",
    "policy_risk_tier",
    "policy_approval",
    "policy_approver",
    "policy_reason",
    "rollback_available",
    "rollback_plan",
    "rollback_executed",
    "machine_outcome",
    "machine_assigned_by",
    "machine_confidence",
    "machine_notes",
    "human_outcome",
    "human_assigned_by",
    "human_confidence",
    "human_notes",
    "recovery_primary_metric",
    "recovery_before",
    "recovery_after_5m",
    "recovery_after_15m",
    "recovery_unit",
    "recovery_slo_restored",
    "recovery_side_effects_json",
    "evidence_refs_json",
    "reflection",
    "tags_json",
]


def parse_ts_ns(value: str) -> int:
    return int(datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp() * 1_000_000_000)


def format_ts(ns: int) -> str:
    return datetime.fromtimestamp(ns / 1_000_000_000, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def json_compact(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def bool_int(value: Any) -> int:
    return 1 if bool(value) else 0


def nullable_float(value: Any) -> float | None:
    if value is None or value == "":
        return None
    return float(value)


def episode_row(episode: dict[str, Any]) -> list[Any]:
    symptoms = episode["symptoms"]
    topology = episode["topology_context"]
    change = episode["change_context"]
    action = episode["action"]
    policy = episode["policy_decision"]
    rollback = episode["rollback_plan"]
    machine = episode["machine_outcome_label"]
    human = episode["human_outcome_label"]
    recovery = episode["recovery_curve"]
    return [
        episode["episode_id"],
        episode["incident_id"],
        episode["incident_type"],
        episode["service"],
        episode["environment"],
        json_compact(episode["entity_refs"]),
        parse_ts_ns(episode["pre_action_window"]["start"]),
        parse_ts_ns(episode["pre_action_window"]["end"]),
        parse_ts_ns(episode["action_ts"]),
        parse_ts_ns(episode["post_action_window"]["start"]),
        parse_ts_ns(episode["post_action_window"]["end"]),
        json_compact(symptoms.get("alerts", [])),
        json_compact(symptoms.get("logs", [])),
        json_compact(symptoms.get("traces", [])),
        json_compact(topology.get("upstream", [])),
        json_compact(topology.get("downstream", [])),
        str(topology.get("blast_radius") or ""),
        str(change.get("change_type") or "none"),
        str(change.get("deploy_id") or ""),
        str(change.get("config_id") or ""),
        str(change.get("flag_id") or ""),
        nullable_float(change.get("minutes_before_alert")),
        json_compact(episode["candidate_root_causes"]),
        action["action_class"],
        action["target"],
        json_compact(action.get("parameters", {})),
        action["actor"],
        action["tool"],
        policy["decision"],
        policy["risk_tier"],
        policy["approval"],
        str(policy.get("approver") or ""),
        str(policy.get("reason") or ""),
        bool_int(rollback.get("available")),
        str(rollback.get("plan") or ""),
        bool_int(rollback.get("executed")),
        machine["label"],
        machine["assigned_by"],
        float(machine["confidence"]),
        str(machine.get("notes") or ""),
        human["label"],
        human["assigned_by"],
        float(human["confidence"]),
        str(human.get("notes") or ""),
        recovery["primary_metric"],
        float(recovery["before"]),
        float(recovery["after_5m"]),
        float(recovery["after_15m"]),
        recovery["unit"],
        bool_int(recovery["slo_restored"]),
        json_compact(recovery.get("side_effects", [])),
        json_compact(episode.get("evidence_refs", [])),
        episode["reflection"],
        json_compact(episode["tags"]),
    ]


def metric_rows(episode: dict[str, Any]) -> list[tuple[str, str, float]]:
    metrics = episode.get("symptoms", {}).get("metrics", {})
    return [
        (episode["episode_id"], str(name), float(value))
        for name, value in sorted(metrics.items())
        if isinstance(value, (int, float))
    ]


def quality_label_rows(path: Path | None) -> list[tuple[str, str, str, str]]:
    if path is None:
        return []
    data = json.loads(path.read_text())
    rows = []
    for label in data.get("labels", []):
        rows.append((
            label["query_id"],
            label["candidate_id"],
            label["label"],
            label.get("reason", ""),
        ))
    return rows


def sql_quote(value: Any) -> str:
    if value is None:
        return "0"
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, (int, float)):
        return str(value)
    return "'" + str(value).replace("'", "''") + "'"


def insert_statement(table: str, columns: Iterable[str], values: Iterable[Any]) -> str:
    return (
        f"INSERT INTO {table} ({', '.join(columns)}) VALUES "
        f"({', '.join(sql_quote(value) for value in values)});"
    )


def create_tables(conn: sqlite3.Connection) -> None:
    for ddl in DDL.values():
        conn.execute(ddl)


def insert_seed_data(
    conn: sqlite3.Connection,
    episodes: list[dict[str, Any]],
    label_rows: list[tuple[str, str, str, str]],
) -> None:
    episode_placeholders = ", ".join("?" for _ in EPISODE_COLUMNS)
    conn.executemany(
        f"INSERT INTO action_outcome_episodes ({', '.join(EPISODE_COLUMNS)}) VALUES ({episode_placeholders})",
        [episode_row(episode) for episode in episodes],
    )
    conn.executemany(
        "INSERT INTO action_outcome_episode_metrics (episode_id, metric_name, metric_value) VALUES (?, ?, ?)",
        [row for episode in episodes for row in metric_rows(episode)],
    )
    conn.executemany(
        "INSERT INTO action_outcome_retrieval_quality_labels (query_id, candidate_id, quality_label, reason) VALUES (?, ?, ?, ?)",
        label_rows,
    )
    conn.commit()


def metric_bucket(metric_map: dict[str, dict[str, float]], episode_id: str) -> dict[str, float]:
    if episode_id not in metric_map:
        metric_map[episode_id] = {}
    return metric_map[episode_id]


def load_episodes_from_sql(conn: sqlite3.Connection) -> list[dict[str, Any]]:
    conn.row_factory = sqlite3.Row
    metric_map: dict[str, dict[str, float]] = {}
    for row in conn.execute(
        "SELECT episode_id, metric_name, metric_value FROM action_outcome_episode_metrics ORDER BY episode_id, metric_name"
    ):
        metric_bucket(metric_map, row["episode_id"])[row["metric_name"]] = float(row["metric_value"])

    episodes = []
    for row in conn.execute(
        "SELECT * FROM action_outcome_episodes ORDER BY action_ts_ns, episode_id"
    ):
        episodes.append(sql_row_to_episode(row, metric_map.get(row["episode_id"], {})))
    return episodes


def load_quality_labels_from_sql(conn: sqlite3.Connection) -> dict[tuple[str, str], str]:
    conn.row_factory = sqlite3.Row
    labels: dict[tuple[str, str], str] = {}
    for row in conn.execute(
        "SELECT query_id, candidate_id, quality_label FROM action_outcome_retrieval_quality_labels"
    ):
        labels[(row["query_id"], row["candidate_id"])] = row["quality_label"]
    return labels


def json_loads(value: Any) -> Any:
    if value is None or value == "":
        return []
    return json.loads(value)


def sql_row_to_episode(row: sqlite3.Row, metrics: dict[str, float]) -> dict[str, Any]:
    return {
        "episode_id": row["episode_id"],
        "incident_id": row["incident_id"],
        "incident_type": row["incident_type"],
        "service": row["service"],
        "environment": row["environment"],
        "entity_refs": json_loads(row["entity_refs_json"]),
        "pre_action_window": {
            "start": format_ts(int(row["pre_start_ns"])),
            "end": format_ts(int(row["pre_end_ns"])),
        },
        "action_ts": format_ts(int(row["action_ts_ns"])),
        "post_action_window": {
            "start": format_ts(int(row["post_start_ns"])),
            "end": format_ts(int(row["post_end_ns"])),
        },
        "symptoms": {
            "alerts": json_loads(row["alerts_json"]),
            "metrics": metrics,
            "logs": json_loads(row["logs_json"]),
            "traces": json_loads(row["traces_json"]),
        },
        "topology_context": {
            "upstream": json_loads(row["topology_upstream_json"]),
            "downstream": json_loads(row["topology_downstream_json"]),
            "blast_radius": row["blast_radius"],
        },
        "change_context": {
            "change_type": row["change_type"],
            "deploy_id": row["deploy_id"] or None,
            "config_id": row["config_id"] or None,
            "flag_id": row["flag_id"] or None,
            "minutes_before_alert": row["minutes_before_alert"],
        },
        "candidate_root_causes": json_loads(row["candidate_root_causes_json"]),
        "action": {
            "action_class": row["action_class"],
            "target": row["action_target"],
            "parameters": json_loads(row["action_parameters_json"]),
            "actor": row["action_actor"],
            "tool": row["action_tool"],
        },
        "policy_decision": {
            "decision": row["policy_decision"],
            "risk_tier": row["policy_risk_tier"],
            "approval": row["policy_approval"],
            "approver": row["policy_approver"],
            "reason": row["policy_reason"],
        },
        "rollback_plan": {
            "available": bool(row["rollback_available"]),
            "plan": row["rollback_plan"],
            "executed": bool(row["rollback_executed"]),
        },
        "machine_outcome_label": {
            "label": row["machine_outcome"],
            "assigned_by": row["machine_assigned_by"],
            "confidence": float(row["machine_confidence"]),
            "notes": row["machine_notes"],
        },
        "human_outcome_label": {
            "label": row["human_outcome"],
            "assigned_by": row["human_assigned_by"],
            "confidence": float(row["human_confidence"]),
            "notes": row["human_notes"],
        },
        "recovery_curve": {
            "primary_metric": row["recovery_primary_metric"],
            "before": float(row["recovery_before"]),
            "after_5m": float(row["recovery_after_5m"]),
            "after_15m": float(row["recovery_after_15m"]),
            "unit": row["recovery_unit"],
            "slo_restored": bool(row["recovery_slo_restored"]),
            "side_effects": json_loads(row["recovery_side_effects_json"]),
        },
        "evidence_refs": json_loads(row["evidence_refs_json"]),
        "reflection": row["reflection"],
        "tags": json_loads(row["tags_json"]),
    }


def evaluate_sql_replay(
    episodes: list[dict[str, Any]],
    quality_labels: dict[tuple[str, str], str],
    query_ids: list[str],
) -> tuple[dict[str, Any], dict[str, Any]]:
    by_id = {episode["episode_id"]: episode for episode in episodes}
    missing = [query_id for query_id in query_ids if query_id not in by_id]
    if missing:
        raise SystemExit(f"Unknown query ids: {', '.join(missing)}")
    queries = [by_id[query_id] for query_id in query_ids]
    baseline = ablation.evaluate_variant(episodes, queries, "full_guarded", set(), quality_labels)
    gated = context_gate.evaluate_context_gated(episodes, queries, quality_labels)
    return baseline, gated


def store_replay_results(
    conn: sqlite3.Connection,
    run_id: str,
    gated: dict[str, Any],
    timestamp_ns: int,
) -> None:
    recommendation_rows = []
    suppression_rows = []
    for row in gated["per_query"]:
        for rank, (action, score) in enumerate(row["actions"], start=1):
            recommendation_rows.append((
                run_id,
                "context_gated",
                row["query_id"],
                rank,
                action,
                int(score * 1_000_000),
                1 if rank == 1 else 0,
                bool_int(row["top3_hit"]),
                bool_int(row["avoids_failed"]),
                timestamp_ns,
            ))
        for suppressed in row["suppressed"]:
            suppression_rows.append((
                run_id,
                row["query_id"],
                suppressed["episode_id"],
                suppressed["action"],
                suppressed["outcome"],
                int(suppressed["raw_value"] * 1_000_000),
                int(suppressed["gated_value"] * 1_000_000),
                int(suppressed["multiplier"] * 1_000_000),
                int(suppressed["context_score"] * 1_000_000),
                ",".join(suppressed["reasons"]),
                timestamp_ns,
            ))

    conn.executemany(
        """
        INSERT INTO action_outcome_replay_recommendations (
            run_id, variant, query_id, recommendation_rank, action_class, score_micros,
            top_action, top3_hit, avoids_failed_repeat, timestamp_ns
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        recommendation_rows,
    )
    conn.executemany(
        """
        INSERT INTO action_outcome_gate_suppressions (
            run_id, query_id, candidate_id, action_class, outcome_label,
            raw_value_micros, gated_value_micros, multiplier_micros,
            context_score_micros, reasons, timestamp_ns
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        suppression_rows,
    )
    conn.commit()


def table_counts(conn: sqlite3.Connection) -> dict[str, int]:
    counts = {}
    for table in DDL:
        counts[table] = int(conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0])
    return counts


def compare_results(sql_gated: dict[str, Any], json_gated: dict[str, Any]) -> dict[str, Any]:
    sql_rows = {row["query_id"]: row for row in sql_gated["per_query"]}
    json_rows = {row["query_id"]: row for row in json_gated["per_query"]}
    mismatches = []
    for query_id, sql_row in sql_rows.items():
        json_row = json_rows[query_id]
        if sql_row["top_action"] != json_row["top_action"]:
            mismatches.append(
                {
                    "query_id": query_id,
                    "field": "top_action",
                    "sql": sql_row["top_action"],
                    "json": json_row["top_action"],
                }
            )
    return {
        "matches": not mismatches
        and sql_gated["top3_hit_rate"] == json_gated["top3_hit_rate"]
        and sql_gated["failed_avoidance_rate"] == json_gated["failed_avoidance_rate"]
        and sql_gated["suppression_count"] == json_gated["suppression_count"],
        "mismatches": mismatches,
    }


def render_sql_seed(
    episodes: list[dict[str, Any]],
    label_rows: list[tuple[str, str, str, str]],
    conn: sqlite3.Connection,
) -> str:
    lines = [
        "-- Action-Outcome SQL Replay Experiment 006 seed and result data",
        "-- Generated for ZeptoDB-compatible SQL ingestion.",
        "",
    ]
    for ddl in DDL.values():
        lines += [ddl + ";", ""]
    for episode in episodes:
        lines.append(insert_statement("action_outcome_episodes", EPISODE_COLUMNS, episode_row(episode)))
        for metric_row in metric_rows(episode):
            lines.append(insert_statement(
                "action_outcome_episode_metrics",
                ["episode_id", "metric_name", "metric_value"],
                metric_row,
            ))
    for label_row in label_rows:
        lines.append(insert_statement(
            "action_outcome_retrieval_quality_labels",
            ["query_id", "candidate_id", "quality_label", "reason"],
            label_row,
        ))

    conn.row_factory = sqlite3.Row
    for row in conn.execute("SELECT * FROM action_outcome_replay_recommendations ORDER BY query_id, recommendation_rank"):
        lines.append(insert_statement(
            "action_outcome_replay_recommendations",
            row.keys(),
            [row[key] for key in row.keys()],
        ))
    for row in conn.execute("SELECT * FROM action_outcome_gate_suppressions ORDER BY query_id, candidate_id"):
        lines.append(insert_statement(
            "action_outcome_gate_suppressions",
            row.keys(),
            [row[key] for key in row.keys()],
        ))
    return "\n".join(lines) + "\n"


def format_actions(actions: list[tuple[str, float]]) -> str:
    return ", ".join(f"{action}:{score:.2f}" for action, score in actions)


def render_report(
    fixture_path: Path,
    extra_fixture_paths: list[Path],
    quality_label_path: Path | None,
    counts: dict[str, int],
    sql_baseline: dict[str, Any],
    sql_gated: dict[str, Any],
    json_gated: dict[str, Any],
    comparison: dict[str, Any],
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    all_fixture_paths = [fixture_path, *extra_fixture_paths]
    sql_rows = {row["query_id"]: row for row in sql_gated["per_query"]}
    json_rows = {row["query_id"]: row for row in json_gated["per_query"]}

    lines = [
        "# ActionOutcomeReplay Experiment 006 SQL-Backed Replay Results",
        "",
        f"Generated at: {now}",
        "Fixtures:",
        *[f"- `{path}`" for path in all_fixture_paths],
        f"- Quality labels: `{quality_label_path}`" if quality_label_path else "- Quality labels: none",
        "",
        "## SQL Table Counts",
        "",
        "| Table | Rows |",
        "| --- | ---: |",
    ]
    for table, count in counts.items():
        lines.append(f"| {table} | {count} |")

    lines += [
        "",
        "## Replay Summary",
        "",
        "| Variant | Source | Top-3 Hit Rate | Failed-Action Avoidance | Cross-Family Top3 | Weak Cross-Family Top3 | Gate Suppressions | Labeled Top3 Quality |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |",
        "| full_guarded | SQL | {hit:.2f} | {avoid:.2f} | {cross} | {weak} | 0 | {quality} |".format(
            hit=sql_baseline["top3_hit_rate"],
            avoid=sql_baseline["failed_avoidance_rate"],
            cross=sql_baseline["cross_family_top3_count"],
            weak=sql_baseline["weak_cross_family_top3_count"],
            quality=ablation.format_quality_counts(sql_baseline["quality_counts"]),
        ),
        "| context_gated | SQL | {hit:.2f} | {avoid:.2f} | {cross} | {weak} | {suppressed} | {quality} |".format(
            hit=sql_gated["top3_hit_rate"],
            avoid=sql_gated["failed_avoidance_rate"],
            cross=sql_gated["cross_family_top3_count"],
            weak=sql_gated["weak_cross_family_top3_count"],
            suppressed=sql_gated["suppression_count"],
            quality=ablation.format_quality_counts(sql_gated["quality_counts"]),
        ),
        "| context_gated | JSON control | {hit:.2f} | {avoid:.2f} | {cross} | {weak} | {suppressed} | {quality} |".format(
            hit=json_gated["top3_hit_rate"],
            avoid=json_gated["failed_avoidance_rate"],
            cross=json_gated["cross_family_top3_count"],
            weak=json_gated["weak_cross_family_top3_count"],
            suppressed=json_gated["suppression_count"],
            quality=ablation.format_quality_counts(json_gated["quality_counts"]),
        ),
        "",
        "## SQL vs JSON Control",
        "",
        f"- Match status: {'pass' if comparison['matches'] else 'fail'}",
        f"- Top-action mismatches: {len(comparison['mismatches'])}",
    ]
    if comparison["mismatches"]:
        for mismatch in comparison["mismatches"]:
            lines.append(
                f"- {mismatch['query_id']}: SQL {mismatch['sql']} != JSON {mismatch['json']}"
            )

    lines += [
        "",
        "## Per-Query SQL Context-Gated Actions",
        "",
        "| Query | SQL Top Action | JSON Top Action | Match | SQL Top Actions | Avoids Failed Repeat |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for query_id, sql_row in sql_rows.items():
        json_row = json_rows[query_id]
        lines.append(
            "| {query} | {sql_top} | {json_top} | {match} | {actions} | {avoid} |".format(
                query=query_id,
                sql_top=sql_row["top_action"],
                json_top=json_row["top_action"],
                match="yes" if sql_row["top_action"] == json_row["top_action"] else "no",
                actions=format_actions(sql_row["actions"]),
                avoid="yes" if sql_row["avoids_failed"] else "no",
            )
        )

    lines += [
        "",
        "## Example SQL Queries",
        "",
        "```sql",
        "SELECT episode_id, incident_type, action_class, human_outcome",
        "FROM action_outcome_episodes",
        "WHERE incident_type = 'order_queue_backlog'",
        "ORDER BY action_ts_ns;",
        "",
        "SELECT episode_id, metric_name, metric_value",
        "FROM action_outcome_episode_metrics",
        "WHERE metric_name IN ('cpu_pct', 'db_conn_used_pct', 'consumer_error_pct')",
        "ORDER BY episode_id, metric_name;",
        "",
        "SELECT query_id, candidate_id, action_class, reasons",
        "FROM action_outcome_gate_suppressions",
        "WHERE multiplier_micros < 1000000",
        "ORDER BY query_id, candidate_id;",
        "```",
        "",
        "## Interpretation",
        "",
        "Experiment 006 validates the first SQL-backed research contract. The replay",
        "logic no longer consumes raw JSON fixtures directly; it reconstructs episodes",
        "from SQL tables and matches the JSON control result.",
        "",
        "This is still a local SQL harness, not a live ZeptoDB server run. The next",
        "step is to execute the generated SQL through ZeptoDB's HTTP SQL endpoint and",
        "compare live query results against this report.",
        "",
        "## Next Steps",
        "",
        "1. Run the generated SQL seed against a live ZeptoDB HTTP server.",
        "2. Add live SQL query checks for top actions and suppression rows.",
        "3. Promote the table schema into a design note if the live replay matches.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", type=Path, default=Path("docs/research/fixtures/action_outcome_episodes.json"))
    parser.add_argument("--extra-fixture", action="append", type=Path, default=[])
    parser.add_argument("--quality-labels", type=Path)
    parser.add_argument("--output", type=Path, default=Path("docs/research/results/action_outcome_sql_replay_006.md"))
    parser.add_argument("--sql-output", type=Path, default=Path("docs/research/results/action_outcome_sql_replay_006.sql"))
    parser.add_argument("--query-id", action="append", dest="query_ids")
    args = parser.parse_args()

    episodes = ablation.load_episodes(args.fixture, args.extra_fixture)
    label_rows = quality_label_rows(args.quality_labels)
    query_ids = args.query_ids or replay.DEFAULT_QUERY_IDS

    conn = sqlite3.connect(":memory:")
    create_tables(conn)
    insert_seed_data(conn, episodes, label_rows)
    sql_episodes = load_episodes_from_sql(conn)
    sql_quality_labels = load_quality_labels_from_sql(conn)
    sql_baseline, sql_gated = evaluate_sql_replay(sql_episodes, sql_quality_labels, query_ids)

    json_quality_labels = ablation.load_quality_labels(args.quality_labels)
    _, json_gated = evaluate_sql_replay(episodes, json_quality_labels, query_ids)
    comparison = compare_results(sql_gated, json_gated)
    timestamp_ns = parse_ts_ns(datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))
    run_id = "sql_replay_006"
    store_replay_results(conn, run_id, sql_gated, timestamp_ns)

    counts = table_counts(conn)
    report = render_report(
        args.fixture,
        args.extra_fixture,
        args.quality_labels,
        counts,
        sql_baseline,
        sql_gated,
        json_gated,
        comparison,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)

    if args.sql_output:
        args.sql_output.parent.mkdir(parents=True, exist_ok=True)
        args.sql_output.write_text(render_sql_seed(episodes, label_rows, conn))

    if not comparison["matches"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
