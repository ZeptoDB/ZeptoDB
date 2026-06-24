#!/usr/bin/env python3
"""Live ZeptoDB SQL replay for Physical AI Action-Outcome research.

Experiment 014 materializes robot-operation-shaped telemetry, incidents,
actions, outcomes, recommendations, retrieval evidence, and suppressions into
native ZeptoDB SQL tables. It then validates the replay contract with hash
JOINs, ASOF JOINs, window functions, and spatial SQL.
"""

from __future__ import annotations

import argparse
import json
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import physical_ai_action_outcome_baseline as baseline


INCIDENT_TABLE = "physical_ai_incidents_014"
EXPECTED_ACTION_TABLE = "physical_ai_expected_actions_014"
ACTION_OUTCOME_TABLE = "physical_ai_action_outcomes_014"
ROBOT_STATE_TABLE = "physical_ai_robot_state_014"
SENSOR_TABLE = "physical_ai_sensor_summary_014"
POSE_TABLE = "physical_ai_pose_014"
RECOMMENDATION_TABLE = "physical_ai_recommendations_014"
RETRIEVAL_TABLE = "physical_ai_retrieval_014"
SUPPRESSION_TABLE = "physical_ai_suppressions_014"

ALL_TABLES = [
    SUPPRESSION_TABLE,
    RETRIEVAL_TABLE,
    RECOMMENDATION_TABLE,
    POSE_TABLE,
    SENSOR_TABLE,
    ROBOT_STATE_TABLE,
    ACTION_OUTCOME_TABLE,
    EXPECTED_ACTION_TABLE,
    INCIDENT_TABLE,
]

VARIANT_CODES = {
    "similar_robot_incident": 1,
    "runbook_action_prior": 2,
    "reflection_only_memory": 3,
    "context_gated_physical_ai_action_outcome": 4,
}

QUALITY_CODES = {
    "useful": 1,
    "superficial": 2,
    "misleading": 3,
    "cross_family": 4,
    "unlabeled": 0,
}

ROBOT_SYMBOLS = {
    "agv_17": 1017,
    "mr_09": 2009,
    "arm_06": 3006,
    "agv_33": 1033,
    "drone_18": 4018,
}

BASE_TS_NS = 1_810_000_000_000_000_000
ASOF_SYMBOL = 1


@dataclass
class SqlResponse:
    ok: bool
    status: int | str
    body: str


@dataclass
class ExpectedRows:
    counts: dict[str, int]
    failed_repeat_rows: list[list[Any]]
    context_top_rows: list[list[Any]]
    suppression_audit_rows: list[list[Any]]
    action_outcome_rows: list[list[Any]]
    robot_asof_rows: list[list[Any]]
    sensor_asof_rows: list[list[Any]]
    window_rows: list[tuple[int, int, int]]
    lag_rows: list[tuple[int, int, int, int]]
    dock_pose_count: int


class SqlRecorder:
    def __init__(self) -> None:
        self.statements: list[str] = []

    def add(self, sql: str) -> None:
        self.statements.append(sql.strip())

    def text(self) -> str:
        return ";\n\n".join(self.statements) + ";\n"


def sql_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def micros(value: float) -> int:
    return int(round(value * 1_000_000))


def bool_int(value: Any) -> int:
    return 1 if bool(value) else 0


def query_event_ts(query_seq: int) -> int:
    return BASE_TS_NS + query_seq * 1_000_000_000


def expected_action_key(query_id: str, action: str) -> str:
    return f"{query_id}|{action}"


def incident_action_key(incident_type: str, action: str) -> str:
    return f"{incident_type}|{action}"


def suppression_key(query_id: str, candidate_id: str) -> str:
    return f"{query_id}|{candidate_id}"


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


def post_checked(
    url: str,
    timeout: float,
    sql: str,
    recorder: SqlRecorder | None = None,
) -> dict[str, Any]:
    if recorder is not None:
        recorder.add(sql)
    response = post_sql(url, sql, timeout)
    if not response.ok:
        raise RuntimeError(
            f"SQL failed ({response.status}): {sql[:180]} -> {response.body[:240]}"
        )
    data = parse_json_response(response)
    if "error" in data:
        raise RuntimeError(f"SQL error: {sql[:180]} -> {data['error']}")
    return data


def query_data(url: str, timeout: float, sql: str) -> dict[str, Any]:
    return post_checked(url, timeout, sql)


def sorted_rows(data: dict[str, Any]) -> list[list[Any]]:
    return sorted(data.get("data", []))


def table_count(url: str, timeout: float, table: str) -> int | None:
    data = query_data(url, timeout, f"SELECT count(*) FROM {table}")
    try:
        return int(data["data"][0][0])
    except (KeyError, IndexError, TypeError, ValueError):
        return None


