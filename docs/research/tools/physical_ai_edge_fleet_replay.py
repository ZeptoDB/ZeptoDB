#!/usr/bin/env python3
"""Two-node edge/fleet replay for Physical AI Action-Outcome research.

Experiment 015 splits the Physical AI memory loop into an edge-local node that
must suppress unsafe actions immediately and a fleet-global node that receives
delayed consolidation rows for audit and cross-robot learning.
"""

from __future__ import annotations

import argparse
import json
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import physical_ai_action_outcome_baseline as baseline
import physical_ai_sql_replay as single


EDGE_INCIDENT_TABLE = "physical_ai_edge_incidents_015"
EDGE_EXPECTED_ACTION_TABLE = "physical_ai_edge_expected_actions_015"
EDGE_ROBOT_STATE_TABLE = "physical_ai_edge_robot_state_015"
EDGE_SENSOR_TABLE = "physical_ai_edge_sensor_summary_015"
EDGE_DECISION_TABLE = "physical_ai_edge_decisions_015"
EDGE_SUPPRESSION_TABLE = "physical_ai_edge_suppressions_015"

FLEET_EXPECTED_ACTION_TABLE = "physical_ai_fleet_expected_actions_015"
FLEET_ACTION_OUTCOME_TABLE = "physical_ai_fleet_action_outcomes_015"
FLEET_EDGE_DECISION_TABLE = "physical_ai_fleet_edge_decisions_015"
FLEET_RETRIEVAL_TABLE = "physical_ai_fleet_retrieval_015"
FLEET_SUPPRESSION_TABLE = "physical_ai_fleet_suppressions_015"

EDGE_TABLES = [
    EDGE_SUPPRESSION_TABLE,
    EDGE_DECISION_TABLE,
    EDGE_SENSOR_TABLE,
    EDGE_ROBOT_STATE_TABLE,
    EDGE_EXPECTED_ACTION_TABLE,
    EDGE_INCIDENT_TABLE,
]

FLEET_TABLES = [
    FLEET_SUPPRESSION_TABLE,
    FLEET_RETRIEVAL_TABLE,
    FLEET_EDGE_DECISION_TABLE,
    FLEET_ACTION_OUTCOME_TABLE,
    FLEET_EXPECTED_ACTION_TABLE,
]


@dataclass
class Expected:
    edge_counts: dict[str, int]
    fleet_counts: dict[str, int]
    edge_recovery_rows: list[list[Any]]
    edge_robot_asof_rows: list[list[Any]]
    edge_sensor_asof_rows: list[list[Any]]
    edge_risky_suppression_count: int
    fleet_recovery_rows: list[list[Any]]
    fleet_audit_rows: list[list[Any]]
    fleet_consolidation_count: int
    fleet_window_rows: list[tuple[int, int, int]]
    fleet_lag_rows: list[tuple[int, int, int]]


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


def delta(before: dict[str, int], after: dict[str, int]) -> dict[str, int]:
    keys = set(before) | set(after)
    return {key: after.get(key, 0) - before.get(key, 0) for key in sorted(keys)}


def table_counts(url: str, timeout: float, tables: list[str]) -> dict[str, int | None]:
    return {table: single.table_count(url, timeout, table) for table in tables}


def reset_tables(url: str, timeout: float, tables: list[str], recorder: single.SqlRecorder) -> None:
    for table in tables:
        single.post_checked(url, timeout, f"DROP TABLE IF EXISTS {table}", recorder)


def create_edge_tables(url: str, timeout: float, recorder: single.SqlRecorder) -> None:
    ddls = [
        f"""
CREATE TABLE IF NOT EXISTS {EDGE_INCIDENT_TABLE} (
    query_id STRING,
    query_seq INT64,
    robot_id STRING,
    robot_code INT64,
    incident_type STRING,
    unsafe_action STRING,
    unsafe_action_code INT64,
    expected_actions STRING,
    symbol INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {EDGE_EXPECTED_ACTION_TABLE} (
    query_id STRING,
    expected_action_key STRING,
    action_class STRING,
    action_code INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {EDGE_ROBOT_STATE_TABLE} (
    symbol INT64,
    robot_code INT64,
    query_seq INT64,
    timestamp TIMESTAMP_NS,
    safety_score_micros INT64,
    human_distance_m FLOAT64,
    battery_pct INT64
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {EDGE_SENSOR_TABLE} (
    symbol INT64,
    query_seq INT64,
    timestamp TIMESTAMP_NS,
    primary_metric_code INT64,
    primary_metric_value FLOAT64,
    quality INT64
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {EDGE_DECISION_TABLE} (
    query_id STRING,
    query_seq INT64,
    robot_code INT64,
    selected_action STRING,
    selected_action_code INT64,
    selected_expected_key STRING,
    unsafe_action STRING,
    unsafe_action_code INT64,
    recovery_top1_hit INT64,
    avoids_risky_repeat INT64,
    risky_action_suppressed INT64,
    suppressed_count INT64,
    edge_latency_ms INT64,
    decision_ts_ns TIMESTAMP_NS,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {EDGE_SUPPRESSION_TABLE} (
    query_id STRING,
    query_seq INT64,
    candidate_id STRING,
    suppression_key STRING,
    action_class STRING,
    action_code INT64,
    outcome_label STRING,
    raw_value_micros INT64,
    gated_value_micros INT64,
    context_score_micros INT64,
    immediate_suppression INT64,
    reasons STRING,
    timestamp TIMESTAMP_NS
)
""".strip(),
    ]
    for ddl in ddls:
        single.post_checked(url, timeout, ddl, recorder)


