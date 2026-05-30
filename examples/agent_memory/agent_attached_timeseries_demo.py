"""Agent-attached time-series demos.

Each scenario pairs ordinary ZeptoDB time-series tables with Agent Memory
records. The table keeps what happened on the timeline; Agent Memory keeps what
the attached agent learned, decided, or should reuse on the next turn.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Callable, Dict, Iterable, List, Optional, Protocol, Sequence

from zepto_py import ZeptoConnection

try:
    from .agentops_schema import insert_sql
    from .provider_cache import deterministic_embedding
except ImportError:
    from agentops_schema import insert_sql
    from provider_cache import deterministic_embedding


class MemoryClient(Protocol):
    def put(
        self,
        content: str,
        embedding: List[float],
        namespace: str,
        tenant_id: str,
        user_id: str,
        session_id: str,
        agent_id: str,
        type: str = "memory",
        metadata_json: str = "{}",
        token_count: int = 0,
        importance: float = 0.0,
        pinned: bool = False,
    ) -> str:
        ...

    def get_context(
        self,
        query_embedding: List[float],
        token_budget: int,
        namespace: str,
        tenant_id: str,
        user_id: str,
        session_id: str,
        agent_id: str,
        limit: int,
    ) -> Dict:
        ...


class QueryResult(Protocol):
    row_count: int
    columns: List[str]
    rows: List[List[object]]


class AgentAttachedDb(Protocol):
    memory: MemoryClient

    def execute(self, sql: str) -> int:
        ...

    def query(self, sql: str) -> QueryResult:
        ...


@dataclass(frozen=True)
class MemorySeed:
    content: str
    type: str
    importance: float
    metadata: Dict[str, object]
    pinned: bool = False


@dataclass(frozen=True)
class TimeSeriesUseCase:
    name: str
    domain: str
    table_name: str
    ddl: str
    rows: Sequence[Dict[str, object]]
    query_sql: str
    question: str
    namespace: str
    agent_id: str
    user_id: str
    session_id: str
    memory_seeds: Sequence[MemorySeed]


@dataclass(frozen=True)
class TimeSeriesRunResult:
    use_case: str
    domain: str
    table_name: str
    inserted_rows: int
    memory_ids: List[str]
    query_sql: str
    query_row_count: Optional[int]
    context_count: int
    context_tokens: int
    prompt: str


USE_CASES: Sequence[TimeSeriesUseCase] = (
    TimeSeriesUseCase(
        name="finance_hft",
        domain="finance",
        table_name="finance_ticks",
        ddl="""
CREATE TABLE IF NOT EXISTS finance_ticks (
    symbol SYMBOL,
    timestamp INT64,
    bid INT64,
    ask INT64,
    trade_price INT64,
    volume INT64,
    risk_score INT64
)
""".strip(),
        rows=(
            {
                "symbol": "AAPL",
                "timestamp": 0,
                "bid": 18995,
                "ask": 19001,
                "trade_price": 18999,
                "volume": 800,
                "risk_score": 41,
            },
            {
                "symbol": "AAPL",
                "timestamp": 1_000_000_000,
                "bid": 19005,
                "ask": 19020,
                "trade_price": 19017,
                "volume": 1200,
                "risk_score": 73,
            },
            {
                "symbol": "AAPL",
                "timestamp": 2_000_000_000,
                "bid": 19004,
                "ask": 19008,
                "trade_price": 19005,
                "volume": 600,
                "risk_score": 58,
            },
        ),
        query_sql=(
            "SELECT symbol, VWAP(trade_price, volume) AS vwap, "
            "MAX(risk_score) AS max_risk "
            "FROM finance_ticks WHERE symbol = 'AAPL' GROUP BY symbol"
        ),
        question=(
            "AAPL risk_score crossed 70 while spread widened. Should the "
            "execution agent keep routing aggressively?"
        ),
        namespace="finance_hft",
        agent_id="execution_agent",
        user_id="trader_7",
        session_id="open_auction",
        memory_seeds=(
            MemorySeed(
                content=(
                    "AAPL liquidity faded in the last open whenever risk_score "
                    "exceeded 70; throttle child orders until the spread narrows."
                ),
                type="strategy_note",
                importance=0.88,
                metadata={"source": "post_trade_review", "symbol": "AAPL"},
                pinned=True,
            ),
            MemorySeed(
                content=(
                    "Compliance approved lowering participation after a venue "
                    "imbalance alert, but not increasing participation."
                ),
                type="compliance_decision",
                importance=0.79,
                metadata={"source": "compliance_review", "symbol": "AAPL"},
            ),
        ),
    ),
    TimeSeriesUseCase(
        name="iot_smart_factory",
        domain="iot",
        table_name="iot_sensor_readings",
        ddl="""
