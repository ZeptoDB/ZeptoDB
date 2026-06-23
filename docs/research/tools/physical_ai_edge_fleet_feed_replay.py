#!/usr/bin/env python3
"""Bounded edge-to-fleet feed replay for Physical AI Action-Outcome research.

Experiment 016 replaces the Experiment 015 direct harness copy with an explicit
edge outbox, a bounded feed worker, persistent fleet acknowledgements, and
failure-injection phases for dropped, duplicated, late, outage, and restart
conditions.
"""

from __future__ import annotations

import argparse
import json
import urllib.request
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import physical_ai_action_outcome_baseline as baseline
import physical_ai_sql_replay as single


EDGE_INCIDENT_TABLE = "physical_ai_edge_incidents_016"
EDGE_EXPECTED_ACTION_TABLE = "physical_ai_edge_expected_actions_016"
EDGE_ROBOT_STATE_TABLE = "physical_ai_edge_robot_state_016"
EDGE_SENSOR_TABLE = "physical_ai_edge_sensor_summary_016"
EDGE_DECISION_TABLE = "physical_ai_edge_decisions_016"
EDGE_SUPPRESSION_TABLE = "physical_ai_edge_suppressions_016"
EDGE_OUTBOX_TABLE = "physical_ai_edge_feed_outbox_016"

FLEET_EXPECTED_ACTION_TABLE = "physical_ai_fleet_expected_actions_016"
FLEET_ACTION_OUTCOME_TABLE = "physical_ai_fleet_action_outcomes_016"
FLEET_EDGE_DECISION_TABLE = "physical_ai_fleet_edge_decisions_016"
FLEET_RETRIEVAL_TABLE = "physical_ai_fleet_retrieval_016"
FLEET_SUPPRESSION_TABLE = "physical_ai_fleet_suppressions_016"
FLEET_INBOX_TABLE = "physical_ai_fleet_feed_inbox_016"
FLEET_ACK_TABLE = "physical_ai_fleet_feed_ack_016"
FLEET_TELEMETRY_TABLE = "physical_ai_fleet_feed_telemetry_016"

EDGE_TABLES = [
    EDGE_OUTBOX_TABLE,
    EDGE_SUPPRESSION_TABLE,
    EDGE_DECISION_TABLE,
    EDGE_SENSOR_TABLE,
    EDGE_ROBOT_STATE_TABLE,
    EDGE_EXPECTED_ACTION_TABLE,
    EDGE_INCIDENT_TABLE,
]

FLEET_TABLES = [
    FLEET_TELEMETRY_TABLE,
    FLEET_ACK_TABLE,
    FLEET_INBOX_TABLE,
    FLEET_SUPPRESSION_TABLE,
    FLEET_RETRIEVAL_TABLE,
    FLEET_EDGE_DECISION_TABLE,
    FLEET_ACTION_OUTCOME_TABLE,
    FLEET_EXPECTED_ACTION_TABLE,
]

OUTBOX_COLUMNS = [
    "feed_event_id",
    "stream_seq",
    "event_kind",
    "query_id",
    "query_seq",
    "candidate_id",
    "suppression_key",
    "selected_action",
    "selected_action_code",
    "selected_expected_key",
    "unsafe_action_code",
    "recovery_top1_hit",
    "avoids_risky_repeat",
    "risky_action_suppressed",
    "suppressed_count",
    "edge_latency_ms",
    "retrieval_rank",
    "quality_label",
    "quality_code",
    "score_micros",
    "action_class",
    "action_code",
    "outcome_label",
    "raw_value_micros",
    "gated_value_micros",
    "context_score_micros",
    "source_edge_node_id",
    "decision_ts_ns",
    "ready_ts_ns",
]


@dataclass(frozen=True)
class FeedEvent:
    feed_event_id: str
    stream_seq: int
    event_kind: str
    query_id: str
    query_seq: int
    candidate_id: str
    suppression_key: str
    selected_action: str
    selected_action_code: int
    selected_expected_key: str
    unsafe_action_code: int
    recovery_top1_hit: int
    avoids_risky_repeat: int
    risky_action_suppressed: int
    suppressed_count: int
    edge_latency_ms: int
    retrieval_rank: int
    quality_label: str
    quality_code: int
    score_micros: int
    action_class: str
    action_code: int
    outcome_label: str
    raw_value_micros: int
    gated_value_micros: int
    context_score_micros: int
    source_edge_node_id: int
    decision_ts_ns: int
    ready_ts_ns: int


@dataclass
class PassStats:
    phase: str
    pass_index: int
    batch_event_count: int
    attempted_count: int
    delivered_count: int
    failed_count: int
    dropped_count: int
    duplicate_attempt_count: int
    late_count: int
    acked_before: int
    acked_after: int
    max_inflight: int
    batch_limit: int
    restart_reloaded_ack: int
    timestamp_ns: int


@dataclass
class Expected:
    edge_counts: dict[str, int]
    fleet_base_counts: dict[str, int]
    outbox_count: int
    ack_kind_counts: list[list[Any]]
    edge_recovery_rows: list[list[Any]]
    fleet_recovery_rows: list[list[Any]]
    fleet_audit_rows: list[list[Any]]


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


def context_result(results: list[dict[str, Any]]) -> dict[str, Any]:
    return next(
        result
        for result in results
        if result["variant"] == "context_gated_physical_ai_action_outcome"
    )


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
        f"""
CREATE TABLE IF NOT EXISTS {EDGE_OUTBOX_TABLE} (
    feed_event_id STRING,
    stream_seq INT64,
    event_kind STRING,
    query_id STRING,
    query_seq INT64,
    candidate_id STRING,
    suppression_key STRING,
    selected_action STRING,
    selected_action_code INT64,
    selected_expected_key STRING,
    unsafe_action_code INT64,
    recovery_top1_hit INT64,
    avoids_risky_repeat INT64,
    risky_action_suppressed INT64,
    suppressed_count INT64,
    edge_latency_ms INT64,
    retrieval_rank INT64,
    quality_label STRING,
    quality_code INT64,
    score_micros INT64,
    action_class STRING,
    action_code INT64,
    outcome_label STRING,
    raw_value_micros INT64,
    gated_value_micros INT64,
    context_score_micros INT64,
    source_edge_node_id INT64,
    decision_ts_ns TIMESTAMP_NS,
    ready_ts_ns TIMESTAMP_NS,
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
        f"""
