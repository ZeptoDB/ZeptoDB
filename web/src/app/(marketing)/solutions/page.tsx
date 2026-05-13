"use client";
import {
  Box,
  Typography,
  Card,
  Table,
  TableBody,
  TableCell,
  TableRow,
  Chip,
} from "@mui/material";
import SmartToyIcon from "@mui/icons-material/SmartToy";
import ShowChartIcon from "@mui/icons-material/ShowChart";
import SportsEsportsIcon from "@mui/icons-material/SportsEsports";
import FactoryIcon from "@mui/icons-material/Factory";
import VisibilityIcon from "@mui/icons-material/Visibility";

interface CapabilityRow {
  need: string;
  feature: string;
}

interface Solution {
  id: string;
  title: string;
  icon: React.ReactNode;
  pain: string;
  capabilities: CapabilityRow[];
  proof: string;
  killer: string;
}

const SOLUTIONS: Solution[] = [
  {
    id: "physical-ai",
    title: "Physical AI",
    icon: <SmartToyIcon color="primary" sx={{ fontSize: 40 }} aria-hidden />,
    pain:
      "Industrial sensors, robots, and fab equipment generate kHz-to-MHz time-series streams. Traditional historians and InfluxDB saturate above 10 kHz; ROS bag is file-based replay only; Redis has no persistence or aggregation.",
    capabilities: [
      { need: "SLA-grade OPC-UA ingest", feature: "Real open62541 UA_Client, Basic256Sha256, reconnect, quality mapping (devlogs 105–110)" },
      { need: "Multi-sensor fusion", feature: "ASOF JOIN and Window JOIN — align LiDAR + camera + IMU + CAN on a common time axis" },
      { need: "Robotics replay & simulation", feature: "Parquet HDB + Isaac Sim / ROS2 bag replay hook (ROS2 plugin on the P9 backlog)" },
      { need: "Sector profiles", feature: "Fab / Auto / Steel / Generic defaults for Samsung, SK hynix, TSMC, POSCO-class deployments (devlog 105)" },
      { need: "Python ML pipeline", feature: "522 ns zero-copy NumPy view — sensor → feature → PyTorch with no IPC" },
    ],
    proof:
      "OPC-UA connector is SLA-grade as of devlog 110. Sector defaults ship for Fab/Auto/Steel/Generic. Microbench parity: OPC-UA 6.69 M ticks/s cheap-path baseline.",
    killer: "HFT-grade ingestion for your industrial sensors.",
  },
  {
    id: "finance",
    title: "Finance / HFT",
    icon: <ShowChartIcon color="primary" sx={{ fontSize: 40 }} aria-hidden />,
    pain:
      "kdb+ costs $100K–500K per core per year and forces the q language. ClickHouse can't keep up with tick-rate ingest. TimescaleDB is PostgreSQL-fast — milliseconds, not microseconds.",
    capabilities: [
      { need: "Tick-rate ingestion", feature: "5.52M ticks/sec sustained; FIX / NASDAQ ITCH / Binance native handlers (ITCH parser 250 ns, FIX parser 350 ns)" },
      { need: "Point-in-time lookups", feature: "Native ASOF JOIN, Window JOIN, UNION JOIN, PLUS JOIN" },
      { need: "OHLCV / VWAP bars", feature: "xbar time buckets, VWAP, first/last — as standard SQL, not UDFs" },
      { need: "Backtesting & research", feature: "Same engine from Jupyter notebook to production matching engine. Zero-copy Polars DSL." },
      { need: "Audit / compliance", feature: "Audit log (SOC 2 / EMIR / MiFID II), WAL, RBAC 5 roles, Parquet → S3 retention" },
    ],
    proof:
      "VWAP 1M p50: 532 µs. 1M-row filter p50: 272 µs. kdb+-class performance under standard SQL. clang-19 baselines per devlogs 097 / 098 for reproducibility.",
    killer: "The engine proven on Wall Street.",
  },
  {
    id: "game",
    title: "Game",
    icon: <SportsEsportsIcon color="primary" sx={{ fontSize: 40 }} aria-hidden />,
    pain:
      "Player telemetry is firehose-shaped: millions of events per second across shards. Anti-cheat wants pattern detection at interactive latency. Live-ops wants A/B buckets over the last hour, not yesterday's batch.",
    capabilities: [
      { need: "Real-time player telemetry", feature: "Kafka consumer with backpressure retry; JSON / binary / human-readable decoders" },
      { need: "Anti-cheat pattern detection", feature: "Window functions (EMA / DELTA / RANK) for per-player anomaly scoring" },
      { need: "Live-ops A/B analytics", feature: "xbar time buckets + GROUP BY segment_id for sub-second cohort aggregates" },
      { need: "Server tick-rate monitoring", feature: "272 µs 1M-row filter + Prometheus /metrics for Grafana tick-rate dashboards" },
      { need: "Leaderboards & session replay", feature: "ROW_NUMBER / RANK / DENSE_RANK windows + ASOF JOIN across session events and Parquet HDB" },
    ],
    proof:
      "5.52M events/sec ingestion, 272 µs filter p50, 2.2 ms EMA over 1M rows. One engine for player telemetry, anti-cheat, and live-ops.",
    killer: "Every shot, every frame, every player — queryable in microseconds.",
  },
  {
    id: "iot",
    title: "IoT / Smart Factory",
    icon: <FactoryIcon color="primary" sx={{ fontSize: 40 }} aria-hidden />,
    pain:
      "Semiconductor fabs produce 10 kHz CMP pressure streams across hundreds of tools. InfluxDB / Prometheus collapse above 10 kHz per metric. Anomaly detection runs in a batch job, hours after the yield hit happened.",
    capabilities: [
      { need: "10 kHz sensor ingestion", feature: "5.52M events/sec; MQTT QoS 0/1/2 with topic wildcards (#, +) and OPC-UA in parallel (devlog 081)" },
      { need: "Multi-sensor correlation", feature: "ASOF JOIN across temperature + vibration + current + yield streams" },
      { need: "Predictive maintenance", feature: "EMA / DELTA / RATIO window functions for drift and anomaly scoring in SQL" },
      { need: "TTL + cold storage", feature: "ALTER TABLE SET TTL + automatic HDB flush to Parquet on S3; zero-downtime eviction" },
      { need: "Sector defaults", feature: "Fab / Auto / Steel / Generic profiles match Samsung, SK hynix, TSMC, POSCO-class workloads" },
    ],
    proof:
      "MQTT consumer with full QoS + wildcards ships today. OPC-UA connector is SLA-grade (sector profiles per devlog 105). HDB tiered storage drops cold partitions to S3 without stopping ingest.",
    killer: "Factory sensor data powered by an ingestion engine proven in HFT.",
  },
  {
    id: "observability",
    title: "Real-Time Observability",
    icon: <VisibilityIcon color="primary" sx={{ fontSize: 40 }} aria-hidden />,
    pain:
      "Logs, metrics, and traces live in three stacks. Query latency is batch-grade. Migrating off InfluxDB is painful because InfluxQL is nothing like SQL.",
    capabilities: [
      { need: "Unified time-series engine", feature: "5.52M events/sec, standard SQL over logs / metrics / traces — one engine, three signals" },
      { need: "Grafana out of the box", feature: "ClickHouse-compatible HTTP API on port 8123 works with the existing ClickHouse Grafana plugin" },
      { need: "Tiered retention", feature: "HDB Hot → Warm → Cold → S3 with ALTER TABLE SET STORAGE POLICY and TTL-driven eviction" },
      { need: "Migration path", feature: "InfluxDB migrator + ClickHouse / DuckDB / TimescaleDB migrators with --dest-table routing" },
      { need: "Low MTTR", feature: "µs query latency means alerts fire seconds after the incident, not minutes" },
    ],
    proof:
      "Same ingestion engine as finance (5.52M events/sec). Standard SQL, no PromQL / Flux lock-in. InfluxDB migration tool on P10 backlog.",
    killer: "Logs, metrics, traces — one unified time-series engine.",
  },
];