def build_results(episodes: list[dict[str, Any]], query_ids: list[str]) -> list[dict[str, Any]]:
    by_id = {episode["episode_id"]: episode for episode in episodes}
    missing = [query_id for query_id in query_ids if query_id not in by_id]
    if missing:
        raise ValueError(f"unknown query ids: {', '.join(missing)}")
    queries = [by_id[query_id] for query_id in query_ids]
    return [
        baseline.evaluate_rows(
            "similar_robot_incident",
            "Similar robot incident retrieval without outcome-aware action learning.",
            episodes,
            queries,
            baseline.similar_robot_incident_score,
            use_outcome=False,
        ),
        baseline.evaluate_runbook_prior(episodes, queries),
        baseline.evaluate_rows(
            "reflection_only_memory",
            "Reflection-style experiential memory using text and outcomes but no structured context gate.",
            episodes,
            queries,
            baseline.reflection_memory_score,
            use_outcome=True,
        ),
        baseline.evaluate_rows(
            "context_gated_physical_ai_action_outcome",
            "Structured Physical AI Action-Outcome Memory with topology, motif, and change-context outcome gates.",
            episodes,
            queries,
            baseline.similar_robot_incident_score,
            use_outcome=True,
            gated=True,
        ),
    ]


def build_action_codes(episodes: list[dict[str, Any]]) -> dict[str, int]:
    actions: set[str] = set()
    for episode in episodes:
        actions.add(str(episode["action"]["action_class"]))
        actions.update(str(action) for action in episode.get("expected_safe_actions", []))
    return {action: idx for idx, action in enumerate(sorted(actions), start=1)}


def build_metric_codes(episodes: list[dict[str, Any]]) -> dict[str, int]:
    metrics = {primary_metric(episode)[0] for episode in episodes}
    return {metric: idx for idx, metric in enumerate(sorted(metrics), start=1)}


def reset_tables(url: str, timeout: float, recorder: SqlRecorder) -> None:
    for table in ALL_TABLES:
        post_checked(url, timeout, f"DROP TABLE IF EXISTS {table}", recorder)


def create_tables(url: str, timeout: float, recorder: SqlRecorder) -> None:
    ddls = [
        f"""
CREATE TABLE IF NOT EXISTS {INCIDENT_TABLE} (
    query_id STRING,
    query_seq INT64,
    incident_type STRING,
    domain STRING,
    robot_id STRING,
    site STRING,
    symbol INT64,
    action_ts_ns TIMESTAMP_NS,
    true_failed_action STRING,
    true_failed_action_code INT64,
    unsafe_actions STRING,
    expected_actions STRING,
    topology_json STRING,
    change_json STRING,
    temporal_motif_json STRING,
    is_query INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {EXPECTED_ACTION_TABLE} (
    query_id STRING,
    action_class STRING,
    action_code INT64,
    expected_action_key STRING,
    incident_action_key STRING,
    action_rank INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {ACTION_OUTCOME_TABLE} (
    episode_id STRING,
    incident_type STRING,
    action_class STRING,
    action_code INT64,
    incident_action_key STRING,
    outcome_label STRING,
    outcome_value_micros INT64,
    safety_restored INT64,
    primary_metric STRING,
    before_micros INT64,
    after_5m_micros INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {ROBOT_STATE_TABLE} (
    symbol INT64,
    robot_code INT64,
    robot_id STRING,
    incident_id STRING,
    session_id STRING,
    timestamp TIMESTAMP_NS,
    zone STRING,
    payload STRING,
    human_distance_m FLOAT64,
    safety_score_micros INT64,
    battery_pct INT64
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {SENSOR_TABLE} (
    symbol INT64,
    robot_id STRING,
    incident_id STRING,
    timestamp TIMESTAMP_NS,
    sensor_kind STRING,
    primary_metric STRING,
    primary_metric_code INT64,
    primary_metric_value FLOAT64,
    secondary_metric_value FLOAT64,
    quality INT64
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {POSE_TABLE} (
    symbol INT64,
    robot_id STRING,
    incident_id STRING,
    timestamp TIMESTAMP_NS,
    lat FLOAT64,
    lon FLOAT64,
    zone STRING
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {RECOMMENDATION_TABLE} (
    variant STRING,
    variant_code INT64,
    query_id STRING,
    query_seq INT64,
    group_id INT64,
    recommendation_rank INT64,
    action_class STRING,
    action_code INT64,
    expected_action_key STRING,
    score_micros INT64,
    top3_hit INT64,
    recovery_top1_hit INT64,
    avoids_risky_repeat INT64,
    hazardous_top_action INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {RETRIEVAL_TABLE} (
    variant STRING,
    variant_code INT64,
    query_id STRING,
    query_seq INT64,
    retrieval_rank INT64,
    candidate_id STRING,
    suppression_key STRING,
    candidate_action STRING,
    candidate_outcome STRING,
    quality_label STRING,
    quality_code INT64,
    score_micros INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {SUPPRESSION_TABLE} (
    query_id STRING,
    query_seq INT64,
    candidate_id STRING,
    suppression_key STRING,
    action_class STRING,
    outcome_label STRING,
    raw_value_micros INT64,
    gated_value_micros INT64,
    multiplier_micros INT64,
    context_score_micros INT64,
    reasons STRING,
    timestamp TIMESTAMP_NS
)
""".strip(),
    ]
    for ddl in ddls:
        post_checked(url, timeout, ddl, recorder)