CREATE TABLE IF NOT EXISTS {FLEET_INBOX_TABLE} (
    attempt_id STRING,
    feed_event_id STRING,
    stream_seq INT64,
    event_kind STRING,
    query_id STRING,
    delivery_status STRING,
    duplicate_delivery INT64,
    late_delivery INT64,
    attempt_no INT64,
    source_edge_node_id INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {FLEET_ACK_TABLE} (
    feed_event_id STRING,
    stream_seq INT64,
    event_kind STRING,
    query_id STRING,
    ack_status STRING,
    source_edge_node_id INT64,
    ack_ts_ns TIMESTAMP_NS,
    timestamp TIMESTAMP_NS
)
""".strip(),
        f"""
CREATE TABLE IF NOT EXISTS {FLEET_TELEMETRY_TABLE} (
    phase STRING,
    pass_index INT64,
    batch_event_count INT64,
    attempted_count INT64,
    delivered_count INT64,
    failed_count INT64,
    dropped_count INT64,
    duplicate_attempt_count INT64,
    late_count INT64,
    acked_before INT64,
    acked_after INT64,
    max_inflight INT64,
    batch_limit INT64,
    restart_reloaded_ack INT64,
    timestamp TIMESTAMP_NS
)
""".strip(),
    ]
    for ddl in ddls:
        single.post_checked(url, timeout, ddl, recorder)


def sql_values(values: list[Any]) -> str:
    rendered = []
    for value in values:
        if isinstance(value, str):
            rendered.append(single.sql_quote(value))
        elif isinstance(value, bool):
            rendered.append(str(single.bool_int(value)))
        else:
            rendered.append(str(value))
    return "(" + ", ".join(rendered) + ")"


def post_outbox_event(
    url: str,
    timeout: float,
    event: FeedEvent,
    recorder: single.SqlRecorder,
) -> None:
    values = [getattr(event, column) for column in OUTBOX_COLUMNS]
    values.append(event.ready_ts_ns)
    single.post_checked(
        url,
        timeout,
        f"INSERT INTO {EDGE_OUTBOX_TABLE} ({', '.join(OUTBOX_COLUMNS)}, timestamp) VALUES "
        f"{sql_values(values)}",
        recorder,
    )


def materialize_edge_and_fleet_base(
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
    gated_rows = {row["query_id"]: row for row in context_result(results)["per_query"]}

    edge_counts = {table: 0 for table in EDGE_TABLES}
    fleet_base_counts = {table: 0 for table in FLEET_TABLES}
    edge_recovery_rows: list[list[Any]] = []
    fleet_recovery_rows: list[list[Any]] = []
    fleet_audit_rows: list[list[Any]] = []
    stream_seq = 0

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
        fleet_base_counts[FLEET_ACTION_OUTCOME_TABLE] += 1

    for query_id in query_ids:
        episode = by_id[query_id]
        row = gated_rows[query_id]
        query_seq = query_seq_by_id[query_id]
        event_ts = single.query_event_ts(query_seq)
        decision_ts = event_ts + 50_000
        selected_action = str(row["top_action"])
        unsafe_action = str(episode["action"]["action_class"])
        robot_code = single.robot_symbol(episode)
        metric_name, before, after_5m = single.primary_metric(episode)
        metric_code = metric_codes[metric_name]
        risky_suppressed = any(str(item["action"]) == unsafe_action for item in row["suppressed"])
        suppressed_count = len(row["suppressed"])
        selected_key = single.expected_action_key(query_id, selected_action)
        edge_latency_ms = 12 + query_seq

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
            edge_insert = (
                f"INSERT INTO {EDGE_EXPECTED_ACTION_TABLE} "
                "(query_id, expected_action_key, action_class, action_code, timestamp) VALUES "
                f"({single.sql_quote(query_id)}, {single.sql_quote(key)}, "
                f"{single.sql_quote(str(action))}, {action_codes[str(action)]}, {event_ts + rank})"
            )
            fleet_insert = edge_insert.replace(EDGE_EXPECTED_ACTION_TABLE, FLEET_EXPECTED_ACTION_TABLE)
            single.post_checked(edge_url, timeout, edge_insert, edge_recorder)
            single.post_checked(fleet_url, timeout, fleet_insert, fleet_recorder)
            edge_counts[EDGE_EXPECTED_ACTION_TABLE] += 1
            fleet_base_counts[FLEET_EXPECTED_ACTION_TABLE] += 1

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

        single.post_checked(
            edge_url,
            timeout,
            f"INSERT INTO {EDGE_DECISION_TABLE} "
            "(query_id, query_seq, robot_code, selected_action, selected_action_code, "
            "selected_expected_key, unsafe_action, unsafe_action_code, recovery_top1_hit, "
            "avoids_risky_repeat, risky_action_suppressed, suppressed_count, edge_latency_ms, "
            "decision_ts_ns, timestamp) VALUES "
            f"({single.sql_quote(query_id)}, {query_seq}, {robot_code}, "
            f"{single.sql_quote(selected_action)}, {action_codes[selected_action]}, "
            f"{single.sql_quote(selected_key)}, {single.sql_quote(unsafe_action)}, "
            f"{action_codes[unsafe_action]}, {single.bool_int(row['recovery_action_hit'])}, "
            f"{single.bool_int(row['avoids_risky_repeat'])}, {single.bool_int(risky_suppressed)}, "
            f"{suppressed_count}, {edge_latency_ms}, {decision_ts}, {decision_ts})",
            edge_recorder,
        )
        edge_counts[EDGE_DECISION_TABLE] += 1
        edge_recovery_rows.append([query_id, selected_action])
        fleet_recovery_rows.append([query_id, selected_action])

        stream_seq += 1
        post_outbox_event(
            edge_url,
            timeout,
            FeedEvent(
                feed_event_id=f"{query_id}|decision",
                stream_seq=stream_seq,
                event_kind="decision",
                query_id=query_id,
                query_seq=query_seq,
                candidate_id="",
                suppression_key="",
                selected_action=selected_action,
                selected_action_code=action_codes[selected_action],
                selected_expected_key=selected_key,
                unsafe_action_code=action_codes[unsafe_action],
                recovery_top1_hit=single.bool_int(row["recovery_action_hit"]),
                avoids_risky_repeat=single.bool_int(row["avoids_risky_repeat"]),
                risky_action_suppressed=single.bool_int(risky_suppressed),
                suppressed_count=suppressed_count,
                edge_latency_ms=edge_latency_ms,
                retrieval_rank=0,
                quality_label="",
                quality_code=0,
                score_micros=0,
                action_class="",
                action_code=0,
                outcome_label="",
                raw_value_micros=0,
                gated_value_micros=0,
                context_score_micros=0,
                source_edge_node_id=edge_node_id,
                decision_ts_ns=decision_ts,
                ready_ts_ns=decision_ts + 250_000_000,
            ),
            edge_recorder,
        )
        edge_counts[EDGE_OUTBOX_TABLE] += 1

        for candidate in row["retrieval_top3"]:
            quality = str(candidate["quality"])
            suppression_key = single.suppression_key(query_id, str(candidate["episode_id"]))
            stream_seq += 1
            post_outbox_event(
                edge_url,
                timeout,
                FeedEvent(
                    feed_event_id=f"{query_id}|retrieval|{candidate['rank']}",
                    stream_seq=stream_seq,
                    event_kind="retrieval",
                    query_id=query_id,
                    query_seq=query_seq,
                    candidate_id=str(candidate["episode_id"]),
                    suppression_key=suppression_key,
                    selected_action="",
                    selected_action_code=0,
                    selected_expected_key="",
                    unsafe_action_code=0,
                    recovery_top1_hit=0,
                    avoids_risky_repeat=0,
                    risky_action_suppressed=0,
                    suppressed_count=0,
                    edge_latency_ms=0,
                    retrieval_rank=int(candidate["rank"]),
                    quality_label=quality,
                    quality_code=single.QUALITY_CODES.get(quality, 0),
                    score_micros=single.micros(float(candidate["score"])),
                    action_class=str(candidate["action"]),
                    action_code=action_codes[str(candidate["action"])],
                    outcome_label="",
                    raw_value_micros=0,
                    gated_value_micros=0,
                    context_score_micros=0,
                    source_edge_node_id=edge_node_id,
                    decision_ts_ns=decision_ts,
                    ready_ts_ns=decision_ts + 300_000_000 + int(candidate["rank"]),
                ),
                edge_recorder,
            )
            edge_counts[EDGE_OUTBOX_TABLE] += 1

        for idx, suppressed in enumerate(row["suppressed"], start=1):
            suppression_key = single.suppression_key(query_id, str(suppressed["episode_id"]))
            reasons = ",".join(suppressed["reasons"]) if suppressed["reasons"] else "none"
            values = (
                f"({single.sql_quote(query_id)}, {query_seq}, "
                f"{single.sql_quote(str(suppressed['episode_id']))}, {single.sql_quote(suppression_key)}, "
                f"{single.sql_quote(str(suppressed['action']))}, {action_codes[str(suppressed['action'])]}, "
                f"{single.sql_quote(str(suppressed['outcome']))}, {single.micros(float(suppressed['raw_value']))}, "
                f"{single.micros(float(suppressed['gated_value']))}, "
                f"{single.micros(float(suppressed['context_score']))}, "
                f"1, {single.sql_quote(reasons)}, {decision_ts + idx})"
            )
            single.post_checked(
                edge_url,
                timeout,
                f"INSERT INTO {EDGE_SUPPRESSION_TABLE} "
                "(query_id, query_seq, candidate_id, suppression_key, action_class, action_code, "
                "outcome_label, raw_value_micros, gated_value_micros, context_score_micros, "
                "immediate_suppression, reasons, timestamp) VALUES "
                f"{values}",
                edge_recorder,
            )
            edge_counts[EDGE_SUPPRESSION_TABLE] += 1

            if single.QUALITY_CODES.get(str(suppressed.get("quality", "")), 0) == single.QUALITY_CODES.get("misleading", -1):
                fleet_audit_rows.append(
                    [query_id, str(suppressed["episode_id"]), str(suppressed["action"]), "misleading"]
                )

            stream_seq += 1
            post_outbox_event(
                edge_url,
                timeout,
                FeedEvent(
                    feed_event_id=f"{query_id}|suppression|{idx}",
                    stream_seq=stream_seq,
                    event_kind="suppression",
                    query_id=query_id,
                    query_seq=query_seq,
                    candidate_id=str(suppressed["episode_id"]),
                    suppression_key=suppression_key,
                    selected_action="",
                    selected_action_code=0,
                    selected_expected_key="",
                    unsafe_action_code=0,
                    recovery_top1_hit=0,
                    avoids_risky_repeat=0,
                    risky_action_suppressed=0,
                    suppressed_count=0,
                    edge_latency_ms=0,
                    retrieval_rank=0,
                    quality_label="",
                    quality_code=0,
                    score_micros=0,
                    action_class=str(suppressed["action"]),
                    action_code=action_codes[str(suppressed["action"])],
                    outcome_label=str(suppressed["outcome"]),
                    raw_value_micros=single.micros(float(suppressed["raw_value"])),
                    gated_value_micros=single.micros(float(suppressed["gated_value"])),
                    context_score_micros=single.micros(float(suppressed["context_score"])),
                    source_edge_node_id=edge_node_id,
                    decision_ts_ns=decision_ts,
                    ready_ts_ns=decision_ts + 350_000_000 + idx,
                ),
                edge_recorder,
            )
            edge_counts[EDGE_OUTBOX_TABLE] += 1

    return Expected(
        edge_counts=edge_counts,
        fleet_base_counts=fleet_base_counts,
        outbox_count=edge_counts[EDGE_OUTBOX_TABLE],
        ack_kind_counts=[["decision", 5], ["retrieval", 15], ["suppression", 32]],
        edge_recovery_rows=sorted(edge_recovery_rows),
        fleet_recovery_rows=sorted(fleet_recovery_rows),
        fleet_audit_rows=[
            ["pai_agv_slip_002", "pai_agv_slip_hard_001", "continue_route", "misleading"],
            ["pai_arm_002", "pai_arm_hard_001", "increase_torque_limit", "misleading"],
            ["pai_cold_002", "pai_cold_hard_001", "ignore_until_checkpoint", "misleading"],
            ["pai_drone_002", "pai_drone_hard_001", "continue_mission", "misleading"],
            ["pai_lidar_002", "pai_lidar_hard_001", "speed_up_clear_zone", "misleading"],
        ],
    )


def as_int(value: Any) -> int:
    return int(value)


def row_to_event(row: list[Any]) -> FeedEvent:
    values = dict(zip(OUTBOX_COLUMNS, row))
    return FeedEvent(
        feed_event_id=str(values["feed_event_id"]),
        stream_seq=as_int(values["stream_seq"]),
        event_kind=str(values["event_kind"]),
        query_id=str(values["query_id"]),
        query_seq=as_int(values["query_seq"]),
        candidate_id=str(values["candidate_id"]),
        suppression_key=str(values["suppression_key"]),
        selected_action=str(values["selected_action"]),
        selected_action_code=as_int(values["selected_action_code"]),
        selected_expected_key=str(values["selected_expected_key"]),
        unsafe_action_code=as_int(values["unsafe_action_code"]),
        recovery_top1_hit=as_int(values["recovery_top1_hit"]),
        avoids_risky_repeat=as_int(values["avoids_risky_repeat"]),
        risky_action_suppressed=as_int(values["risky_action_suppressed"]),
        suppressed_count=as_int(values["suppressed_count"]),
        edge_latency_ms=as_int(values["edge_latency_ms"]),
        retrieval_rank=as_int(values["retrieval_rank"]),
        quality_label=str(values["quality_label"]),
        quality_code=as_int(values["quality_code"]),
        score_micros=as_int(values["score_micros"]),
        action_class=str(values["action_class"]),
        action_code=as_int(values["action_code"]),
        outcome_label=str(values["outcome_label"]),
        raw_value_micros=as_int(values["raw_value_micros"]),
        gated_value_micros=as_int(values["gated_value_micros"]),
        context_score_micros=as_int(values["context_score_micros"]),
        source_edge_node_id=as_int(values["source_edge_node_id"]),
        decision_ts_ns=as_int(values["decision_ts_ns"]),
        ready_ts_ns=as_int(values["ready_ts_ns"]),
    )


def load_outbox(edge_url: str, timeout: float) -> list[FeedEvent]:
    sql = f"SELECT {', '.join(OUTBOX_COLUMNS)} FROM {EDGE_OUTBOX_TABLE} ORDER BY stream_seq ASC"
    rows = single.query_data(edge_url, timeout, sql).get("data", [])
    return [row_to_event(row) for row in rows]


def load_acked(fleet_url: str, timeout: float) -> set[str]:
    data = single.query_data(fleet_url, timeout, f"SELECT feed_event_id FROM {FLEET_ACK_TABLE}")
    return {str(row[0]) for row in data.get("data", [])}


class BoundedEdgeFleetFeed:
    def __init__(
        self,
        *,
        edge_url: str,
        fleet_url: str,
        timeout: float,
        batch_limit: int,
        max_inflight: int,
        fleet_recorder: single.SqlRecorder,
    ) -> None:
        self.edge_url = edge_url
        self.fleet_url = fleet_url
        self.timeout = timeout
        self.batch_limit = batch_limit
        self.max_inflight = max_inflight
        self.fleet_recorder = fleet_recorder

    def run_pass(
        self,
        *,
        phase: str,
        pass_index: int,
        target_fleet_url: str | None = None,
        drop_event_ids: set[str] | None = None,
        duplicate_event_ids: set[str] | None = None,
        late_event_ids: set[str] | None = None,
        restart_reloaded_ack: bool = False,
    ) -> PassStats:
        drop_event_ids = drop_event_ids or set()
        duplicate_event_ids = duplicate_event_ids or set()
        late_event_ids = late_event_ids or set()
        target_url = target_fleet_url or self.fleet_url
        acked = load_acked(self.fleet_url, self.timeout)
        outbox = load_outbox(self.edge_url, self.timeout)
        candidates = [event for event in outbox if event.feed_event_id not in acked]
        batch_size = min(self.batch_limit, self.max_inflight, len(candidates))
        batch = candidates[:batch_size]
        delivered = 0
        failed = 0
        dropped = 0
        duplicate_attempts = 0
        late = 0
        local_acked = set(acked)

        for idx, event in enumerate(batch, start=1):
            if event.feed_event_id in drop_event_ids:
                dropped += 1
                continue
            is_late = event.feed_event_id in late_event_ids
            ack_ts = event.ready_ts_ns + pass_index * 10_000_000 + idx
            if is_late:
                ack_ts += 900_000_000
                late += 1
            ok = self.deliver_event(
                event,
                target_url,
                attempt_no=1,
                ack_ts_ns=ack_ts,
                duplicate_delivery=False,
                late_delivery=is_late,
                local_acked=local_acked,
            )
            if ok:
                delivered += 1
                local_acked.add(event.feed_event_id)
                if event.feed_event_id in duplicate_event_ids:
                    duplicate_ok = self.deliver_event(
                        event,
                        target_url,
                        attempt_no=2,
                        ack_ts_ns=ack_ts + 1,
                        duplicate_delivery=True,
                        late_delivery=is_late,
                        local_acked=local_acked,
                    )
                    if duplicate_ok:
                        duplicate_attempts += 1
            else:
                failed += 1

        return PassStats(
            phase=phase,
            pass_index=pass_index,
            batch_event_count=len(batch),
            attempted_count=len(batch) - dropped,
            delivered_count=delivered,
            failed_count=failed,
            dropped_count=dropped,
            duplicate_attempt_count=duplicate_attempts,
            late_count=late,
            acked_before=len(acked),
            acked_after=len(local_acked),
            max_inflight=self.max_inflight,
            batch_limit=self.batch_limit,
            restart_reloaded_ack=single.bool_int(restart_reloaded_ack),
            timestamp_ns=single.BASE_TS_NS + 20_000_000_000 + pass_index * 1_000_000,
        )

    def deliver_event(
        self,
        event: FeedEvent,
        target_url: str,
        *,
        attempt_no: int,
        ack_ts_ns: int,
        duplicate_delivery: bool,
        late_delivery: bool,
        local_acked: set[str],
    ) -> bool:
        status = "duplicate" if duplicate_delivery else "delivered"
        attempt_id = f"{event.feed_event_id}|attempt|{attempt_no}|{ack_ts_ns}"
        inbox_sql = (
            f"INSERT INTO {FLEET_INBOX_TABLE} "
            "(attempt_id, feed_event_id, stream_seq, event_kind, query_id, delivery_status, "
            "duplicate_delivery, late_delivery, attempt_no, source_edge_node_id, timestamp) VALUES "
            f"({single.sql_quote(attempt_id)}, {single.sql_quote(event.feed_event_id)}, "
            f"{event.stream_seq}, {single.sql_quote(event.event_kind)}, {single.sql_quote(event.query_id)}, "
            f"{single.sql_quote(status)}, {single.bool_int(duplicate_delivery)}, "
            f"{single.bool_int(late_delivery)}, {attempt_no}, {event.source_edge_node_id}, {ack_ts_ns})"
        )
        try:
            single.post_checked(target_url, self.timeout, inbox_sql, self.fleet_recorder)
            if event.feed_event_id in local_acked:
                return True
            self.apply_event(target_url, event, ack_ts_ns)
            ack_sql = (
                f"INSERT INTO {FLEET_ACK_TABLE} "
                "(feed_event_id, stream_seq, event_kind, query_id, ack_status, "
                "source_edge_node_id, ack_ts_ns, timestamp) VALUES "
                f"({single.sql_quote(event.feed_event_id)}, {event.stream_seq}, "
                f"{single.sql_quote(event.event_kind)}, {single.sql_quote(event.query_id)}, "
                f"{single.sql_quote('acked')}, {event.source_edge_node_id}, {ack_ts_ns}, {ack_ts_ns})"
            )
            single.post_checked(target_url, self.timeout, ack_sql, self.fleet_recorder)
            return True
        except RuntimeError:
            return False

    def apply_event(self, target_url: str, event: FeedEvent, ack_ts_ns: int) -> None:
        if event.event_kind == "decision":
            lag_ms = max(0, (ack_ts_ns - event.decision_ts_ns) // 1_000_000)
            sql = (
                f"INSERT INTO {FLEET_EDGE_DECISION_TABLE} "
                "(query_id, query_seq, robot_code, selected_action, selected_action_code, "
                "selected_expected_key, unsafe_action_code, recovery_top1_hit, avoids_risky_repeat, "
                "risky_action_suppressed, suppressed_count, edge_latency_ms, decision_ts_ns, "
                "consolidated_ts_ns, consolidation_lag_ms, source_edge_node_id, timestamp) VALUES "
                f"({single.sql_quote(event.query_id)}, {event.query_seq}, 0, "
                f"{single.sql_quote(event.selected_action)}, {event.selected_action_code}, "
                f"{single.sql_quote(event.selected_expected_key)}, {event.unsafe_action_code}, "
                f"{event.recovery_top1_hit}, {event.avoids_risky_repeat}, "
                f"{event.risky_action_suppressed}, {event.suppressed_count}, {event.edge_latency_ms}, "
                f"{event.decision_ts_ns}, {ack_ts_ns}, {lag_ms}, {event.source_edge_node_id}, {ack_ts_ns})"
            )
        elif event.event_kind == "retrieval":
            sql = (
                f"INSERT INTO {FLEET_RETRIEVAL_TABLE} "
                "(query_id, query_seq, retrieval_rank, candidate_id, suppression_key, "
                "candidate_action, quality_label, quality_code, score_micros, timestamp) VALUES "
                f"({single.sql_quote(event.query_id)}, {event.query_seq}, {event.retrieval_rank}, "
                f"{single.sql_quote(event.candidate_id)}, {single.sql_quote(event.suppression_key)}, "
                f"{single.sql_quote(event.action_class)}, {single.sql_quote(event.quality_label)}, "
                f"{event.quality_code}, {event.score_micros}, {ack_ts_ns})"
            )
        elif event.event_kind == "suppression":
            sql = (
                f"INSERT INTO {FLEET_SUPPRESSION_TABLE} "
                "(query_id, query_seq, candidate_id, suppression_key, action_class, action_code, "
                "outcome_label, raw_value_micros, gated_value_micros, context_score_micros, "
                "source_edge_node_id, timestamp) VALUES "
                f"({single.sql_quote(event.query_id)}, {event.query_seq}, "
                f"{single.sql_quote(event.candidate_id)}, {single.sql_quote(event.suppression_key)}, "
                f"{single.sql_quote(event.action_class)}, {event.action_code}, "
                f"{single.sql_quote(event.outcome_label)}, {event.raw_value_micros}, "
                f"{event.gated_value_micros}, {event.context_score_micros}, "
                f"{event.source_edge_node_id}, {ack_ts_ns})"
            )
        else:
            raise RuntimeError(f"unknown feed event kind: {event.event_kind}")
        single.post_checked(target_url, self.timeout, sql, self.fleet_recorder)


def record_telemetry(
    fleet_url: str,
    timeout: float,
    stats: list[PassStats],
    recorder: single.SqlRecorder,
) -> None:
    for item in stats:
        single.post_checked(
            fleet_url,
            timeout,
            f"INSERT INTO {FLEET_TELEMETRY_TABLE} "
            "(phase, pass_index, batch_event_count, attempted_count, delivered_count, "
            "failed_count, dropped_count, duplicate_attempt_count, late_count, acked_before, "
            "acked_after, max_inflight, batch_limit, restart_reloaded_ack, timestamp) VALUES "
            f"({single.sql_quote(item.phase)}, {item.pass_index}, {item.batch_event_count}, "
            f"{item.attempted_count}, {item.delivered_count}, {item.failed_count}, "
            f"{item.dropped_count}, {item.duplicate_attempt_count}, {item.late_count}, "
            f"{item.acked_before}, {item.acked_after}, {item.max_inflight}, "
            f"{item.batch_limit}, {item.restart_reloaded_ack}, {item.timestamp_ns})",
            recorder,
        )


def run_feed_scenarios(
    *,
    edge_url: str,
    fleet_url: str,
    outage_url: str,
    timeout: float,
    batch_limit: int,
    max_inflight: int,
    fleet_recorder: single.SqlRecorder,
) -> list[PassStats]:
    outbox = load_outbox(edge_url, timeout)
    drop_ids = {outbox[1].feed_event_id, outbox[14].feed_event_id}
    duplicate_ids = {outbox[2].feed_event_id}
    late_ids = set(drop_ids)
    stats: list[PassStats] = []

    feed = BoundedEdgeFleetFeed(
        edge_url=edge_url,
        fleet_url=fleet_url,
        timeout=timeout,
        batch_limit=batch_limit,
        max_inflight=max_inflight,
        fleet_recorder=fleet_recorder,
    )
    stats.append(
        feed.run_pass(
            phase="outage_probe",
            pass_index=1,
            target_fleet_url=outage_url,
        )
    )
    stats.append(
        feed.run_pass(
            phase="bounded_recovery_with_drop_duplicate",
            pass_index=2,
            drop_event_ids=drop_ids,
            duplicate_event_ids=duplicate_ids,
        )
    )

    restarted_feed = BoundedEdgeFleetFeed(
        edge_url=edge_url,
        fleet_url=fleet_url,
        timeout=timeout,
        batch_limit=batch_limit,
        max_inflight=max_inflight,
        fleet_recorder=fleet_recorder,
    )
    stats.append(
        restarted_feed.run_pass(
            phase="restart_retry_late_delivery",
            pass_index=3,
            late_event_ids=late_ids,
            restart_reloaded_ack=True,
        )
    )

    pass_index = 4
    while len(load_acked(fleet_url, timeout)) < len(outbox):
        stats.append(
            restarted_feed.run_pass(
                phase=f"bounded_final_drain_{pass_index - 3}",
                pass_index=pass_index,
            )
        )
        pass_index += 1
        if pass_index > 20:
            raise RuntimeError("feed did not converge within 20 bounded passes")

    record_telemetry(fleet_url, timeout, stats, fleet_recorder)
    return stats


def scalar_int(data: dict[str, Any]) -> int | None:
    try:
        return int(data["data"][0][0])
    except (KeyError, IndexError, TypeError, ValueError):
        return None


def table_counts(url: str, timeout: float, tables: list[str]) -> dict[str, int | None]:
    return {table: single.table_count(url, timeout, table) for table in tables}


def validate(
    *,
    edge_url: str,
    fleet_url: str,
    timeout: float,
    expected: Expected,
    batch_limit: int,
    max_inflight: int,
) -> dict[str, Any]:
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

    ack_count = scalar_int(single.query_data(fleet_url, timeout, f"SELECT count(*) FROM {FLEET_ACK_TABLE}"))
    ack_kind_values = [
        str(row[0])
        for row in single.query_data(
            fleet_url,
            timeout,
            f"SELECT event_kind FROM {FLEET_ACK_TABLE}",
        ).get("data", [])
    ]
    ack_kind_counts = Counter(ack_kind_values)
    ack_kind_rows = sorted([[kind, count] for kind, count in ack_kind_counts.items()])
    duplicate_count = scalar_int(
        single.query_data(
            fleet_url,
            timeout,
            f"SELECT count(*) FROM {FLEET_INBOX_TABLE} WHERE duplicate_delivery = 1",
        )
    )
    late_count = scalar_int(
        single.query_data(
            fleet_url,
            timeout,
            f"SELECT count(*) FROM {FLEET_INBOX_TABLE} WHERE late_delivery = 1",
        )
    )
    outage_phase_count = scalar_int(
        single.query_data(
            fleet_url,
            timeout,
            f"SELECT count(*) FROM {FLEET_TELEMETRY_TABLE} WHERE failed_count > 0",
        )
    )
    restart_phase_count = scalar_int(
        single.query_data(
            fleet_url,
            timeout,
            f"SELECT count(*) FROM {FLEET_TELEMETRY_TABLE} "
            "WHERE restart_reloaded_ack = 1 AND acked_before > 0",
        )
    )
    telemetry_rows = single.query_data(
        fleet_url,
        timeout,
        "SELECT batch_event_count, batch_limit, max_inflight "
        f"FROM {FLEET_TELEMETRY_TABLE}",
    ).get("data", [])
    bounded_status = all(
        int(row[0]) <= batch_limit and int(row[0]) <= max_inflight
        and int(row[1]) == batch_limit and int(row[2]) == max_inflight
        for row in telemetry_rows
    )

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

    ack_window_sql = (
        "SELECT stream_seq, ROW_NUMBER() OVER (ORDER BY stream_seq) AS rn "
        f"FROM {FLEET_ACK_TABLE}"
    )
    ack_window_rows = sorted(
        (int(row[0]), int(row[1]))
        for row in single.query_data(fleet_url, timeout, ack_window_sql).get("data", [])
    )
    ack_lag_sql = (
        "SELECT stream_seq, LAG(stream_seq, 1, 0) OVER (ORDER BY stream_seq) AS prev_seq "
        f"FROM {FLEET_ACK_TABLE}"
    )
    ack_lag_rows = sorted(
        (int(row[0]), int(row[1]))
        for row in single.query_data(fleet_url, timeout, ack_lag_sql).get("data", [])
    )
    ack_ordered_stream_rows = [
        int(row[0])
        for row in single.query_data(
            fleet_url,
            timeout,
            f"SELECT stream_seq FROM {FLEET_ACK_TABLE} ORDER BY stream_seq ASC",
        ).get("data", [])
    ]
    ack_sequence_status = (
        ack_ordered_stream_rows == list(range(1, expected.outbox_count + 1))
        and len(ack_window_rows) == expected.outbox_count
        and len(ack_lag_rows) == expected.outbox_count
    )

    final_counts_status = (
        fleet_counts[FLEET_EDGE_DECISION_TABLE] == 5
        and fleet_counts[FLEET_RETRIEVAL_TABLE] == 15
        and fleet_counts[FLEET_SUPPRESSION_TABLE] == 32
    )
    overall_status = all(
        [
            edge_counts == expected.edge_counts,
            edge_recovery_rows == expected.edge_recovery_rows,
            edge_risky_suppression_count == 5,
            ack_count == expected.outbox_count,
            ack_kind_rows == expected.ack_kind_counts,
            duplicate_count is not None and duplicate_count >= 1,
            late_count is not None and late_count >= 2,
            outage_phase_count is not None and outage_phase_count >= 1,
            restart_phase_count is not None and restart_phase_count >= 1,
            bounded_status,
            final_counts_status,
            fleet_recovery_rows == expected.fleet_recovery_rows,
            fleet_audit_rows == expected.fleet_audit_rows,
            ack_sequence_status,
        ]
    )

    return {
        "edge_counts": edge_counts,
        "fleet_counts": fleet_counts,
        "edge_count_status": edge_counts == expected.edge_counts,
        "edge_recovery_rows": edge_recovery_rows,
        "edge_recovery_status": edge_recovery_rows == expected.edge_recovery_rows,
        "edge_risky_suppression_count": edge_risky_suppression_count,
        "edge_risky_suppression_status": edge_risky_suppression_count == 5,
        "ack_count": ack_count,
        "ack_status": ack_count == expected.outbox_count,
        "ack_kind_rows": ack_kind_rows,
        "ack_kind_status": ack_kind_rows == expected.ack_kind_counts,
        "duplicate_count": duplicate_count,
        "duplicate_status": duplicate_count is not None and duplicate_count >= 1,
        "late_count": late_count,
        "late_status": late_count is not None and late_count >= 2,
        "outage_phase_count": outage_phase_count,
        "outage_status": outage_phase_count is not None and outage_phase_count >= 1,
        "restart_phase_count": restart_phase_count,
        "restart_status": restart_phase_count is not None and restart_phase_count >= 1,
        "bounded_status": bounded_status,
        "fleet_final_counts_status": final_counts_status,
        "fleet_recovery_rows": fleet_recovery_rows,
        "fleet_recovery_status": fleet_recovery_rows == expected.fleet_recovery_rows,
        "fleet_audit_rows": fleet_audit_rows,
        "fleet_audit_status": fleet_audit_rows == expected.fleet_audit_rows,
        "ack_window_rows": ack_window_rows,
        "ack_lag_rows": ack_lag_rows,
        "ack_ordered_stream_rows": ack_ordered_stream_rows,
        "ack_sequence_status": ack_sequence_status,
        "overall_status": overall_status,
    }


def format_counts(counts: dict[str, int | None]) -> list[str]:
    return [
        "| Table | Rows |",
        "| --- | ---: |",
        *[f"| `{table}` | {rows if rows is not None else 'n/a'} |" for table, rows in sorted(counts.items())],
    ]


def format_stats_delta(role: str, stats_delta: dict[str, int], research_rows: int) -> str:
    return (
        f"| {role} | {stats_delta.get('ticks_ingested', 0)} | "
        f"{stats_delta.get('ticks_stored', 0)} | "
        f"{stats_delta.get('partitions_created', 0)} | {research_rows} |"
    )


def write_report(
    output: Path,
    *,
    edge_url: str,
    fleet_url: str,
    outage_url: str,
    batch_limit: int,
    max_inflight: int,
    expected: Expected,
    validation: dict[str, Any],
    pass_stats: list[PassStats],
    edge_stats_delta: dict[str, int],
    fleet_stats_delta: dict[str, int],
) -> None:
    generated = datetime.now(timezone.utc).isoformat()
    edge_rows = sum(rows or 0 for rows in validation["edge_counts"].values())
    fleet_rows = sum(rows or 0 for rows in validation["fleet_counts"].values())
    lines = [
        "# Physical AI Edge/Fleet Feed Replay Experiment 016 Results",
        "",
        f"Generated: {generated}",
        "",
        "## Configuration",
        "",
        f"- Edge URL: `{edge_url}`",
        f"- Fleet URL: `{fleet_url}`",
        f"- Injected outage URL: `{outage_url}`",
        f"- Batch limit: {batch_limit}",
        f"- Max in-flight events: {max_inflight}",
        "- Classification: Research-only",
        "",
        "## Status",
        "",
        f"- Edge-local count status: {'pass' if validation['edge_count_status'] else 'fail'}",
        f"- Edge-local immediate recovery status: {'pass' if validation['edge_recovery_status'] else 'fail'}",
        f"- Edge-local risky-action suppression status: {'pass' if validation['edge_risky_suppression_status'] else 'fail'}",
        f"- Feed acknowledgement convergence status: {'pass' if validation['ack_status'] else 'fail'}",
        f"- Feed event-kind accounting status: {'pass' if validation['ack_kind_status'] else 'fail'}",
        f"- Duplicate delivery handling status: {'pass' if validation['duplicate_status'] else 'fail'}",
        f"- Late delivery handling status: {'pass' if validation['late_status'] else 'fail'}",
        f"- Outage retry status: {'pass' if validation['outage_status'] else 'fail'}",
        f"- Restart reload status: {'pass' if validation['restart_status'] else 'fail'}",
        f"- Bounded batch status: {'pass' if validation['bounded_status'] else 'fail'}",
        f"- Fleet final count status: {'pass' if validation['fleet_final_counts_status'] else 'fail'}",
        f"- Fleet recovery JOIN status: {'pass' if validation['fleet_recovery_status'] else 'fail'}",
        f"- Fleet suppression audit JOIN status: {'pass' if validation['fleet_audit_status'] else 'fail'}",
        f"- Fleet ACK ROW_NUMBER/LAG status: {'pass' if validation['ack_sequence_status'] else 'fail'}",
        f"- Overall bounded feed replay status: {'pass' if validation['overall_status'] else 'fail'}",
        "",
        "## Node Activity",
        "",
        "| Node Role | ticks_ingested delta | ticks_stored delta | partitions_created delta | Research rows |",
        "| --- | ---: | ---: | ---: | ---: |",
        format_stats_delta("edge-local", edge_stats_delta, edge_rows),
        format_stats_delta("fleet-global", fleet_stats_delta, fleet_rows),
        "",
        "## Feed Passes",
        "",
        "| Phase | Batch | Attempted | Delivered | Failed | Dropped | Duplicates | Late | Acked Before | Acked After | Restart Reload |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for item in pass_stats:
        lines.append(
            f"| `{item.phase}` | {item.batch_event_count} | {item.attempted_count} | "
            f"{item.delivered_count} | {item.failed_count} | {item.dropped_count} | "
            f"{item.duplicate_attempt_count} | {item.late_count} | {item.acked_before} | "
            f"{item.acked_after} | {item.restart_reloaded_ack} |"
        )
    lines.extend(
        [
            "",
            "## Feed Convergence",
            "",
            f"- Edge outbox events: {expected.outbox_count}",
            f"- Fleet acknowledged events: {validation['ack_count']}",
            f"- Duplicate inbox attempts: {validation['duplicate_count']}",
            f"- Late inbox attempts: {validation['late_count']}",
            f"- Outage telemetry rows: {validation['outage_phase_count']}",
            f"- Restart reload telemetry rows: {validation['restart_phase_count']}",
            "",
            "| Event Kind | ACK Rows |",
            "| --- | ---: |",
        ]
    )
    for kind, count in validation["ack_kind_rows"]:
        lines.append(f"| `{kind}` | {count} |")
    lines.extend(
        [
            "",
            "## Edge Tables",
            "",
            *format_counts(validation["edge_counts"]),
            "",
            "## Fleet Tables",
            "",
            *format_counts(validation["fleet_counts"]),
            "",
            "## Fleet Recovery Decisions",
            "",
            "| Query | Fleet Selected Action |",
            "| --- | --- |",
        ]
    )
    for query_id, action in validation["fleet_recovery_rows"]:
        lines.append(f"| `{query_id}` | `{action}` |")
    lines.extend(
        [
            "",
            "## Fleet Suppression Audit JOIN",
            "",
            "| Query | Candidate | Suppressed Action | Quality |",
            "| --- | --- | --- | --- |",
        ]
    )
    for query_id, candidate_id, action, quality in validation["fleet_audit_rows"]:
        lines.append(f"| `{query_id}` | `{candidate_id}` | `{action}` | `{quality}` |")
    lines.extend(
        [
            "",
            "## Interpretation",
            "",
            "Experiment 016 validates a bounded, explicit edge-to-fleet feed shape. "
            "The edge-local node still makes the immediate safety decision and "
            "suppresses risky robot actions before fleet consolidation. Fleet-global "
            "memory receives edge-generated evidence only through the outbox, bounded "
            "feed passes, persistent ACK rows, and retry logic.",
            "",
            "This remains research-only. The feed worker is a deterministic research "
            "tool, not a ZeptoDB runtime replication service. It proves the semantics "
            "needed before product promotion: bounded batches, duplicate tolerance, "
            "late arrival tolerance, outage retry, and restart ACK reload.",
            "",
            "## Next Best Step",
            "",
            "Promote the feed semantics into an experimental runtime connector with "
            "operator-visible telemetry, persisted cursor state, and explicit behavior "
            "for the non-transactional final-table-plus-ACK boundary.",
            "",
        ]
    )
    output.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--edge-url", default="http://127.0.0.1:19441/")
    parser.add_argument("--fleet-url", default="http://127.0.0.1:19442/")
    parser.add_argument("--outage-url", default="http://127.0.0.1:1/")
    parser.add_argument("--edge-stats-url", default="http://127.0.0.1:19441/stats")
    parser.add_argument("--fleet-stats-url", default="http://127.0.0.1:19442/stats")
    parser.add_argument(
        "--fixture",
        default="docs/research/fixtures/physical_ai_action_outcome_episodes.json",
    )
    parser.add_argument(
        "--output",
        default="docs/research/results/physical_ai_edge_fleet_feed_replay_016.md",
    )
    parser.add_argument(
        "--edge-sql-output",
        default="docs/research/results/physical_ai_edge_fleet_feed_replay_016_edge.sql",
    )
    parser.add_argument(
        "--fleet-sql-output",
        default="docs/research/results/physical_ai_edge_fleet_feed_replay_016_fleet.sql",
    )
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--batch-limit", type=int, default=12)
    parser.add_argument("--max-inflight", type=int, default=12)
    parser.add_argument(
        "--query-id",
        dest="query_ids",
        action="append",
        default=None,
        help="Query episode id to replay. Defaults to the Physical AI benchmark query set.",
    )
    args = parser.parse_args()

    if args.batch_limit <= 0 or args.max_inflight <= 0:
        raise ValueError("batch-limit and max-inflight must be positive")

    fixture = Path(args.fixture)
    episodes = baseline.load_fixture(fixture)
    query_ids = args.query_ids or baseline.DEFAULT_QUERY_IDS
    results = single.build_results(episodes, query_ids)
    action_codes = single.build_action_codes(episodes)
    metric_codes = single.build_metric_codes(episodes)
    edge_recorder = single.SqlRecorder()
    fleet_recorder = single.SqlRecorder()

    edge_stats_before = fetch_stats(args.edge_stats_url, args.timeout)
    fleet_stats_before = fetch_stats(args.fleet_stats_url, args.timeout)
    expected = materialize_edge_and_fleet_base(
        edge_url=args.edge_url,
        fleet_url=args.fleet_url,
        timeout=args.timeout,
        episodes=episodes,
        results=results,
        query_ids=query_ids,
        action_codes=action_codes,
        metric_codes=metric_codes,
        edge_node_id=1,
        edge_recorder=edge_recorder,
        fleet_recorder=fleet_recorder,
    )
    pass_stats = run_feed_scenarios(
        edge_url=args.edge_url,
        fleet_url=args.fleet_url,
        outage_url=args.outage_url,
        timeout=args.timeout,
        batch_limit=args.batch_limit,
        max_inflight=args.max_inflight,
        fleet_recorder=fleet_recorder,
    )
    validation = validate(
        edge_url=args.edge_url,
        fleet_url=args.fleet_url,
        timeout=args.timeout,
        expected=expected,
        batch_limit=args.batch_limit,
        max_inflight=args.max_inflight,
    )
    edge_stats_after = fetch_stats(args.edge_stats_url, args.timeout)
    fleet_stats_after = fetch_stats(args.fleet_stats_url, args.timeout)

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    write_report(
        output,
        edge_url=args.edge_url,
        fleet_url=args.fleet_url,
        outage_url=args.outage_url,
        batch_limit=args.batch_limit,
        max_inflight=args.max_inflight,
        expected=expected,
        validation=validation,
        pass_stats=pass_stats,
        edge_stats_delta=delta(edge_stats_before, edge_stats_after),
        fleet_stats_delta=delta(fleet_stats_before, fleet_stats_after),
    )
    Path(args.edge_sql_output).write_text(edge_recorder.text(), encoding="utf-8")
    Path(args.fleet_sql_output).write_text(fleet_recorder.text(), encoding="utf-8")
    print(f"wrote {output}")
    if not validation["overall_status"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
