"""Physical AI Agent Memory demo with concrete time-series rows.

The scenarios in this file pair realistic Physical AI telemetry with Agent
Memory records. ZeptoDB stores the operational timeline; Agent Memory stores
prior decisions, failure episodes, and operator knowledge that an attached
agent can retrieve before making the next decision.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Callable, Dict, Iterable, List, Optional, Protocol, Sequence

from zepto_py import ZeptoConnection

try:
    from .agentops_schema import install_agentops_schema, insert_sql
    from .context_trace import build_context_replay_sql, build_context_trace_sql
    from .provider_cache import deterministic_embedding
except ImportError:
    from agentops_schema import install_agentops_schema, insert_sql
    from context_trace import build_context_replay_sql, build_context_trace_sql
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


class PhysicalAiDb(Protocol):
    memory: MemoryClient

    def execute(self, sql: str) -> int:
        ...

    def query(self, sql: str) -> QueryResult:
        ...


@dataclass(frozen=True)
class PhysicalAiTable:
    name: str
    ddl: str
    rows: Sequence[Dict[str, object]]


@dataclass(frozen=True)
class MemorySeed:
    content: str
    type: str
    importance: float
    metadata: Dict[str, object]
    pinned: bool = False


@dataclass(frozen=True)
class PhysicalAiScenario:
    name: str
    domain: str
    namespace: str
    agent_id: str
    user_id: str
    session_id: str
    tables: Sequence[PhysicalAiTable]
    query_sql: str
    question: str
    memory_seeds: Sequence[MemorySeed]


@dataclass(frozen=True)
class PhysicalAiRunResult:
    scenario: str
    domain: str
    inserted_rows: int
    memory_ids: List[str]
    query_sql: str
    query_row_count: Optional[int]
    context_count: int
    context_tokens: int
    trace_rows: int
    replay_rows: int
    prompt: str


PHYSICAL_AI_SCENARIOS: Sequence[PhysicalAiScenario] = (
    PhysicalAiScenario(
        name="warehouse_agv_pallet_timeline",
        domain="physical_ai_logistics",
        namespace="warehouse_ops",
        agent_id="fleet_route_agent",
        user_id="ops_lead",
        session_id="shift_2026_06_13",
        tables=(
            PhysicalAiTable(
                name="agv_pose",
                ddl="""
CREATE TABLE IF NOT EXISTS agv_pose (
    agv_id SYMBOL,
    timestamp INT64,
    lat FLOAT64,
    lon FLOAT64,
    heading_deg INT64,
    battery_pct INT64,
    wheel_slip_ppm INT64,
    route_segment SYMBOL
)
""".strip(),
                rows=(
                    {
                        "agv_id": "agv_17",
                        "timestamp": 0,
                        "lat": 37.774900,
                        "lon": -122.419400,
                        "heading_deg": 88,
                        "battery_pct": 86,
                        "wheel_slip_ppm": 900,
                        "route_segment": "dock_a",
                    },
                    {
                        "agv_id": "agv_17",
                        "timestamp": 1_000_000_000,
                        "lat": 37.774940,
                        "lon": -122.419360,
                        "heading_deg": 91,
                        "battery_pct": 85,
                        "wheel_slip_ppm": 7200,
                        "route_segment": "dock_a",
                    },
                    {
                        "agv_id": "agv_17",
                        "timestamp": 2_000_000_000,
                        "lat": 37.775010,
                        "lon": -122.419250,
                        "heading_deg": 93,
                        "battery_pct": 84,
                        "wheel_slip_ppm": 1800,
                        "route_segment": "dock_b",
                    },
                ),
            ),
            PhysicalAiTable(
                name="pallet_events",
                ddl="""