def robot_symbol(episode: dict[str, Any]) -> int:
    return ROBOT_SYMBOLS.get(str(episode["robot_id"]), 9000 + abs(hash(episode["robot_id"])) % 1000)


def primary_metric(episode: dict[str, Any]) -> tuple[str, float, float]:
    recovery = episode.get("recovery_curve", {})
    metric_name = str(recovery.get("primary_metric", "safety_score"))
    before = float(recovery.get("before", 0.0))
    after = float(recovery.get("after_5m", before))
    return metric_name, before, after


def zone_lat_lon(episode: dict[str, Any]) -> tuple[float, float]:
    domain = episode.get("domain")
    context = episode.get("topology_context", {})
    zone = str(context.get("zone", "unknown"))
    if domain == "warehouse_agv" and zone == "dock_a":
        return 37.77492, -122.41938
    if domain == "warehouse_agv":
        return 37.77550, -122.42010
    if domain == "drone_fleet" and zone == "no_fly_boundary":
        return 37.78000, -122.41000
    if domain == "drone_fleet":
        return 37.79000, -122.40000
    if domain == "cold_chain_logistics":
        return 37.77600, -122.42100
    if domain == "mobile_robot":
        return 37.77000, -122.41800
    return 37.77200, -122.41700


def materialize(
    url: str,
    timeout: float,
    episodes: list[dict[str, Any]],
    results: list[dict[str, Any]],
    query_ids: list[str],
    action_codes: dict[str, int],
    metric_codes: dict[str, int],
    recorder: SqlRecorder,
) -> ExpectedRows:
    reset_tables(url, timeout, recorder)
    create_tables(url, timeout, recorder)

    by_id = {episode["episode_id"]: episode for episode in episodes}
    query_seq_by_id = {query_id: idx for idx, query_id in enumerate(query_ids, start=1)}

    count_accumulator: dict[str, int] = {table: 0 for table in ALL_TABLES}

    for episode in episodes:
        outcome = baseline.outcome_label(episode)
        value = baseline.outcome_value(episode)
        metric_name, before, after_5m = primary_metric(episode)
        recovery = episode.get("recovery_curve", {})
        post_checked(
            url,
            timeout,
            f"INSERT INTO {ACTION_OUTCOME_TABLE} "
            "(episode_id, incident_type, action_class, action_code, incident_action_key, outcome_label, "
            "outcome_value_micros, safety_restored, primary_metric, "
            "before_micros, after_5m_micros, timestamp) VALUES "
            f"({sql_quote(episode['episode_id'])}, {sql_quote(episode['incident_type'])}, "
            f"{sql_quote(episode['action']['action_class'])}, "
            f"{action_codes[str(episode['action']['action_class'])]}, "
            f"{sql_quote(incident_action_key(str(episode['incident_type']), str(episode['action']['action_class'])))}, "
            f"{sql_quote(outcome)}, "
            f"{micros(value)}, {bool_int(recovery.get('safety_restored'))}, "
            f"{sql_quote(metric_name)}, {micros(before)}, {micros(after_5m)}, "
            f"{int(episode['action_ts_ns'])})",
            recorder,
        )
        count_accumulator[ACTION_OUTCOME_TABLE] += 1

    for query_id in query_ids:
        episode = by_id[query_id]
        symbol = ASOF_SYMBOL
        robot_code = robot_symbol(episode)
        query_seq = query_seq_by_id[query_id]
        event_ts = query_event_ts(query_seq)
        context = episode["topology_context"]
        change = episode["change_context"]
        metric_name, before, after_5m = primary_metric(episode)
        metric_code = metric_codes[metric_name]
        action_ts = int(episode["action_ts_ns"])
        lat, lon = zone_lat_lon(episode)
        is_query = 1
        post_checked(
            url,
            timeout,
            f"INSERT INTO {INCIDENT_TABLE} "
            "(query_id, query_seq, incident_type, domain, robot_id, site, symbol, "
            "action_ts_ns, true_failed_action, true_failed_action_code, unsafe_actions, expected_actions, "
            "topology_json, change_json, temporal_motif_json, is_query, timestamp) VALUES "
            f"({sql_quote(query_id)}, {query_seq}, {sql_quote(episode['incident_type'])}, "
            f"{sql_quote(episode['domain'])}, {sql_quote(episode['robot_id'])}, "
            f"{sql_quote(episode['site'])}, {symbol}, {action_ts}, "
            f"{sql_quote(episode['action']['action_class'])}, "
            f"{action_codes[str(episode['action']['action_class'])]}, "
            f"{sql_quote(','.join(episode.get('unsafe_repeat_actions', [])))}, "
            f"{sql_quote(','.join(episode.get('expected_safe_actions', [])))}, "
            f"{sql_quote(json.dumps(context, sort_keys=True))}, "
            f"{sql_quote(json.dumps(change, sort_keys=True))}, "
            f"{sql_quote(','.join(episode.get('temporal_motif', [])))}, "
            f"{is_query}, {event_ts})",
            recorder,
        )
        count_accumulator[INCIDENT_TABLE] += 1

        for rank, action in enumerate(episode.get("expected_safe_actions", []), start=1):
            post_checked(
                url,
                timeout,
                f"INSERT INTO {EXPECTED_ACTION_TABLE} "
                "(query_id, action_class, action_code, expected_action_key, incident_action_key, "
                "action_rank, timestamp) VALUES "
                f"({sql_quote(query_id)}, {sql_quote(action)}, {action_codes[str(action)]}, "
                f"{sql_quote(expected_action_key(query_id, str(action)))}, "
                f"{sql_quote(incident_action_key(str(episode['incident_type']), str(action)))}, "
                f"{rank}, {event_ts + rank})",
                recorder,
            )
            count_accumulator[EXPECTED_ACTION_TABLE] += 1

        state_times = [event_ts - 20_000_000, event_ts - 5_000_000, event_ts]
        safety_scores = [250_000, 420_000, 180_000 if baseline.outcome_value(episode) < 0 else 850_000]
        for idx, timestamp in enumerate(state_times):
            post_checked(
                url,
                timeout,
                f"INSERT INTO {ROBOT_STATE_TABLE} "
                "(symbol, robot_code, robot_id, incident_id, session_id, timestamp, zone, payload, "
                "human_distance_m, safety_score_micros, battery_pct) VALUES "
                f"({symbol}, {robot_code}, {sql_quote(episode['robot_id'])}, {sql_quote(query_id)}, "
                f"{sql_quote('session_' + query_id)}, {timestamp}, "
                f"{sql_quote(str(context.get('zone', 'unknown')))}, "
                f"{sql_quote(str(context.get('payload', 'unknown')))}, "
                f"{float(episode['symptoms']['metrics'].get('human_distance_m', 0))}, "
                f"{safety_scores[idx]}, {int(episode['symptoms']['metrics'].get('battery_pct', 80))})",
                recorder,
            )
            count_accumulator[ROBOT_STATE_TABLE] += 1

        for idx, timestamp in enumerate([event_ts - 18_000_000, event_ts - 4_000_000, event_ts]):
            value = before if idx < 2 else after_5m
            post_checked(
                url,
                timeout,
                f"INSERT INTO {SENSOR_TABLE} "
                "(symbol, robot_id, incident_id, timestamp, sensor_kind, primary_metric, "
                "primary_metric_code, primary_metric_value, secondary_metric_value, quality) VALUES "
                f"({symbol}, {sql_quote(episode['robot_id'])}, {sql_quote(query_id)}, "
                f"{timestamp}, {sql_quote(metric_name)}, {sql_quote(metric_name)}, "
                f"{metric_code}, {value}, {after_5m}, {100 - idx})",
                recorder,
            )
            count_accumulator[SENSOR_TABLE] += 1

        post_checked(
            url,
            timeout,
            f"INSERT INTO {POSE_TABLE} "
            "(symbol, robot_id, incident_id, timestamp, lat, lon, zone) VALUES "
            f"({symbol}, {sql_quote(episode['robot_id'])}, {sql_quote(query_id)}, "
            f"{event_ts}, {lat}, {lon}, {sql_quote(str(context.get('zone', 'unknown')))})",
            recorder,
        )
        count_accumulator[POSE_TABLE] += 1

    recommendation_rows: list[tuple[Any, ...]] = []
    retrieval_rows: list[tuple[Any, ...]] = []
    suppression_rows: list[tuple[Any, ...]] = []

    for result in results:
        variant = result["variant"]
        variant_code = VARIANT_CODES[variant]
        for row in result["per_query"]:
            query_id = row["query_id"]
            query_seq = query_seq_by_id[query_id]
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
                        action_codes[str(action)],
                        expected_action_key(query_id, str(action)),
                        micros(score),
                        bool_int(row["top3_hit"]),
                        bool_int(row["recovery_action_hit"] and rank == 1),
                        bool_int(row["avoids_risky_repeat"] and rank == 1),
                        bool_int(row["hazardous_top_action"] and rank == 1),
                        BASE_TS_NS + len(recommendation_rows),
                    )
                )
            for candidate in row["retrieval_top3"]:
                quality = candidate["quality"]
                retrieval_rows.append(
                    (
                        variant,
                        variant_code,
                        query_id,
                        query_seq,
                        int(candidate["rank"]),
                        str(candidate["episode_id"]),
                        suppression_key(query_id, str(candidate["episode_id"])),
                        str(candidate["action"]),
                        str(candidate["outcome"]),
                        quality,
                        QUALITY_CODES.get(quality, 0),
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
                        suppression_key(query_id, str(suppressed["episode_id"])),
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

    for row in recommendation_rows:
        post_checked(
            url,
            timeout,
            f"INSERT INTO {RECOMMENDATION_TABLE} "
            "(variant, variant_code, query_id, query_seq, group_id, recommendation_rank, "
            "action_class, action_code, expected_action_key, score_micros, top3_hit, recovery_top1_hit, "
            "avoids_risky_repeat, hazardous_top_action, timestamp) VALUES "
            f"({sql_quote(row[0])}, {row[1]}, {sql_quote(row[2])}, {row[3]}, "
            f"{row[4]}, {row[5]}, {sql_quote(row[6])}, {row[7]}, "
            f"{sql_quote(row[8])}, {row[9]}, {row[10]}, {row[11]}, "
            f"{row[12]}, {row[13]}, {row[14]})",
            recorder,
        )
        count_accumulator[RECOMMENDATION_TABLE] += 1

    for row in retrieval_rows:
        post_checked(
            url,
            timeout,
            f"INSERT INTO {RETRIEVAL_TABLE} "
            "(variant, variant_code, query_id, query_seq, retrieval_rank, "
            "candidate_id, suppression_key, candidate_action, candidate_outcome, quality_label, "
            "quality_code, score_micros, timestamp) VALUES "
            f"({sql_quote(row[0])}, {row[1]}, {sql_quote(row[2])}, {row[3]}, "
            f"{row[4]}, {sql_quote(row[5])}, {sql_quote(row[6])}, "
            f"{sql_quote(row[7])}, {sql_quote(row[8])}, {sql_quote(row[9])}, "
            f"{row[10]}, {row[11]}, {row[12]})",
            recorder,
        )
        count_accumulator[RETRIEVAL_TABLE] += 1

    for row in suppression_rows:
        post_checked(
            url,
            timeout,
            f"INSERT INTO {SUPPRESSION_TABLE} "
            "(query_id, query_seq, candidate_id, suppression_key, action_class, outcome_label, "
            "raw_value_micros, gated_value_micros, multiplier_micros, "
            "context_score_micros, reasons, timestamp) VALUES "
            f"({sql_quote(row[0])}, {row[1]}, {sql_quote(row[2])}, "
            f"{sql_quote(row[3])}, {sql_quote(row[4])}, {sql_quote(row[5])}, "
            f"{row[6]}, {row[7]}, {row[8]}, {row[9]}, {sql_quote(row[10])}, "
            f"{row[11]})",
            recorder,
        )
        count_accumulator[SUPPRESSION_TABLE] += 1

    failed_repeat_rows = [
        [row[0], row[2], row[6], by_id[row[2]]["action"]["action_class"]]
        for row in recommendation_rows
        if row[5] == 1 and row[13] == 1
    ]
    context_top_rows = [
        [row[2], row[6]]
        for row in recommendation_rows
        if row[1] == VARIANT_CODES["context_gated_physical_ai_action_outcome"]
        and row[5] == 1
        and row[11] == 1
        and row[12] == 1
    ]
    suppression_audit_rows = [
        [row[2], row[5], row[7], row[9]]
        for row in retrieval_rows
        if row[1] == VARIANT_CODES["context_gated_physical_ai_action_outcome"]
        and row[10] == QUALITY_CODES["misleading"]
    ]
    action_outcome_rows = [
        [query_id, action]
        for query_id in query_ids
        for action in by_id[query_id].get("expected_safe_actions", [])
    ]

    by_group: dict[int, list[tuple[int, int]]] = {}
    for row in recommendation_rows:
        by_group.setdefault(row[4], []).append((row[5], row[9]))
    window_rows: list[tuple[int, int, int]] = []
    lag_rows: list[tuple[int, int, int, int]] = []
    for group_id, rows in by_group.items():
        previous_score = 0
        for idx, (rank, score) in enumerate(sorted(rows), start=1):
            window_rows.append((group_id, rank, idx))
            lag_rows.append((group_id, rank, score, previous_score))
            previous_score = score

    robot_asof_rows = [
        [
            query_seq_by_id[query_id],
            robot_symbol(by_id[query_id]),
            action_codes[str(by_id[query_id]["action"]["action_class"])],
        ]
        for query_id in query_ids
    ]
    sensor_asof_rows = [
        [query_seq_by_id[query_id], metric_codes[primary_metric(by_id[query_id])[0]]]
        for query_id in query_ids
    ]

    return ExpectedRows(
        counts=count_accumulator,
        failed_repeat_rows=sorted(failed_repeat_rows),
        context_top_rows=sorted(context_top_rows),
        suppression_audit_rows=sorted(suppression_audit_rows),
        action_outcome_rows=sorted(action_outcome_rows),
        robot_asof_rows=sorted(robot_asof_rows),
        sensor_asof_rows=sorted(sensor_asof_rows),
        window_rows=sorted(window_rows),
        lag_rows=sorted(lag_rows),
        dock_pose_count=1,
    )


def validate(url: str, timeout: float, expected: ExpectedRows) -> dict[str, Any]:
    counts = {table: table_count(url, timeout, table) for table in expected.counts}

    failed_repeat_sql = (
        "SELECT r.variant, r.query_id, r.action_class, i.true_failed_action "
        f"FROM {RECOMMENDATION_TABLE} r "
        f"JOIN {INCIDENT_TABLE} i ON r.query_id = i.query_id "
        "WHERE r.recommendation_rank = 1 AND r.hazardous_top_action = 1"
    )
    failed_repeat_rows = sorted_rows(query_data(url, timeout, failed_repeat_sql))

    context_top_sql = (
        "SELECT r.query_id, r.action_class "
        f"FROM {RECOMMENDATION_TABLE} r "
        f"JOIN {EXPECTED_ACTION_TABLE} e ON r.expected_action_key = e.expected_action_key "
        "WHERE r.variant_code = 4 AND r.recommendation_rank = 1 "
        "AND r.recovery_top1_hit = 1 AND r.avoids_risky_repeat = 1"
    )
    context_top_rows = sorted_rows(query_data(url, timeout, context_top_sql))

    suppression_audit_sql = (
        "SELECT s.query_id, s.candidate_id, s.action_class, t.quality_label "
        f"FROM {SUPPRESSION_TABLE} s "
        f"JOIN {RETRIEVAL_TABLE} t ON s.suppression_key = t.suppression_key "
        "WHERE t.variant_code = 4 AND t.quality_code = 3"
    )
    suppression_audit_rows = sorted_rows(query_data(url, timeout, suppression_audit_sql))

    action_outcome_sql = (
        "SELECT e.query_id, o.action_class "
        f"FROM {EXPECTED_ACTION_TABLE} e "
        f"JOIN {ACTION_OUTCOME_TABLE} o ON e.incident_action_key = o.incident_action_key "
        "WHERE o.outcome_value_micros > 0"
    )
    action_outcome_rows = sorted_rows(query_data(url, timeout, action_outcome_sql))

    robot_asof_sql = (
        "SELECT i.query_seq, r.robot_code, i.true_failed_action_code "
        f"FROM {INCIDENT_TABLE} i "
        f"ASOF JOIN {ROBOT_STATE_TABLE} r "
        "ON i.symbol = r.symbol AND i.timestamp >= r.timestamp "
        "ORDER BY i.query_seq ASC"
    )
    robot_asof_rows = sorted_rows(query_data(url, timeout, robot_asof_sql))

    sensor_asof_sql = (
        "SELECT i.query_seq, s.primary_metric_code "
        f"FROM {INCIDENT_TABLE} i "
        f"ASOF JOIN {SENSOR_TABLE} s "
        "ON i.symbol = s.symbol AND i.timestamp >= s.timestamp "
        "ORDER BY i.query_seq ASC"
    )
    sensor_asof_rows = sorted_rows(query_data(url, timeout, sensor_asof_sql))

    window_sql = (
        "SELECT group_id, recommendation_rank, "
        "ROW_NUMBER() OVER (PARTITION BY group_id ORDER BY recommendation_rank) AS rank_check "
        f"FROM {RECOMMENDATION_TABLE}"
    )
    window_rows = sorted(
        (int(row[0]), int(row[1]), int(row[2]))
        for row in query_data(url, timeout, window_sql).get("data", [])
    )

    lag_sql = (
        "SELECT group_id, recommendation_rank, score_micros, "
        "LAG(score_micros, 1, 0) OVER (PARTITION BY group_id ORDER BY recommendation_rank) AS prev_score "
        f"FROM {RECOMMENDATION_TABLE}"
    )
    lag_rows = sorted(
        (int(row[0]), int(row[1]), int(row[2]), int(row[3]))
        for row in query_data(url, timeout, lag_sql).get("data", [])
    )

    spatial_sql = (
        f"SELECT count(*) FROM {POSE_TABLE} "
        "WHERE ST_Within(lat, lon, 37.7749, -122.4194, 50)"
    )
    dock_pose_count = table_scalar_int(query_data(url, timeout, spatial_sql))

    return {
        "counts": counts,
        "count_status": counts == expected.counts,
        "failed_repeat_rows": failed_repeat_rows,
        "failed_repeat_status": failed_repeat_rows == expected.failed_repeat_rows,
        "context_top_rows": context_top_rows,
        "context_top_status": context_top_rows == expected.context_top_rows,
        "suppression_audit_rows": suppression_audit_rows,
        "suppression_audit_status": suppression_audit_rows == expected.suppression_audit_rows,
        "action_outcome_rows": action_outcome_rows,
        "action_outcome_status": action_outcome_rows == expected.action_outcome_rows,
        "robot_asof_rows": robot_asof_rows,
        "robot_asof_status": robot_asof_rows == expected.robot_asof_rows,
        "sensor_asof_rows": sensor_asof_rows,
        "sensor_asof_status": sensor_asof_rows == expected.sensor_asof_rows,
        "window_rows": window_rows,
        "window_status": window_rows == expected.window_rows,
        "lag_rows": lag_rows,
        "lag_status": lag_rows == expected.lag_rows,
        "dock_pose_count": dock_pose_count,
        "spatial_status": dock_pose_count == expected.dock_pose_count,
    }


def table_scalar_int(data: dict[str, Any]) -> int | None:
    try:
        return int(data["data"][0][0])
    except (KeyError, IndexError, TypeError, ValueError):
        return None


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
    fixture_path: Path,
    sql_output: Path,
    validation: dict[str, Any],
    action_codes: dict[str, int],
    metric_codes: dict[str, int],
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    status_keys = [
        "count_status",
        "failed_repeat_status",
        "context_top_status",
        "suppression_audit_status",
        "action_outcome_status",
        "robot_asof_status",
        "sensor_asof_status",
        "window_status",
        "lag_status",
        "spatial_status",
    ]
    overall = all(validation[key] for key in status_keys)
    lines = [
        "# Physical AI Action-Outcome Experiment 014 SQL Replay Results",
        "",
        f"Generated at: {now}",
        f"Endpoint: `{url}`",
        f"Fixture: `{fixture_path}`",
        f"SQL replay file: `{sql_output}`",
        "Classification: Research-only",
        "",
        "## Status",
        "",
        f"- Row-count status: {'pass' if validation['count_status'] else 'fail'}",
        f"- Failed-repeat JOIN status: {'pass' if validation['failed_repeat_status'] else 'fail'}",
        f"- Context top-action JOIN status: {'pass' if validation['context_top_status'] else 'fail'}",
        f"- Suppression audit JOIN status: {'pass' if validation['suppression_audit_status'] else 'fail'}",
        f"- Action/outcome JOIN status: {'pass' if validation['action_outcome_status'] else 'fail'}",
        f"- Robot state ASOF JOIN status: {'pass' if validation['robot_asof_status'] else 'fail'}",
        f"- Sensor ASOF JOIN status: {'pass' if validation['sensor_asof_status'] else 'fail'}",
        f"- ROW_NUMBER window status: {'pass' if validation['window_status'] else 'fail'}",
        f"- LAG window status: {'pass' if validation['lag_status'] else 'fail'}",
        f"- Spatial `ST_Within` status: {'pass' if validation['spatial_status'] else 'fail'}",
        f"- Overall SQL replay status: {'pass' if overall else 'fail'}",
        "",
        "## Table Counts",
        "",
        "| Table | Rows |",
        "| --- | ---: |",
    ]
    for table, count in sorted(validation["counts"].items()):
        lines.append(f"| `{table}` | {count} |")

    lines += [
        "",
        "## Failed-Repeat JOIN",
        "",
        "Native SQL finds every non-gated baseline that selected the known unsafe",
        "query action as its Top-1 recommendation.",
        "",
        *markdown_table(
            validation["failed_repeat_rows"],
            ["Variant", "Query", "Recommended Action", "Unsafe Query Action"],
        ),
        "",
        "## Context-Gated Recovery JOIN",
        "",
        "Native SQL joins context-gated recommendations to expected recovery actions.",
        "",
        *markdown_table(
            validation["context_top_rows"],
            ["Query", "Context-Gated Top Action"],
        ),
        "",
        "## Robot/Sensor ASOF JOINs",
        "",
        "The replay uses robot-operation-shaped telemetry tables and validates that",
        "each incident can bind to the latest robot state and sensor summary before",
        "the action timestamp.",
        "",
        "The native ASOF path returns numeric projections, so the replay stores",
        "semantic robot, action, and metric strings alongside stable integer codes.",
        "",
        *markdown_table(validation["robot_asof_rows"], ["Query", "Robot", "Unsafe Action"]),
        "",
        *markdown_table(validation["sensor_asof_rows"], ["Query", "Primary Metric"]),
        "",
        "### Code Maps",
        "",
        "| Code Type | Code | Meaning |",
        "| --- | ---: | --- |",
    ]
    for robot_id, code in sorted(ROBOT_SYMBOLS.items(), key=lambda item: item[1]):
        lines.append(f"| robot | {code} | `{robot_id}` |")
    for action, code in sorted(action_codes.items(), key=lambda item: item[1]):
        lines.append(f"| action | {code} | `{action}` |")
    for metric, code in sorted(metric_codes.items(), key=lambda item: item[1]):
        lines.append(f"| metric | {code} | `{metric}` |")

    lines += [
        "",
        "## Suppression Audit JOIN",
        "",
        *markdown_table(
            validation["suppression_audit_rows"],
            ["Query", "Candidate", "Suppressed Action", "Retrieval Quality"],
        ),
        "",
        "## Window And Spatial Checks",
        "",
        f"- ROW_NUMBER rows: {len(validation['window_rows'])}",
        f"- LAG rows: {len(validation['lag_rows'])}",
        f"- Dock pose rows within 50m of the dock geofence: {validation['dock_pose_count']}",
        "",
        "## Interpretation",
        "",
        "Experiment 014 moves the Physical AI comparison from a Python-only fixture",
        "into live ZeptoDB SQL. The replay validates realistic robot operation",
        "surfaces: event-time telemetry, action/outcome rows, recommendation ranks,",
        "retrieval evidence, suppressions, robot state ASOF joins, sensor ASOF joins,",
        "window ranking, and spatial geofence checks.",
        "",
        "The core result from Experiment 013 survives native SQL materialization:",
        "similar incident retrieval, runbook priors, and reflection-only memory all",
        "repeat hazardous Top-1 actions on the hard robot-safety distractors, while",
        "the context-gated Physical AI Action-Outcome path selects the expected",
        "recovery actions and exposes the suppressed misleading evidence for audit.",
        "",
        "## Next Best Step",
        "",
        "Port this replay into a two-node live topology and split edge-local memory",
        "from fleet-global memory consolidation.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8123/")
    parser.add_argument(
        "--fixture",
        type=Path,
        default=Path("docs/research/fixtures/physical_ai_action_outcome_episodes.json"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("docs/research/results/physical_ai_action_outcome_sql_replay_014.md"),
    )
    parser.add_argument(
        "--sql-output",
        type=Path,
        default=Path("docs/research/results/physical_ai_action_outcome_sql_replay_014.sql"),
    )
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--query-id", action="append", dest="query_ids")
    args = parser.parse_args()

    episodes = baseline.load_fixture(args.fixture)
    query_ids = args.query_ids or baseline.DEFAULT_QUERY_IDS
    results = build_results(episodes, query_ids)
    action_codes = build_action_codes(episodes)
    metric_codes = build_metric_codes(episodes)
    recorder = SqlRecorder()
    expected = materialize(
        args.url,
        args.timeout,
        episodes,
        results,
        query_ids,
        action_codes,
        metric_codes,
        recorder,
    )
    validation = validate(args.url, args.timeout, expected)

    args.sql_output.parent.mkdir(parents=True, exist_ok=True)
    args.sql_output.write_text(recorder.text())

    report = render_report(
        url=args.url,
        fixture_path=args.fixture,
        sql_output=args.sql_output,
        validation=validation,
        action_codes=action_codes,
        metric_codes=metric_codes,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report)

    status_keys = [
        "count_status",
        "failed_repeat_status",
        "context_top_status",
        "suppression_audit_status",
        "action_outcome_status",
        "robot_asof_status",
        "sensor_asof_status",
        "window_status",
        "lag_status",
        "spatial_status",
    ]
    if not all(validation[key] for key in status_keys):
        raise SystemExit("Physical AI SQL replay validation failed")


if __name__ == "__main__":
    main()