function SolutionSection({ s }: { s: Solution }) {
  return (
    <Card id={s.id} sx={{ p: { xs: 3, md: 4 }, scrollMarginTop: 80 }}>
      <Box sx={{ display: "flex", alignItems: "center", gap: 2, mb: 2, flexWrap: "wrap" }}>
        {s.icon}
        <Typography variant="h4" component="h2">{s.title}</Typography>
        <Chip label={s.killer} color="secondary" variant="outlined" sx={{ ml: { md: "auto" }, fontWeight: 600 }} />
      </Box>

      <Typography variant="subtitle2" color="text.secondary" sx={{ mb: 0.5, textTransform: "uppercase", letterSpacing: "0.05em" }}>
        The pain
      </Typography>
      <Typography variant="body1" color="text.primary" sx={{ mb: 3 }}>
        {s.pain}
      </Typography>

      <Typography variant="subtitle2" color="text.secondary" sx={{ mb: 1, textTransform: "uppercase", letterSpacing: "0.05em" }}>
        What ZeptoDB ships today
      </Typography>
      <Table size="small" sx={{ mb: 3 }}>
        <TableBody>
          {s.capabilities.map((c) => (
            <TableRow key={c.need}>
              <TableCell sx={{ width: { xs: "40%", md: "30%" }, verticalAlign: "top", fontWeight: 600, borderBottom: "1px solid", borderColor: "divider" }}>
                {c.need}
              </TableCell>
              <TableCell sx={{ color: "text.secondary", borderBottom: "1px solid", borderColor: "divider" }}>
                {c.feature}
              </TableCell>
            </TableRow>
          ))}
        </TableBody>
      </Table>

      <Typography variant="subtitle2" color="text.secondary" sx={{ mb: 0.5, textTransform: "uppercase", letterSpacing: "0.05em" }}>
        Proof point
      </Typography>
      <Typography variant="body2" color="text.secondary">
        {s.proof}
      </Typography>
    </Card>
  );
}

export default function Solutions() {
  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 4, pb: 6 }}>
      <Box sx={{ textAlign: "center", pt: 2, mb: 2 }}>
        <Typography variant="h3" component="h1" color="primary" sx={{ mb: 2 }}>
          Solutions
        </Typography>
        <Typography variant="h6" color="text.secondary" sx={{ maxWidth: 820, mx: "auto", fontWeight: 400 }}>
          The same engine powers HFT desks, semiconductor fabs, autonomous fleets, live game backends, and real-time observability stacks. Pick your vertical below.
        </Typography>
      </Box>

      {SOLUTIONS.map((s) => (
        <SolutionSection key={s.id} s={s} />
      ))}

      {/* + more verticals footer */}
      <Card sx={{ p: { xs: 3, md: 4 } }}>
        <Typography variant="h5" component="h2" sx={{ mb: 1 }}>+ more verticals</Typography>
        <Typography variant="body2" color="text.secondary">
          Crypto / DeFi — multi-exchange order-book streaming with Binance and Kafka consumers, cross-exchange ASOF JOIN for arbitrage. Autonomous Vehicles / Robotics — LiDAR + camera + IMU fusion with Window JOIN (ROS2 plugin is on the P9 backlog). Logistics — AGV fleets, WMS event timelines, and cold-chain audit trails (design doc and benchmark suite tracked in P9). The engine is the same; only the feed handler changes.
        </Typography>
      </Card>
    </Box>
  );
}