CREATE TABLE IF NOT EXISTS pallet_events (
    pallet_id SYMBOL,
    order_id SYMBOL,
    agv_id SYMBOL,
    timestamp INT64,
    zone SYMBOL,
    state SYMBOL
)
""".strip(),
                rows=(
                    {
                        "pallet_id": "pallet_10042",
                        "order_id": "order_7301",
                        "agv_id": "agv_17",
                        "timestamp": 1_500_000_000,
                        "zone": "cold_dock",
                        "state": "picked",
                    },
                    {
                        "pallet_id": "pallet_10042",
                        "order_id": "order_7301",
                        "agv_id": "agv_17",
                        "timestamp": 2_500_000_000,
                        "zone": "sorter_feed",
                        "state": "handoff",
                    },
                ),
            ),
        ),
        query_sql=(
            "SELECT p.timestamp, p.state, p.zone, a.lat, a.lon, a.wheel_slip_ppm "
            "FROM pallet_events p ASOF JOIN agv_pose a "
            "ON p.agv_id = a.agv_id AND p.timestamp >= a.timestamp "
            "WHERE p.pallet_id = 'pallet_10042' ORDER BY p.timestamp ASC"
        ),
        question=(
            "AGV 17 slipped on dock_a while carrying pallet_10042. Should the "
            "fleet agent keep routing cold-chain pallets through dock_a?"
        ),
        memory_seeds=(
            MemorySeed(
                content=(
                    "Dock A slip spikes after floor cleaning; route cold-chain "
                    "pallets through dock_b when wheel_slip_ppm exceeds 5000."
                ),
                type="route_decision",
                importance=0.93,
                metadata={"source": "fleet_review", "agv_id": "agv_17"},
                pinned=True,
            ),
            MemorySeed(
                content=(
                    "Pallet handoff delays at sorter_feed are acceptable if the "
                    "AGV remains above 80 percent battery and avoids dock_a."
                ),
                type="operator_policy",
                importance=0.72,
                metadata={"source": "shift_playbook", "pallet_id": "pallet_10042"},
            ),
        ),
    ),
    PhysicalAiScenario(
        name="robot_lidar_obstacle_replay",
        domain="physical_ai_robotics",
        namespace="robot_replay",
        agent_id="navigation_replay_agent",
        user_id="robotics_eng",
        session_id="rosbag2_replay_9",
        tables=(
            PhysicalAiTable(
                name="ros_odometry",
                ddl="""
CREATE TABLE IF NOT EXISTS ros_odometry (
    symbol INT64,
    timestamp INT64,
    robot_id SYMBOL,
    pose_position_x FLOAT64,
    pose_position_y FLOAT64,
    twist_linear_x FLOAT64
)
""".strip(),
                rows=(
                    {
                        "symbol": 900,
                        "timestamp": 1_710_000_000_000_000_000,
                        "robot_id": "bot_42",
                        "pose_position_x": 12.10,
                        "pose_position_y": 4.20,
                        "twist_linear_x": 0.85,
                    },
                    {
                        "symbol": 900,
                        "timestamp": 1_710_000_001_000_000_000,
                        "robot_id": "bot_42",
                        "pose_position_x": 12.92,
                        "pose_position_y": 4.35,
                        "twist_linear_x": 0.45,
                    },
                ),
            ),
            PhysicalAiTable(
                name="ros_laserscan",
                ddl="""
CREATE TABLE IF NOT EXISTS ros_laserscan (
    symbol INT64,
    timestamp INT64,
    robot_id SYMBOL,
    ranges_min FLOAT64,
    ranges_mean FLOAT64,
    ranges_max FLOAT64,
    intensities_mean FLOAT64
)
""".strip(),
                rows=(
                    {
                        "symbol": 900,
                        "timestamp": 1_710_000_000_900_000_000,
                        "robot_id": "bot_42",
                        "ranges_min": 0.42,
                        "ranges_mean": 2.8,
                        "ranges_max": 8.4,
                        "intensities_mean": 120.0,
                    },
                    {
                        "symbol": 900,
                        "timestamp": 1_710_000_001_900_000_000,
                        "robot_id": "bot_42",
                        "ranges_min": 0.31,
                        "ranges_mean": 2.1,
                        "ranges_max": 7.9,
                        "intensities_mean": 134.0,
                    },
                ),
            ),
        ),
        query_sql=(
            "SELECT o.timestamp, o.pose_position_x, o.pose_position_y, "
            "l.ranges_min, l.ranges_mean FROM ros_odometry o ASOF JOIN "
            "ros_laserscan l ON o.symbol = l.symbol AND o.timestamp >= l.timestamp "
            "WHERE o.symbol = 900 ORDER BY o.timestamp ASC"
        ),
        question=(
            "Bot 42 saw sub-0.5m LaserScan returns during replay. Should the "
            "navigation agent flag a perception issue or a real obstacle?"
        ),
        memory_seeds=(
            MemorySeed(
                content=(
                    "Aisle 12 reflective wrap previously caused false near-range "
                    "LaserScan returns; confirm intensity_mean before labeling a "
                    "real obstacle."
                ),
                type="sensor_failure_episode",
                importance=0.87,
                metadata={"source": "rosbag_review", "robot_id": "bot_42"},
                pinned=True,
            ),
            MemorySeed(
                content=(
                    "If odometry slows while ranges_min stays below 0.5m for two "
                    "frames, route the replay to human review before policy tuning."
                ),
                type="replay_policy",
                importance=0.78,
                metadata={"source": "navigation_sop", "robot_id": "bot_42"},
            ),
        ),
    ),
    PhysicalAiScenario(
        name="cold_chain_shipment_exception",
        domain="physical_ai_cold_chain",
        namespace="cold_chain_ops",
        agent_id="shipment_audit_agent",
        user_id="quality_ops",
        session_id="shipment_8807_audit",
        tables=(
            PhysicalAiTable(
                name="cold_chain_events",
                ddl="""
