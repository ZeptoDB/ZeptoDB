"""OpenTelemetry GenAI span mapping for AgentOps tables.

The helpers accept OTLP JSON-style span dictionaries and emit SQL INSERT
statements for the AgentOps schema in :mod:`agentops_schema`. They intentionally
avoid an OpenTelemetry SDK dependency so examples and tests stay lightweight.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

from .agentops_schema import insert_sql


def span_attributes(span: Mapping[str, Any]) -> dict[str, Any]:
    """Return a flat attribute dict from an OTLP JSON span."""
    raw = span.get("attributes", {})
    if isinstance(raw, Mapping):
        return {str(key): _unwrap_otel_value(value) for key, value in raw.items()}
    if isinstance(raw, Sequence) and not isinstance(raw, (str, bytes, bytearray)):
        out: dict[str, Any] = {}
        for item in raw:
            if not isinstance(item, Mapping):
                continue
            key = item.get("key")
            if key is None:
                continue
            out[str(key)] = _unwrap_otel_value(item.get("value"))
        return out
    return {}


def map_span_to_agentops_sql(
    span: Mapping[str, Any],
    *,
    default_tenant_id: str = "default",
) -> list[str]:
    """Map one OTLP GenAI/tool/cache span to AgentOps INSERT statements."""
    attrs = span_attributes(span)
    run_id = str(_first(attrs, "agent.run_id", "gen_ai.conversation.id",
                        "session.id") or span.get("traceId") or "run_unknown")
    tenant_id = str(_first(attrs, "tenant.id", "enduser.id") or default_tenant_id)
    timestamp_ns = _int_value(span.get("startTimeUnixNano") or
                              _first(attrs, "timestamp_ns")) or 0
    latency_ms = _latency_ms(span, attrs)
    status = _span_status(span, attrs)
    rows: list[str] = []

    if _is_llm_span(span, attrs):
        call_id = str(_first(attrs, "gen_ai.request.id", "llm.request.id") or
                      span.get("spanId") or "llm_unknown")
        provider = str(_first(attrs, "gen_ai.system", "llm.provider") or "unknown")
        model = str(_first(attrs, "gen_ai.request.model",
                           "gen_ai.response.model", "llm.model_name") or "unknown")
        cache_hit = _bool_value(_first(attrs, "gen_ai.cache.hit", "llm.cache_hit",
                                      "cache.hit"))
        rows.append(insert_sql(
            "llm_calls",
            [
                "call_id",
                "run_id",
                "tenant_id",
                "provider",
                "model",
                "prompt_tokens",
                "completion_tokens",
                "latency_ms",
                "cache_hit",
                "timestamp_ns",
            ],
            [
                call_id,
                run_id,
                tenant_id,
                provider,
                model,
                _int_value(_first(attrs, "gen_ai.usage.input_tokens",
                                  "gen_ai.usage.prompt_tokens",
                                  "llm.usage.prompt_tokens")),
                _int_value(_first(attrs, "gen_ai.usage.output_tokens",
                                  "gen_ai.usage.completion_tokens",
                                  "llm.usage.completion_tokens")),
                latency_ms,
                cache_hit,
                timestamp_ns,
            ],
        ))
        error_type = _error_type(span, attrs)
        if error_type:
            rows.append(insert_sql(
                "llm_errors",
                [
                    "event_id",
                    "call_id",
                    "run_id",
                    "tenant_id",
                    "provider",
                    "model",
                    "error_type",
                    "status",
                    "latency_ms",
                    "timestamp_ns",
                ],
                [
                    "err_" + call_id,
                    call_id,
                    run_id,
                    tenant_id,
                    provider,
                    model,
                    error_type,
                    status,
                    latency_ms,
                    timestamp_ns,
                ],
            ))

    cache_value = _first(attrs, "gen_ai.cache.hit", "llm.cache_hit", "cache.hit")
    if cache_value is not None:
        cache_hit = _bool_value(cache_value)
        cache_kind = str(_first(attrs, "gen_ai.cache.type", "cache.kind") or
                         ("hit" if cache_hit else "miss"))
        event_id = str(_first(attrs, "cache.event_id") or span.get("spanId") or
                       "cache_unknown")
        rows.append(insert_sql(
            "cache_events",
            ["event_id", "run_id", "tenant_id", "cache_hit", "cache_kind",
             "latency_ms", "timestamp_ns"],
            [event_id, run_id, tenant_id, cache_hit, cache_kind, latency_ms,
             timestamp_ns],
        ))

    if _is_tool_span(span, attrs):
        call_id = str(_first(attrs, "gen_ai.tool.call.id", "tool.call.id") or
                      span.get("spanId") or "tool_unknown")
        tool_name = str(_first(attrs, "gen_ai.tool.name", "tool.name") or
                        span.get("name") or "unknown")
        rows.append(insert_sql(
            "tool_calls",
            ["call_id", "run_id", "tenant_id", "tool_name", "status",
             "latency_ms", "timestamp_ns"],
            [call_id, run_id, tenant_id, tool_name, status, latency_ms,
             timestamp_ns],
        ))

    return rows


def map_spans_to_agentops_sql(
    spans: Sequence[Mapping[str, Any]],
    *,
    default_tenant_id: str = "default",
) -> list[str]:
    """Map multiple OTLP spans to AgentOps INSERT statements."""
    rows: list[str] = []
    for span in spans:
        rows.extend(map_span_to_agentops_sql(span,
                                             default_tenant_id=default_tenant_id))
    return rows


def _unwrap_otel_value(value: Any) -> Any:
    if not isinstance(value, Mapping):
        return value
    for key in ("stringValue", "intValue", "doubleValue", "boolValue"):
        if key in value:
            return value[key]
    if "arrayValue" in value:
        values = value.get("arrayValue", {}).get("values", [])
        return [_unwrap_otel_value(item) for item in values]
    return value


def _first(attrs: Mapping[str, Any], *keys: str) -> Any:
    for key in keys:
        if key in attrs and attrs[key] is not None:
            return attrs[key]
    return None


def _int_value(value: Any) -> int:
    if value is None or value == "":
        return 0
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def _bool_value(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.lower() in {"1", "true", "yes", "hit"}
    return False


def _latency_ms(span: Mapping[str, Any], attrs: Mapping[str, Any]) -> int:
    start = _int_value(span.get("startTimeUnixNano") or _first(attrs, "start_ns"))
    end = _int_value(span.get("endTimeUnixNano") or _first(attrs, "end_ns"))
    if start > 0 and end >= start:
        return (end - start) // 1_000_000
    return _int_value(_first(attrs, "latency_ms", "duration_ms"))


def _span_status(span: Mapping[str, Any], attrs: Mapping[str, Any]) -> str:
    if _error_type(span, attrs):
        return "error"
    explicit = _first(attrs, "agent.status", "tool.status", "status")
    if explicit:
        return str(explicit).lower()
    status = span.get("status")
    if isinstance(status, Mapping):
        code = str(status.get("code", "")).upper()
        if code in {"2", "STATUS_CODE_ERROR", "ERROR"}:
            return "error"
    return "ok"


def _error_type(span: Mapping[str, Any], attrs: Mapping[str, Any]) -> str:
    error = _first(attrs, "error.type", "exception.type", "gen_ai.error.type")
    if error:
        return str(error)
    status = span.get("status")
    if isinstance(status, Mapping):
        code = str(status.get("code", "")).upper()
        if code in {"2", "STATUS_CODE_ERROR", "ERROR"}:
            return str(status.get("message") or "error")
    return ""


def _is_llm_span(span: Mapping[str, Any], attrs: Mapping[str, Any]) -> bool:
    if _first(attrs, "gen_ai.system", "gen_ai.request.model",
              "gen_ai.response.model", "llm.provider", "llm.model_name"):
        return True
    return str(span.get("name", "")).lower().startswith("gen_ai")


def _is_tool_span(span: Mapping[str, Any], attrs: Mapping[str, Any]) -> bool:
    return _first(attrs, "gen_ai.tool.name", "tool.name",
                  "gen_ai.tool.call.id", "tool.call.id") is not None