CREATE TABLE IF NOT EXISTS iot_sensor_readings (
    device_id SYMBOL,
    timestamp INT64,
    temperature_c INT64,
    vibration_um INT64,
    current_ma INT64,
    pressure_kpa INT64,
    state SYMBOL
)
""".strip(),
        rows=(
            {
                "device_id": "pump_7",
                "timestamp": 10_000_000_000,
                "temperature_c": 68,
                "vibration_um": 120,
                "current_ma": 1410,
                "pressure_kpa": 360,
                "state": "running",
            },
            {
                "device_id": "pump_7",
                "timestamp": 11_000_000_000,
                "temperature_c": 75,
                "vibration_um": 255,
                "current_ma": 1590,
                "pressure_kpa": 352,
                "state": "running",
            },
            {
                "device_id": "pump_7",
                "timestamp": 12_000_000_000,
                "temperature_c": 78,
                "vibration_um": 310,
                "current_ma": 1660,
                "pressure_kpa": 346,
                "state": "warning",
            },
        ),
        query_sql=(
            "SELECT device_id, AVG(vibration_um) AS avg_vibration, "
            "MAX(temperature_c) AS max_temp "
            "FROM iot_sensor_readings WHERE device_id = 'pump_7' "
            "GROUP BY device_id"
        ),
        question=(
            "Pump 7 vibration and current are rising. What should the "
            "maintenance agent check before stopping the line?"
        ),
        namespace="iot_factory",
        agent_id="maintenance_agent",
        user_id="operator_3",
        session_id="line_a_shift_2",
        memory_seeds=(
            MemorySeed(
                content=(
                    "Pump 7 showed the same vibration ramp before a bearing "
                    "replacement; inspect the bearing housing before shutdown."
                ),
                type="failure_mode",
                importance=0.91,
                metadata={"source": "cmms_history", "device_id": "pump_7"},
                pinned=True,
            ),
            MemorySeed(
                content=(
                    "Operator note: pressure below 350 kPa plus high current "
                    "usually means clogged intake filter, not motor failure."
                ),
                type="operator_note",
                importance=0.74,
                metadata={"source": "shift_log", "device_id": "pump_7"},
            ),
        ),
    ),
    TimeSeriesUseCase(
        name="observability_apm",
        domain="observability",
        table_name="observability_metrics",
        ddl="""
CREATE TABLE IF NOT EXISTS observability_metrics (
    service SYMBOL,
    timestamp INT64,
    latency_ms INT64,
    error_count INT64,
    slo_burn_ppm INT64,
    deploy_id SYMBOL
)
""".strip(),
        rows=(
            {
                "service": "checkout",
                "timestamp": 20_000_000_000,
                "latency_ms": 180,
                "error_count": 2,
                "slo_burn_ppm": 1200,
                "deploy_id": "deploy_41",
            },
            {
                "service": "checkout",
                "timestamp": 21_000_000_000,
                "latency_ms": 940,
                "error_count": 37,
                "slo_burn_ppm": 8700,
                "deploy_id": "deploy_42",
            },
            {
                "service": "checkout",
                "timestamp": 22_000_000_000,
                "latency_ms": 720,
                "error_count": 22,
                "slo_burn_ppm": 7100,
                "deploy_id": "deploy_42",
            },
        ),
        query_sql=(
            "SELECT service, MAX(latency_ms) AS max_latency, "
            "SUM(error_count) AS errors "
            "FROM observability_metrics WHERE service = 'checkout' "
            "GROUP BY service"
        ),
        question=(
            "Checkout latency spiked after deploy_42. Which previous incident "
            "context should the SRE agent reuse?"
        ),
        namespace="observability_apm",
        agent_id="sre_agent",
        user_id="oncall_1",
        session_id="incident_checkout_42",
        memory_seeds=(
            MemorySeed(
                content=(
                    "The last checkout incident after deploy_39 was caused by "
                    "connection pool exhaustion in the payment client."
                ),
                type="incident_summary",
                importance=0.86,
                metadata={"source": "postmortem", "service": "checkout"},
                pinned=True,
            ),
            MemorySeed(
                content=(
                    "Rollback stopped the error spike, but the durable fix was "
                    "raising payment client pool limits and adding saturation alerts."
                ),
                type="remediation_outcome",
                importance=0.76,
                metadata={"source": "runbook", "service": "checkout"},
            ),
        ),
    ),
    TimeSeriesUseCase(
        name="robotics_fleet",
        domain="robotics",
        table_name="robotics_events",
        ddl="""
