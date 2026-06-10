"""Context trace and replay helpers for Agent Memory examples."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

from .agentops_schema import insert_sql


def build_context_trace_sql(
    context: Mapping[str, Any],
    *,
    run_id: str,
    tenant_id: str,
    timestamp_ns: int,
) -> list[str]:
    """Build INSERTs explaining why each selected memory entered a prompt."""
    rows: list[str] = []
    memories = context.get("memories", [])
    if not isinstance(memories, Sequence) or isinstance(memories, (str, bytes, bytearray)):
        return rows
    for rank, match in enumerate(memories, start=1):
        if not isinstance(match, Mapping):
            continue
        record = _record_from_match(match)
        memory_id = str(record.get("memory_id") or match.get("memory_id") or "")
        score = _float_value(match.get("score"))
        similarity = _float_value(match.get("similarity"))
        token_count = _int_value(record.get("token_count") or match.get("token_count"))
        reason = explain_memory_selection(match)
        trace_id = f"{run_id}_mem_{rank}"
        rows.append(insert_sql(
            "context_traces",
            [
                "trace_id",
                "run_id",
                "tenant_id",
                "memory_id",
                "rank",
                "score_micros",
                "similarity_micros",
                "token_count",
                "reason",
                "timestamp_ns",
            ],
            [
                trace_id,
                run_id,
                tenant_id,
                memory_id,
                rank,
                int(score * 1_000_000),
                int(similarity * 1_000_000),
                token_count,
                reason,
                timestamp_ns,
            ],
        ))
    return rows


def build_context_replay_sql(
    snapshots: Sequence[Mapping[str, Any]],
    *,
    run_id: str,
    tenant_id: str,
    timestamp_ns: int,
) -> list[str]:
    """Build INSERTs for time-series snapshots used around a context decision."""
    rows: list[str] = []
    for idx, snapshot in enumerate(snapshots, start=1):
        source_table = str(snapshot.get("source_table") or snapshot.get("table") or "")
        query = str(snapshot.get("query") or "")
        row_count = _int_value(snapshot.get("row_count"))
        event_ts = _int_value(snapshot.get("timestamp_ns")) or timestamp_ns
        rows.append(insert_sql(
            "context_replay_events",
            ["event_id", "run_id", "tenant_id", "source_table", "query",
             "row_count", "timestamp_ns"],
            [f"{run_id}_replay_{idx}", run_id, tenant_id, source_table, query,
             row_count, event_ts],
        ))
    return rows


def explain_memory_selection(match: Mapping[str, Any]) -> str:
    """Return a compact reason string for a selected memory match."""
    record = _record_from_match(match)
    reasons: list[str] = []
    if bool(record.get("pinned")):
        reasons.append("pinned")
    if _float_value(match.get("similarity")) > 0:
        reasons.append("semantic_match")
    if _float_value(record.get("importance")) > 0:
        reasons.append("importance")
    if _int_value(record.get("access_count")) > 0:
        reasons.append("prior_use")
    if _int_value(record.get("token_count") or match.get("token_count")) > 0:
        reasons.append("token_budget_fit")
    if not reasons:
        reasons.append("ranked_context")
    return "+".join(reasons)


def _record_from_match(match: Mapping[str, Any]) -> Mapping[str, Any]:
    for key in ("record", "memory"):
        value = match.get(key)
        if isinstance(value, Mapping):
            return value
    return match


def _int_value(value: Any) -> int:
    if value is None or value == "":
        return 0
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def _float_value(value: Any) -> float:
    if value is None or value == "":
        return 0.0
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0