CREATE TABLE IF NOT EXISTS cold_chain_events (
    shipment_id SYMBOL,
    timestamp INT64,
    temperature_c FLOAT64,
    humidity_pct FLOAT64,
    door_open INT64,
    zone SYMBOL,
    quality SYMBOL
)
""".strip(),
                rows=(
                    {
                        "shipment_id": "ship_8807",
                        "timestamp": 50_000_000_000,
                        "temperature_c": 3.8,
                        "humidity_pct": 71.0,
                        "door_open": 0,
                        "zone": "reefer_2",
                        "quality": "ok",
                    },
                    {
                        "shipment_id": "ship_8807",
                        "timestamp": 51_000_000_000,
                        "temperature_c": 8.6,
                        "humidity_pct": 84.0,
                        "door_open": 1,
                        "zone": "crossdock_gate_4",
                        "quality": "excursion",
                    },
                    {
                        "shipment_id": "ship_8807",
                        "timestamp": 52_000_000_000,
                        "temperature_c": 5.4,
                        "humidity_pct": 78.0,
                        "door_open": 0,
                        "zone": "reefer_2",
                        "quality": "recovering",
                    },
                ),
            ),
        ),
        query_sql=(
            "SELECT shipment_id, MAX(temperature_c) AS max_temp, "
            "SUM(door_open) AS door_open_events FROM cold_chain_events "
            "WHERE shipment_id = 'ship_8807' GROUP BY shipment_id"
        ),
        question=(
            "Shipment ship_8807 had an 8.6C excursion at crossdock_gate_4. "
            "Should the audit agent quarantine the pallet or accept recovery?"
        ),
        memory_seeds=(
            MemorySeed(
                content=(
                    "For vaccines, any temperature above 8C during a door-open "
                    "event requires quarantine unless a calibrated probe confirms "
                    "under 10 minutes of exposure."
                ),
                type="quality_policy",
                importance=0.95,
                metadata={"source": "cold_chain_sop", "shipment_class": "vaccine"},
                pinned=True,
            ),
            MemorySeed(
                content=(
                    "Gate 4 excursions are often caused by staging delay; inspect "
                    "WMS handoff timestamps before blaming reefer failure."
                ),
                type="root_cause_hint",
                importance=0.68,
                metadata={"source": "quality_review", "gate": "crossdock_gate_4"},
            ),
        ),
    ),
)


def run_physical_ai_scenario(
    db: PhysicalAiDb,
    scenario: PhysicalAiScenario,
    tenant_id: str = "physical_ai_demo",
    token_budget: int = 512,
    context_limit: int = 6,
    execute_query: bool = True,
    record_agentops: bool = True,
    embed: Callable[[str], List[float]] = deterministic_embedding,
) -> PhysicalAiRunResult:
    """Load Physical AI rows, retrieve Agent Memory context, and trace it."""
    if record_agentops:
        install_agentops_schema(db)

    inserted_rows = 0
    for table in scenario.tables:
        db.execute(table.ddl)
        for row in table.rows:
            db.execute(insert_sql(table.name, row.keys(), row.values()))
            inserted_rows += 1

    memory_ids: List[str] = []
    for seed in scenario.memory_seeds:
        metadata = {
            "demo": "physical_ai_agent",
            "scenario": scenario.name,
            "domain": scenario.domain,
            "tables": [table.name for table in scenario.tables],
            **seed.metadata,
        }
        memory_ids.append(
            db.memory.put(
                seed.content,
                embedding=embed(seed.content),
                namespace=scenario.namespace,
                tenant_id=tenant_id,
                user_id=scenario.user_id,
                session_id=scenario.session_id,
                agent_id=scenario.agent_id,
                type=seed.type,
                metadata_json=json.dumps(metadata, sort_keys=True),
                token_count=_estimate_tokens(seed.content),
                importance=seed.importance,
                pinned=seed.pinned,
            )
        )

    context = db.memory.get_context(
        query_embedding=embed(scenario.question),
        token_budget=token_budget,
        namespace=scenario.namespace,
        tenant_id=tenant_id,
        user_id=scenario.user_id,
        session_id=scenario.session_id,
        agent_id=scenario.agent_id,
        limit=context_limit,
    )

    query_row_count: Optional[int] = None
    query_summary = "query skipped"
    if execute_query:
        result = db.query(scenario.query_sql)
        query_row_count = int(getattr(result, "row_count", 0))
        query_summary = _summarize_query_result(result)

    timestamp_ns = _first_timestamp(scenario)
    trace_sql = build_context_trace_sql(
        context,
        run_id=f"{scenario.name}_run",
        tenant_id=tenant_id,
        timestamp_ns=timestamp_ns,
    )
    replay_sql = build_context_replay_sql(
        [
            {
                "source_table": "+".join(table.name for table in scenario.tables),
                "query": scenario.query_sql,
                "row_count": query_row_count or 0,
                "timestamp_ns": timestamp_ns,
            }
        ],
        run_id=f"{scenario.name}_run",
        tenant_id=tenant_id,
        timestamp_ns=timestamp_ns,
    )
    if record_agentops:
        for sql in trace_sql:
            db.execute(sql)
        for sql in replay_sql:
            db.execute(sql)

    prompt = build_physical_ai_prompt(scenario, context, query_summary)
    return PhysicalAiRunResult(
        scenario=scenario.name,
        domain=scenario.domain,
        inserted_rows=inserted_rows,
        memory_ids=memory_ids,
        query_sql=scenario.query_sql,
        query_row_count=query_row_count,
        context_count=len(context.get("memories", [])),
        context_tokens=int(context.get("token_count", 0)),
        trace_rows=len(trace_sql),
        replay_rows=len(replay_sql),
        prompt=prompt,
    )


def run_all_physical_ai_scenarios(
    db: PhysicalAiDb,
    scenarios: Iterable[PhysicalAiScenario] = PHYSICAL_AI_SCENARIOS,
    tenant_id: str = "physical_ai_demo",
    execute_query: bool = True,
    record_agentops: bool = True,
    embed: Callable[[str], List[float]] = deterministic_embedding,
) -> List[PhysicalAiRunResult]:
    """Run every Physical AI scenario against one ZeptoDB connection."""
    return [
        run_physical_ai_scenario(
            db,
            scenario,
            tenant_id=tenant_id,
            execute_query=execute_query,
            record_agentops=record_agentops,
            embed=embed,
        )
        for scenario in scenarios
    ]


def build_physical_ai_prompt(
    scenario: PhysicalAiScenario,
    context: Dict,
    query_summary: str,
) -> str:
    """Build the prompt an attached Physical AI agent would send to a model."""
    memories = []
    for memory in context.get("memories", []):
        if isinstance(memory, dict) and memory.get("content"):
            memories.append(f"- {memory['content']}")
    memory_block = "\n".join(memories) if memories else "(no prior memory)"
    tables = ", ".join(table.name for table in scenario.tables)
    return (
        f"Physical AI domain: {scenario.domain}\n"
        f"Scenario: {scenario.name}\n"
        f"Telemetry tables: {tables}\n"
        f"Current replay/live query:\n{scenario.query_sql}\n\n"
        f"Current time-series result:\n{query_summary}\n\n"
        f"Retrieved operational memory:\n{memory_block}\n\n"
        f"Decision question:\n{scenario.question}"
    )


def _summarize_query_result(result: QueryResult, max_rows: int = 3) -> str:
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


def _first_timestamp(scenario: PhysicalAiScenario) -> int:
    for table in scenario.tables:
        for row in table.rows:
            value = row.get("timestamp")
            if value is not None:
                return int(value)
    return 0


def main() -> None:
    db = ZeptoConnection("localhost", 8123)
    for result in run_all_physical_ai_scenarios(db):
        print(json.dumps(result.__dict__, indent=2))


if __name__ == "__main__":
    main()