CREATE TABLE IF NOT EXISTS robotics_events (
    robot_id SYMBOL,
    timestamp INT64,
    imu_yaw_mdeg INT64,
    wheel_slip_ppm INT64,
    battery_mv INT64,
    route_segment SYMBOL,
    obstacle_count INT64
)
""".strip(),
        rows=(
            {
                "robot_id": "bot_42",
                "timestamp": 30_000_000_000,
                "imu_yaw_mdeg": 140,
                "wheel_slip_ppm": 1200,
                "battery_mv": 50100,
                "route_segment": "dock_a",
                "obstacle_count": 1,
            },
            {
                "robot_id": "bot_42",
                "timestamp": 31_000_000_000,
                "imu_yaw_mdeg": 520,
                "wheel_slip_ppm": 8200,
                "battery_mv": 49920,
                "route_segment": "dock_a",
                "obstacle_count": 4,
            },
            {
                "robot_id": "bot_42",
                "timestamp": 32_000_000_000,
                "imu_yaw_mdeg": 210,
                "wheel_slip_ppm": 1800,
                "battery_mv": 49890,
                "route_segment": "dock_b",
                "obstacle_count": 1,
            },
        ),
        query_sql=(
            "SELECT robot_id, MAX(wheel_slip_ppm) AS max_slip, "
            "SUM(obstacle_count) AS obstacles "
            "FROM robotics_events WHERE robot_id = 'bot_42' GROUP BY robot_id"
        ),
        question=(
            "Bot 42 slipped on dock_a and saw repeated obstacles. Should the "
            "route planner keep dock_a active?"
        ),
        namespace="robotics_fleet",
        agent_id="route_agent",
        user_id="fleet_ops",
        session_id="warehouse_replay_42",
        memory_seeds=(
            MemorySeed(
                content=(
                    "Dock A becomes unreliable after floor cleaning; route Bot 42 "
                    "through dock_b when wheel_slip_ppm exceeds 5000."
                ),
                type="route_decision",
                importance=0.89,
                metadata={"source": "operator_feedback", "robot_id": "bot_42"},
                pinned=True,
            ),
            MemorySeed(
                content=(
                    "A prior replay showed obstacle_count spikes at dock_a were "
                    "temporary pallets, not perception sensor drift."
                ),
                type="failure_episode",
                importance=0.69,
                metadata={"source": "route_replay", "robot_id": "bot_42"},
            ),
        ),
    ),
    TimeSeriesUseCase(
        name="game_liveops",
        domain="game_liveops",
        table_name="game_liveops_events",
        ddl="""
