"""AgentOps telemetry schema helpers for ZeptoDB.

Agent Memory stores durable context objects; these tables store the time-series
operational trace around agent work. Keeping both in ZeptoDB lets operators ask
"what happened?" and "what did the agent remember?" in the same system.
"""

from __future__ import annotations

from typing import Iterable, Protocol


class SqlExecutor(Protocol):
    def execute(self, sql: str) -> int:
        ...


AGENTOPS_DDL = {
    "agent_runs": """
CREATE TABLE IF NOT EXISTS agent_runs (
    run_id SYMBOL,
    tenant_id SYMBOL,
    user_id SYMBOL,
    session_id SYMBOL,
    agent_id SYMBOL,
    started_at_ns INT64,
    ended_at_ns INT64,
    status SYMBOL
)
""".strip(),
    "retrieval_events": """
CREATE TABLE IF NOT EXISTS retrieval_events (
    event_id SYMBOL,
    run_id SYMBOL,
    tenant_id SYMBOL,
    memory_count INT64,
    token_count INT64,
    latency_ms INT64,
    timestamp_ns INT64
)
""".strip(),
    "cache_events": """
CREATE TABLE IF NOT EXISTS cache_events (
    event_id SYMBOL,
    run_id SYMBOL,
    tenant_id SYMBOL,
    cache_hit INT64,
    cache_kind SYMBOL,
    latency_ms INT64,
    timestamp_ns INT64
)
""".strip(),
    "llm_calls": """
CREATE TABLE IF NOT EXISTS llm_calls (
    call_id SYMBOL,
    run_id SYMBOL,
    tenant_id SYMBOL,
    provider SYMBOL,
    model SYMBOL,
    prompt_tokens INT64,
    completion_tokens INT64,
    latency_ms INT64,
    cache_hit INT64,
    timestamp_ns INT64
)
""".strip(),
    "tool_calls": """
CREATE TABLE IF NOT EXISTS tool_calls (
    call_id SYMBOL,
    run_id SYMBOL,
    tenant_id SYMBOL,
    tool_name SYMBOL,
    status SYMBOL,
    latency_ms INT64,
    timestamp_ns INT64
)
""".strip(),
}


def install_agentops_schema(db: SqlExecutor, tables: Iterable[str] = AGENTOPS_DDL) -> None:
    """Create AgentOps telemetry tables if they do not exist."""
    for table in tables:
        db.execute(AGENTOPS_DDL[table])


def sql_quote(value: str) -> str:
    """Quote a SQL string literal for example INSERT statements."""
    return "'" + value.replace("'", "''") + "'"


def insert_sql(table: str, columns: Iterable[str], values: Iterable[object]) -> str:
    """Build a small INSERT statement for controlled example schemas."""
    rendered = []
    for value in values:
        if isinstance(value, str):
            rendered.append(sql_quote(value))
        elif isinstance(value, bool):
            rendered.append("1" if value else "0")
        else:
            rendered.append(str(value))
    return (
        f"INSERT INTO {table} ({', '.join(columns)}) VALUES "
        f"({', '.join(rendered)})"
    )