def create_fleet_tables(url: str, timeout: float, recorder: single.SqlRecorder) -> None:
    ddls = [
        f"""
CREATE TABLE IF NOT EXISTS {FLEET_EXPECTED_ACTION_TABLE} (
    query_id STRING,
    expected_action_key STRING,
    action_class STRING,
    action_code INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {FLEET_ACTION_OUTCOME_TABLE} (
    episode_id STRING,
    incident_type STRING,
    action_class STRING,
    action_code INT64,
    incident_action_key STRING,
    outcome_label STRING,
    outcome_value_micros INT64,
    safety_restored INT64,
    primary_metric_code INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {FLEET_EDGE_DECISION_TABLE} (
    query_id STRING,
    query_seq INT64,
    robot_code INT64,
    selected_action STRING,
    selected_action_code INT64,
    selected_expected_key STRING,
    unsafe_action_code INT64,
    recovery_top1_hit INT64,
    avoids_risky_repeat INT64,
    risky_action_suppressed INT64,
    suppressed_count INT64,
    edge_latency_ms INT64,
    decision_ts_ns TIMESTAMP_NS,
    consolidated_ts_ns TIMESTAMP_NS,
    consolidation_lag_ms INT64,
    source_edge_node_id INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {FLEET_RETRIEVAL_TABLE} (
    query_id STRING,
    query_seq INT64,
    retrieval_rank INT64,
    candidate_id STRING,
    suppression_key STRING,
    candidate_action STRING,
    quality_label STRING,
    quality_code INT64,
    score_micros INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {FLEET_SUPPRESSION_TABLE} (
    query_id STRING,
    query_seq INT64,
    candidate_id STRING,
    suppression_key STRING,
    action_class STRING,
    action_code INT64,
    outcome_label STRING,
    raw_value_micros INT64,
    gated_value_micros INT64,
    context_score_micros INT64,
    source_edge_node_id INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
    ]
    for ddl in ddls:
        single.post_checked(url, timeout, ddl, recorder)


def context_result(results: list[dict[str, Any]]) -> dict[str, Any]:
    return next(
        result
        for result in results
        if result["variant"] == "context_gated_physical_ai_action_outcome"
    )


def materialize(
    *,
    edge_url: str,
    fleet_url: str,
    timeout: float,
    episodes: list[dict[str, Any]],
    results: list[dict[str, Any]],
    query_ids: list[str],
    action_codes: dict[str, int],
    metric_codes: dict[str, int],
    edge_node_id: int,
    edge_recorder: single.SqlRecorder,
    fleet_recorder: single.SqlRecorder,
) -> Expected:
    reset_tables(edge_url, timeout, EDGE_TABLES, edge_recorder)
    reset_tables(fleet_url, timeout, FLEET_TABLES, fleet_recorder)
    create_edge_tables(edge_url, timeout, edge_recorder)
    create_fleet_tables(fleet_url, timeout, fleet_recorder)

    by_id = {episode["episode_id"]: episode for episode in episodes}
    query_seq_by_id = {query_id: idx for idx, query_id in enumerate(query_ids, start=1)}
    gated = context_result(results)
    gated_rows = {row["query_id"]: row for row in gated["per_query"]}

    edge_counts = {table: 0 for table in EDGE_TABLES}
    fleet_counts = {table: 0 for table in FLEET_TABLES}
    edge_recovery_rows: list[list[Any]] = []
    edge_robot_asof_rows: list[list[Any]] = []
    edge_sensor_asof_rows: list[list[Any]] = []
    fleet_recovery_rows: list[list[Any]] = []
    fleet_audit_rows: list[list[Any]] = []
    fleet_window_rows: list[tuple[int, int, int]] = []
    fleet_lag_rows: list[tuple[int, int, int]] = []

    for episode in episodes:
        metric_name, _before, _after_5m = single.primary_metric(episode)
        outcome = baseline.outcome_label(episode)
        value = baseline.outcome_value(episode)
        recovery = episode.get("recovery_curve", {})
        action = str(episode["action"]["action_class"])
        single.post_checked(
            fleet_url,
            timeout,
            f"INSERT INTO {FLEET_ACTION_OUTCOME_TABLE} "
            "(episode_id, incident_type, action_class, action_code, incident_action_key, "
            "outcome_label, outcome_value_micros, safety_restored, primary_metric_code, timestamp) VALUES "
            f"({single.sql_quote(episode['episode_id'])}, {single.sql_quote(episode['incident_type'])}, "
            f"{single.sql_quote(action)}, {action_codes[action]}, "
            f"{single.sql_quote(single.incident_action_key(str(episode['incident_type']), action))}, "
            f"{single.sql_quote(outcome)}, {single.micros(value)}, "
            f"{single.bool_int(recovery.get('safety_restored'))}, {metric_codes[metric_name]}, "
            f"{int(episode['action_ts_ns'])})",
            fleet_recorder,
        )
        fleet_counts[FLEET_ACTION_OUTCOME_TABLE] += 1

    for query_id in query_ids:
        episode = by_id[query_id]
        row = gated_rows[query_id]
        query_seq = query_seq_by_id[query_id]
        event_ts = single.query_event_ts(query_seq)
        decision_ts = event_ts + 50_000
        consolidation_lag_ms = 250 + query_seq * 20
        consolidated_ts = decision_ts + consolidation_lag_ms * 1_000_000
        selected_action = str(row["top_action"])
        unsafe_action = str(episode["action"]["action_class"])
        robot_code = single.robot_symbol(episode)
        metric_name, before, after_5m = single.primary_metric(episode)
        metric_code = metric_codes[metric_name]
        risky_suppressed = any(str(item["action"]) == unsafe_action for item in row["suppressed"])
        suppressed_count = len(row["suppressed"])

        single.post_checked(
            edge_url,
            timeout,
            f"INSERT INTO {EDGE_INCIDENT_TABLE} "
            "(query_id, query_seq, robot_id, robot_code, incident_type, unsafe_action, "
            "unsafe_action_code, expected_actions, symbol, timestamp) VALUES "
            f"({single.sql_quote(query_id)}, {query_seq}, {single.sql_quote(episode['robot_id'])}, "
            f"{robot_code}, {single.sql_quote(episode['incident_type'])}, "
            f"{single.sql_quote(unsafe_action)}, {action_codes[unsafe_action]}, "
            f"{single.sql_quote(','.join(episode.get('expected_safe_actions', [])))}, "
            f"{single.ASOF_SYMBOL}, {event_ts})",
            edge_recorder,
        )
        edge_counts[EDGE_INCIDENT_TABLE] += 1

        for rank, action in enumerate(episode.get("expected_safe_actions", []), start=1):
            key = single.expected_action_key(query_id, str(action))
            for url, table, recorder, counts in (
                (edge_url, EDGE_EXPECTED_ACTION_TABLE, edge_recorder, edge_counts),
                (fleet_url, FLEET_EXPECTED_ACTION_TABLE, fleet_recorder, fleet_counts),
            ):
                single.post_checked(
                    url,
                    timeout,
                    f"INSERT INTO {table} "
                    "(query_id, expected_action_key, action_class, action_code, timestamp) VALUES "
                    f"({single.sql_quote(query_id)}, {single.sql_quote(key)}, "
                    f"{single.sql_quote(str(action))}, {action_codes[str(action)]}, {event_ts + rank})",
                    recorder,
                )
                counts[table] += 1

        for idx, timestamp in enumerate([event_ts - 20_000_000, event_ts - 5_000_000, event_ts]):
            safety_score = [250_000, 420_000, 180_000][idx]
            single.post_checked(
                edge_url,
                timeout,
                f"INSERT INTO {EDGE_ROBOT_STATE_TABLE} "
                "(symbol, robot_code, query_seq, timestamp, safety_score_micros, human_distance_m, battery_pct) VALUES "
                f"({single.ASOF_SYMBOL}, {robot_code}, {query_seq}, {timestamp}, {safety_score}, "
                f"{float(episode['symptoms']['metrics'].get('human_distance_m', 0))}, "
                f"{int(episode['symptoms']['metrics'].get('battery_pct', 80))})",
                edge_recorder,
            )
            edge_counts[EDGE_ROBOT_STATE_TABLE] += 1

        for idx, timestamp in enumerate([event_ts - 18_000_000, event_ts - 4_000_000, event_ts]):
            value = before if idx < 2 else after_5m
            single.post_checked(
                edge_url,
                timeout,
                f"INSERT INTO {EDGE_SENSOR_TABLE} "
                "(symbol, query_seq, timestamp, primary_metric_code, primary_metric_value, quality) VALUES "
                f"({single.ASOF_SYMBOL}, {query_seq}, {timestamp}, {metric_code}, {value}, {100 - idx})",
                edge_recorder,
            )
            edge_counts[EDGE_SENSOR_TABLE] += 1

        selected_key = single.expected_action_key(query_id, selected_action)
        edge_latency_ms = 12 + query_seq
        decision_values = (
            f"({single.sql_quote(query_id)}, {query_seq}, {robot_code}, "
            f"{single.sql_quote(selected_action)}, {action_codes[selected_action]}, "
            f"{single.sql_quote(selected_key)}, {single.sql_quote(unsafe_action)}, "
            f"{action_codes[unsafe_action]}, {single.bool_int(row['recovery_action_hit'])}, "
            f"{single.bool_int(row['avoids_risky_repeat'])}, {single.bool_int(risky_suppressed)}, "
            f"{suppressed_count}, {edge_latency_ms}, {decision_ts}, {decision_ts})"
        )
        single.post_checked(
            edge_url,
            timeout,
            f"INSERT INTO {EDGE_DECISION_TABLE} "
            "(query_id, query_seq, robot_code, selected_action, selected_action_code, "
            "selected_expected_key, unsafe_action, unsafe_action_code, recovery_top1_hit, "
            "avoids_risky_repeat, risky_action_suppressed, suppressed_count, edge_latency_ms, "
            "decision_ts_ns, timestamp) VALUES "
            f"{decision_values}",
            edge_recorder,
        )
        edge_counts[EDGE_DECISION_TABLE] += 1

        single.post_checked(
            fleet_url,
            timeout,
            f"INSERT INTO {FLEET_EDGE_DECISION_TABLE} "
            "(query_id, query_seq, robot_code, selected_action, selected_action_code, "
            "selected_expected_key, unsafe_action_code, recovery_top1_hit, avoids_risky_repeat, "
            "risky_action_suppressed, suppressed_count, edge_latency_ms, decision_ts_ns, "
            "consolidated_ts_ns, consolidation_lag_ms, source_edge_node_id, timestamp) VALUES "
            f"({single.sql_quote(query_id)}, {query_seq}, {robot_code}, "
            f"{single.sql_quote(selected_action)}, {action_codes[selected_action]}, "
            f"{single.sql_quote(selected_key)}, {action_codes[unsafe_action]}, "
            f"{single.bool_int(row['recovery_action_hit'])}, {single.bool_int(row['avoids_risky_repeat'])}, "
            f"{single.bool_int(risky_suppressed)}, {suppressed_count}, {edge_latency_ms}, "
            f"{decision_ts}, {consolidated_ts}, {consolidation_lag_ms}, {edge_node_id}, {consolidated_ts})",
            fleet_recorder,
        )
        fleet_counts[FLEET_EDGE_DECISION_TABLE] += 1

        edge_recovery_rows.append([query_id, selected_action])
        fleet_recovery_rows.append([query_id, selected_action])
        edge_robot_asof_rows.append([query_seq, robot_code, action_codes[unsafe_action]])
        edge_sensor_asof_rows.append([query_seq, metric_code])
        previous_lag = 0 if query_seq == 1 else 250 + (query_seq - 1) * 20
        fleet_lag_rows.append((query_seq, consolidation_lag_ms, previous_lag))

        for candidate in row["retrieval_top3"]:
            quality = str(candidate["quality"])
            suppression_key = single.suppression_key(query_id, str(candidate["episode_id"]))
            single.post_checked(
                fleet_url,
                timeout,
                f"INSERT INTO {FLEET_RETRIEVAL_TABLE} "
                "(query_id, query_seq, retrieval_rank, candidate_id, suppression_key, "
                "candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES "
                f"({single.sql_quote(query_id)}, {query_seq}, {int(candidate['rank'])}, "
                f"{single.sql_quote(str(candidate['episode_id']))}, {single.sql_quote(suppression_key)}, "
                f"{single.sql_quote(str(candidate['action']))}, {single.sql_quote(quality)}, "
                f"{single.QUALITY_CODES.get(quality, 0)}, {single.micros(float(candidate['score']))}, "
                f"{consolidated_ts + int(candidate['rank'])})",
                fleet_recorder,
            )
            fleet_counts[FLEET_RETRIEVAL_TABLE] += 1
            fleet_window_rows.append((query_seq, int(candidate["rank"]), int(candidate["rank"])))
            if single.QUALITY_CODES.get(quality, 0) == single.QUALITY_CODES["misleading"]:
                fleet_audit_rows.append(
                    [query_id, str(candidate["episode_id"]), str(candidate["action"]), quality]
                )

        for idx, suppressed in enumerate(row["suppressed"], start=1):
            suppression_key = single.suppression_key(query_id, str(suppressed["episode_id"]))
            values = (
                f"({single.sql_quote(query_id)}, {query_seq}, "
                f"{single.sql_quote(str(suppressed['episode_id']))}, {single.sql_quote(suppression_key)}, "
                f"{single.sql_quote(str(suppressed['action']))}, {action_codes[str(suppressed['action'])]}, "
                f"{single.sql_quote(str(suppressed['outcome']))}, {single.micros(float(suppressed['raw_value']))}, "
                f"{single.micros(float(suppressed['gated_value']))}, "
                f"{single.micros(float(suppressed['context_score']))}, "
            )
            reasons = ",".join(suppressed["reasons"]) if suppressed["reasons"] else "none"
            single.post_checked(
                edge_url,
                timeout,
                f"INSERT INTO {EDGE_SUPPRESSION_TABLE} "
                "(query_id, query_seq, candidate_id, suppression_key, action_class, action_code, "
                "outcome_label, raw_value_micros, gated_value_micros, context_score_micros, "
                "immediate_suppression, reasons, timestamp) VALUES "
                f"{values}1, {single.sql_quote(reasons)}, {decision_ts + idx})",
                edge_recorder,
            )
            edge_counts[EDGE_SUPPRESSION_TABLE] += 1

            single.post_checked(
                fleet_url,
                timeout,
                f"INSERT INTO {FLEET_SUPPRESSION_TABLE} "
                "(query_id, query_seq, candidate_id, suppression_key, action_class, action_code, "
                "outcome_label, raw_value_micros, gated_value_micros, context_score_micros, "
                "source_edge_node_id, timestamp) VALUES "
                f"{values}{edge_node_id}, {consolidated_ts + idx})",
                fleet_recorder,
            )
            fleet_counts[FLEET_SUPPRESSION_TABLE] += 1

    return Expected(
        edge_counts=edge_counts,
        fleet_counts=fleet_counts,
        edge_recovery_rows=sorted(edge_recovery_rows),
        edge_robot_asof_rows=sorted(edge_robot_asof_rows),
        edge_sensor_asof_rows=sorted(edge_sensor_asof_rows),
        edge_risky_suppression_count=len(query_ids),
        fleet_recovery_rows=sorted(fleet_recovery_rows),
        fleet_audit_rows=sorted(fleet_audit_rows),
        fleet_consolidation_count=len(query_ids),
        fleet_window_rows=sorted(fleet_window_rows),
        fleet_lag_rows=sorted(fleet_lag_rows),
    )


def scalar_int(data: dict[str, Any]) -> int | None:
    try:
        return int(data["data"][0][0])
    except (KeyError, IndexError, TypeError, ValueError):
        return None


def validate(edge_url: str, fleet_url: str, timeout: float, expected: Expected) -> dict[str, Any]:
    edge_counts = table_counts(edge_url, timeout, EDGE_TABLES)
    fleet_counts = table_counts(fleet_url, timeout, FLEET_TABLES)

    edge_recovery_sql = (
        "SELECT d.query_id, d.selected_action "
        f"FROM {EDGE_DECISION_TABLE} d "
        f"JOIN {EDGE_EXPECTED_ACTION_TABLE} e ON d.selected_expected_key = e.expected_action_key "
        "WHERE d.recovery_top1_hit = 1 AND d.avoids_risky_repeat = 1"
    )
    edge_recovery_rows = single.sorted_rows(single.query_data(edge_url, timeout, edge_recovery_sql))

    edge_risky_sql = (
        f"SELECT count(*) FROM {EDGE_DECISION_TABLE} "
        "WHERE avoids_risky_repeat = 1 AND risky_action_suppressed = 1 "
        "AND selected_action_code != unsafe_action_code"
    )
    edge_risky_suppression_count = scalar_int(single.query_data(edge_url, timeout, edge_risky_sql))

    edge_robot_asof_sql = (
        "SELECT i.query_seq, r.robot_code, i.unsafe_action_code "
        f"FROM {EDGE_INCIDENT_TABLE} i "
        f"ASOF JOIN {EDGE_ROBOT_STATE_TABLE} r "
        "ON i.symbol = r.symbol AND i.timestamp >= r.timestamp "
        "ORDER BY i.query_seq ASC"
    )
    edge_robot_asof_rows = single.sorted_rows(single.query_data(edge_url, timeout, edge_robot_asof_sql))

    edge_sensor_asof_sql = (
        "SELECT i.query_seq, s.primary_metric_code "
        f"FROM {EDGE_INCIDENT_TABLE} i "
        f"ASOF JOIN {EDGE_SENSOR_TABLE} s "
        "ON i.symbol = s.symbol AND i.timestamp >= s.timestamp "
        "ORDER BY i.query_seq ASC"
    )
    edge_sensor_asof_rows = single.sorted_rows(single.query_data(edge_url, timeout, edge_sensor_asof_sql))

    fleet_recovery_sql = (
        "SELECT d.query_id, d.selected_action "
        f"FROM {FLEET_EDGE_DECISION_TABLE} d "
        f"JOIN {FLEET_EXPECTED_ACTION_TABLE} e ON d.selected_expected_key = e.expected_action_key "
        "WHERE d.recovery_top1_hit = 1 AND d.avoids_risky_repeat = 1"
    )
    fleet_recovery_rows = single.sorted_rows(single.query_data(fleet_url, timeout, fleet_recovery_sql))

    fleet_audit_sql = (
        "SELECT s.query_id, s.candidate_id, s.action_class, r.quality_label "
        f"FROM {FLEET_SUPPRESSION_TABLE} s "
        f"JOIN {FLEET_RETRIEVAL_TABLE} r ON s.suppression_key = r.suppression_key "
        "WHERE r.quality_code = 3"
    )
    fleet_audit_rows = single.sorted_rows(single.query_data(fleet_url, timeout, fleet_audit_sql))

    fleet_consolidation_sql = (
        f"SELECT count(*) FROM {FLEET_EDGE_DECISION_TABLE} "
        "WHERE consolidated_ts_ns > decision_ts_ns AND consolidation_lag_ms >= 250"
    )
    fleet_consolidation_count = scalar_int(
        single.query_data(fleet_url, timeout, fleet_consolidation_sql)
    )

    fleet_window_sql = (
        "SELECT query_seq, retrieval_rank, "
        "ROW_NUMBER() OVER (PARTITION BY query_seq ORDER BY retrieval_rank) AS rank_check "
        f"FROM {FLEET_RETRIEVAL_TABLE}"
    )
    fleet_window_rows = sorted(
        (int(row[0]), int(row[1]), int(row[2]))
        for row in single.query_data(fleet_url, timeout, fleet_window_sql).get("data", [])
    )

    fleet_lag_sql = (
        "SELECT query_seq, consolidation_lag_ms, "
        "LAG(consolidation_lag_ms, 1, 0) OVER (ORDER BY query_seq) AS prev_lag "
        f"FROM {FLEET_EDGE_DECISION_TABLE}"
    )
    fleet_lag_rows = sorted(
        (int(row[0]), int(row[1]), int(row[2]))
        for row in single.query_data(fleet_url, timeout, fleet_lag_sql).get("data", [])
    )

    return {
        "edge_counts": edge_counts,
        "fleet_counts": fleet_counts,
        "edge_count_status": edge_counts == expected.edge_counts,
        "fleet_count_status": fleet_counts == expected.fleet_counts,
        "edge_recovery_rows": edge_recovery_rows,
        "edge_recovery_status": edge_recovery_rows == expected.edge_recovery_rows,
        "edge_risky_suppression_count": edge_risky_suppression_count,
        "edge_risky_suppression_status": edge_risky_suppression_count == expected.edge_risky_suppression_count,
        "edge_robot_asof_rows": edge_robot_asof_rows,
        "edge_robot_asof_status": edge_robot_asof_rows == expected.edge_robot_asof_rows,
        "edge_sensor_asof_rows": edge_sensor_asof_rows,
        "edge_sensor_asof_status": edge_sensor_asof_rows == expected.edge_sensor_asof_rows,
        "fleet_recovery_rows": fleet_recovery_rows,
        "fleet_recovery_status": fleet_recovery_rows == expected.fleet_recovery_rows,
        "fleet_audit_rows": fleet_audit_rows,
        "fleet_audit_status": fleet_audit_rows == expected.fleet_audit_rows,
        "fleet_consolidation_count": fleet_consolidation_count,
        "fleet_consolidation_status": fleet_consolidation_count == expected.fleet_consolidation_count,
        "fleet_window_rows": fleet_window_rows,
        "fleet_window_status": fleet_window_rows == expected.fleet_window_rows,
        "fleet_lag_rows": fleet_lag_rows,
        "fleet_lag_status": fleet_lag_rows == expected.fleet_lag_rows,
    }


def markdown_table(rows: list[list[Any]], headers: list[str]) -> list[str]:
    return single.markdown_table(rows, headers)


def render_report(
    *,
    edge_url: str,
    fleet_url: str,
    fixture: Path,
    edge_sql: Path,
    fleet_sql: Path,
    validation: dict[str, Any],
    before_edge_stats: dict[str, int],
    after_edge_stats: dict[str, int],
    before_fleet_stats: dict[str, int],
    after_fleet_stats: dict[str, int],
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    edge_delta = delta(before_edge_stats, after_edge_stats)
    fleet_delta = delta(before_fleet_stats, after_fleet_stats)
    status_keys = [
        "edge_count_status",
        "fleet_count_status",
        "edge_recovery_status",
        "edge_risky_suppression_status",
        "edge_robot_asof_status",
        "edge_sensor_asof_status",
        "fleet_recovery_status",
        "fleet_audit_status",
        "fleet_consolidation_status",
        "fleet_window_status",
        "fleet_lag_status",
    ]
    edge_activity = sum(validation["edge_counts"].values()) > 0
    fleet_activity = sum(validation["fleet_counts"].values()) > 0
    overall = all(validation[key] for key in status_keys) and edge_activity and fleet_activity
    lines = [
        "# Physical AI Action-Outcome Experiment 015 Edge/Fleet Replay Results",
        "",
        f"Generated at: {now}",
        f"Edge endpoint: `{edge_url}`",
        f"Fleet endpoint: `{fleet_url}`",
        f"Fixture: `{fixture}`",
        f"Edge SQL replay file: `{edge_sql}`",
        f"Fleet SQL replay file: `{fleet_sql}`",
        "Classification: Research-only",
        "",
        "## Status",
        "",
        f"- Edge row-count status: {'pass' if validation['edge_count_status'] else 'fail'}",
        f"- Fleet row-count status: {'pass' if validation['fleet_count_status'] else 'fail'}",
        f"- Edge immediate recovery status: {'pass' if validation['edge_recovery_status'] else 'fail'}",
        f"- Edge risky-action suppression status: {'pass' if validation['edge_risky_suppression_status'] else 'fail'}",
        f"- Edge robot ASOF status: {'pass' if validation['edge_robot_asof_status'] else 'fail'}",
        f"- Edge sensor ASOF status: {'pass' if validation['edge_sensor_asof_status'] else 'fail'}",
        f"- Fleet consolidated recovery status: {'pass' if validation['fleet_recovery_status'] else 'fail'}",
        f"- Fleet suppression audit JOIN status: {'pass' if validation['fleet_audit_status'] else 'fail'}",
        f"- Fleet consolidation-lag status: {'pass' if validation['fleet_consolidation_status'] else 'fail'}",
        f"- Fleet ROW_NUMBER status: {'pass' if validation['fleet_window_status'] else 'fail'}",
        f"- Fleet LAG status: {'pass' if validation['fleet_lag_status'] else 'fail'}",
        f"- Overall edge/fleet replay status: {'pass' if overall else 'fail'}",
        "",
        "## Node Activity",
        "",
        "| Node Role | ticks_ingested delta | ticks_stored delta | partitions_created delta | Research rows |",
        "| --- | ---: | ---: | ---: | ---: |",
        f"| edge-local | {edge_delta.get('ticks_ingested', 0)} | {edge_delta.get('ticks_stored', 0)} | {edge_delta.get('partitions_created', 0)} | {sum(validation['edge_counts'].values())} |",
        f"| fleet-global | {fleet_delta.get('ticks_ingested', 0)} | {fleet_delta.get('ticks_stored', 0)} | {fleet_delta.get('partitions_created', 0)} | {sum(validation['fleet_counts'].values())} |",
        "",
        "## Table Counts",
        "",
        "### Edge-Local Tables",
        "",
        "| Table | Rows |",
        "| --- | ---: |",
    ]
    for table, count in sorted(validation["edge_counts"].items()):
        lines.append(f"| `{table}` | {count} |")

    lines += ["", "### Fleet-Global Tables", "", "| Table | Rows |", "| --- | ---: |"]
    for table, count in sorted(validation["fleet_counts"].items()):
        lines.append(f"| `{table}` | {count} |")

    lines += [
        "",
        "## Edge-Local Immediate Decision",
        "",
        "The edge node validates that the selected action is an expected recovery",
        "action and differs from the unsafe query action before any fleet",
        "consolidation happens.",
        "",
        *markdown_table(validation["edge_recovery_rows"], ["Query", "Edge Selected Action"]),
        "",
        f"- Risky actions suppressed immediately: {validation['edge_risky_suppression_count']}",
        "",
        "## Edge ASOF Telemetry",
        "",
        *markdown_table(validation["edge_robot_asof_rows"], ["Query Seq", "Robot Code", "Unsafe Action Code"]),
        "",
        *markdown_table(validation["edge_sensor_asof_rows"], ["Query Seq", "Primary Metric Code"]),
        "",
        "## Fleet-Global Consolidation",
        "",
        "The fleet node receives delayed decision rows and validates that recovery",
        "selection and risky-repeat avoidance survive consolidation.",
        "",
        *markdown_table(validation["fleet_recovery_rows"], ["Query", "Fleet Consolidated Action"]),
        "",
        f"- Consolidated rows with lag >= 250 ms: {validation['fleet_consolidation_count']}",
        "",
        "## Fleet Audit JOIN",
        "",
        *markdown_table(
            validation["fleet_audit_rows"],
            ["Query", "Candidate", "Suppressed Action", "Retrieval Quality"],
        ),
        "",
        "## Fleet Window Checks",
        "",
        f"- ROW_NUMBER rows: {len(validation['fleet_window_rows'])}",
        f"- LAG rows: {len(validation['fleet_lag_rows'])}",
        "",
        "## Interpretation",
        "",
        "Experiment 015 validates the intended split-brain-safe memory shape for",
        "Physical AI: edge-local memory makes the immediate safety decision, while",
        "fleet-global memory receives slower consolidated evidence for audit and",
        "future cross-robot learning.",
        "",
        "This is still research-only. The delayed edge-to-fleet transfer is modeled",
        "by the harness writing SQL rows to two live ZeptoDB endpoints; it is not a",
        "new runtime replication or control-plane feature.",
        "",
        "## Next Best Step",
        "",
        "Replace harness-driven consolidation with a bounded, explicit edge-to-fleet",
        "replication path or feed connector, then test dropped/duplicated",
        "consolidation events and late-arriving fleet audits.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--edge-url", default="http://127.0.0.1:19441/")
    parser.add_argument("--fleet-url", default="http://127.0.0.1:19442/")
    parser.add_argument("--edge-stats-url", default="http://127.0.0.1:19441/stats")
    parser.add_argument("--fleet-stats-url", default="http://127.0.0.1:19442/stats")
    parser.add_argument("--edge-node-id", type=int, default=1)
    parser.add_argument(
        "--fixture",
        type=Path,
        default=Path("docs/research/fixtures/physical_ai_action_outcome_episodes.json"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("docs/research/results/physical_ai_edge_fleet_replay_015.md"),
    )
    parser.add_argument(
        "--edge-sql-output",
        type=Path,
        default=Path("docs/research/results/physical_ai_edge_fleet_replay_015_edge.sql"),
    )
    parser.add_argument(
        "--fleet-sql-output",
        type=Path,
        default=Path("docs/research/results/physical_ai_edge_fleet_replay_015_fleet.sql"),
    )
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--query-id", action="append", dest="query_ids")
    args = parser.parse_args()

    before_edge_stats = fetch_stats(args.edge_stats_url, args.timeout)
    before_fleet_stats = fetch_stats(args.fleet_stats_url, args.timeout)

    episodes = baseline.load_fixture(args.fixture)
    query_ids = args.query_ids or baseline.DEFAULT_QUERY_IDS
    results = single.build_results(episodes, query_ids)
    action_codes = single.build_action_codes(episodes)
    metric_codes = single.build_metric_codes(episodes)
    edge_recorder = single.SqlRecorder()
    fleet_recorder = single.SqlRecorder()

    expected = materialize(
        edge_url=args.edge_url,
        fleet_url=args.fleet_url,
        timeout=args.timeout,
        episodes=episodes,
        results=results,
        query_ids=query_ids,
        action_codes=action_codes,
        metric_codes=metric_codes,
        edge_node_id=args.edge_node_id,
        edge_recorder=edge_recorder,
        fleet_recorder=fleet_recorder,
    )
    validation = validate(args.edge_url, args.fleet_url, args.timeout, expected)

    args.edge_sql_output.parent.mkdir(parents=True, exist_ok=True)
    args.edge_sql_output.write_text(edge_recorder.text())
    args.fleet_sql_output.parent.mkdir(parents=True, exist_ok=True)
    args.fleet_sql_output.write_text(fleet_recorder.text())

    after_edge_stats = fetch_stats(args.edge_stats_url, args.timeout)
    after_fleet_stats = fetch_stats(args.fleet_stats_url, args.timeout)

    report = render_report(
        edge_url=args.edge_url,
        fleet_url=args.fleet_url,
        fixture=args.fixture,
        edge_sql=args.edge_sql_output,
        fleet_sql=args.fleet_sql_output,
        validation=validation,
        before_edge_stats=before_edge_stats,
        after_edge_stats=after_edge_stats,
        before_fleet_stats=before_fleet_stats,
        after_fleet_stats=after_fleet_stats,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)

    required = [
        "edge_count_status",
        "fleet_count_status",
        "edge_recovery_status",
        "edge_risky_suppression_status",
        "edge_robot_asof_status",
        "edge_sensor_asof_status",
        "fleet_recovery_status",
        "fleet_audit_status",
        "fleet_consolidation_status",
        "fleet_window_status",
        "fleet_lag_status",
    ]
    if not all(validation[key] for key in required):
        raise SystemExit("Physical AI edge/fleet replay validation failed")


if __name__ == "__main__":
    main()