CREATE TABLE IF NOT EXISTS game_liveops_events (
    cohort SYMBOL,
    timestamp INT64,
    match_count INT64,
    churn_risk_ppm INT64,
    currency_sink INT64,
    conversion_ppm INT64,
    experiment SYMBOL
)
""".strip(),
        rows=(
            {
                "cohort": "new_players",
                "timestamp": 40_000_000_000,
                "match_count": 4500,
                "churn_risk_ppm": 2400,
                "currency_sink": 81000,
                "conversion_ppm": 3200,
                "experiment": "starter_pack_a",
            },
            {
                "cohort": "new_players",
                "timestamp": 41_000_000_000,
                "match_count": 3900,
                "churn_risk_ppm": 5100,
                "currency_sink": 57000,
                "conversion_ppm": 2800,
                "experiment": "starter_pack_b",
            },
            {
                "cohort": "new_players",
                "timestamp": 42_000_000_000,
                "match_count": 4100,
                "churn_risk_ppm": 4300,
                "currency_sink": 65000,
                "conversion_ppm": 3050,
                "experiment": "starter_pack_b",
            },
        ),
        query_sql=(
            "SELECT cohort, SUM(match_count) AS matches, "
            "AVG(churn_risk_ppm) AS churn_risk "
            "FROM game_liveops_events WHERE cohort = 'new_players' "
            "GROUP BY cohort"
        ),
        question=(
            "New player churn risk increased during starter_pack_b. What should "
            "the live-ops agent avoid repeating?"
        ),
        namespace="game_liveops",
        agent_id="liveops_agent",
        user_id="pm_liveops",
        session_id="starter_pack_review",
        memory_seeds=(
            MemorySeed(
                content=(
                    "The last starter-pack discount improved conversion but "
                    "reduced currency_sink enough to inflate the early economy."
                ),
                type="experiment_interpretation",
                importance=0.83,
                metadata={"source": "experiment_review", "cohort": "new_players"},
                pinned=True,
            ),
            MemorySeed(
                content=(
                    "Approved action: pair new-player discounts with a cosmetic "
                    "sink instead of increasing progression currency rewards."
                ),
                type="approved_action",
                importance=0.77,
                metadata={"source": "economy_review", "cohort": "new_players"},
            ),
        ),
    ),
)


def run_use_case(
    db: AgentAttachedDb,
    use_case: TimeSeriesUseCase,
    tenant_id: str = "demo",
    token_budget: int = 384,
    context_limit: int = 6,
    execute_query: bool = True,
    embed: Callable[[str], List[float]] = deterministic_embedding,
) -> TimeSeriesRunResult:
    """Install one scenario, seed time-series rows and memory, then retrieve context."""
    db.execute(use_case.ddl)

    inserted_rows = 0
    for row in use_case.rows:
        db.execute(insert_sql(use_case.table_name, row.keys(), row.values()))
        inserted_rows += 1

    memory_ids: List[str] = []
    for seed in use_case.memory_seeds:
        metadata = {
            "demo": "agent_attached_timeseries",
            "use_case": use_case.name,
            "domain": use_case.domain,
            "table": use_case.table_name,
            **seed.metadata,
        }
        memory_ids.append(
            db.memory.put(
                seed.content,
                embedding=embed(seed.content),
                namespace=use_case.namespace,
                tenant_id=tenant_id,
                user_id=use_case.user_id,
                session_id=use_case.session_id,
                agent_id=use_case.agent_id,
                type=seed.type,
                metadata_json=json.dumps(metadata, sort_keys=True),
                token_count=_estimate_tokens(seed.content),
                importance=seed.importance,
                pinned=seed.pinned,
            )
        )

    context = db.memory.get_context(
        query_embedding=embed(use_case.question),
        token_budget=token_budget,
        namespace=use_case.namespace,
        tenant_id=tenant_id,
        user_id=use_case.user_id,
        session_id=use_case.session_id,
        agent_id=use_case.agent_id,
        limit=context_limit,
    )

    query_row_count: Optional[int] = None
    query_summary = "query skipped"
    if execute_query:
        result = db.query(use_case.query_sql)
        query_row_count = int(getattr(result, "row_count", 0))
        query_summary = summarize_query_result(result)

    prompt = build_agent_prompt(use_case, context, query_summary)
    return TimeSeriesRunResult(
        use_case=use_case.name,
        domain=use_case.domain,
        table_name=use_case.table_name,
        inserted_rows=inserted_rows,
        memory_ids=memory_ids,
        query_sql=use_case.query_sql,
        query_row_count=query_row_count,
        context_count=len(context.get("memories", [])),
        context_tokens=int(context.get("token_count", 0)),
        prompt=prompt,
    )


def run_all_use_cases(
    db: AgentAttachedDb,
    use_cases: Iterable[TimeSeriesUseCase] = USE_CASES,
    tenant_id: str = "demo",
    execute_query: bool = True,
    embed: Callable[[str], List[float]] = deterministic_embedding,
) -> List[TimeSeriesRunResult]:
    """Run every vertical demo against one ZeptoDB connection."""
    return [
        run_use_case(
            db,
            use_case,
            tenant_id=tenant_id,
            execute_query=execute_query,
            embed=embed,
        )
        for use_case in use_cases
    ]


def build_agent_prompt(
    use_case: TimeSeriesUseCase,
    context: Dict,
    query_summary: str,
) -> str:
    """Build the prompt an attached agent would send to a provider."""
    memories = []
    for memory in context.get("memories", []):
        if isinstance(memory, dict) and memory.get("content"):
            memories.append(f"- {memory['content']}")
    memory_block = "\n".join(memories) if memories else "(no prior memory)"
    return (
        f"Domain: {use_case.domain}\n"
        f"Current time-series query:\n{use_case.query_sql}\n\n"
        f"Current time-series result:\n{query_summary}\n\n"
        f"Retrieved agent memory:\n{memory_block}\n\n"
        f"Decision question:\n{use_case.question}"
    )


def summarize_query_result(result: QueryResult, max_rows: int = 3) -> str:
    """Render a compact query result preview for an agent prompt."""
    columns = list(getattr(result, "columns", []))
    rows = list(getattr(result, "rows", []))
    row_count = int(getattr(result, "row_count", len(rows)))
    if not rows:
        return f"0 rows returned for columns {columns}"

    rendered_rows = []
    for row in rows[:max_rows]:
        pairs = [
            f"{columns[i]}={row[i]}"
            for i in range(min(len(columns), len(row)))
        ]
        rendered_rows.append("{" + ", ".join(pairs) + "}")
    suffix = "" if row_count <= max_rows else f" ... {row_count - max_rows} more"
    return f"{row_count} row(s): " + "; ".join(rendered_rows) + suffix


def _estimate_tokens(text: str) -> int:
    return max(1, (len(text) + 3) // 4)


def main() -> None:
    db = ZeptoConnection("localhost", 8123)
    for result in run_all_use_cases(db):
        print(json.dumps(result.__dict__, indent=2))


if __name__ == "__main__":
    main()
