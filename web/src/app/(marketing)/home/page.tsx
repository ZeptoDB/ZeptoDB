"use client";
import { Box, Typography, Button, Card, Stack } from "@mui/material";
import Link from "next/link";
import StorageIcon from "@mui/icons-material/Storage";
import SmartToyIcon from "@mui/icons-material/SmartToy";
import ShowChartIcon from "@mui/icons-material/ShowChart";
import SportsEsportsIcon from "@mui/icons-material/SportsEsports";
import FactoryIcon from "@mui/icons-material/Factory";
import MoreHorizIcon from "@mui/icons-material/MoreHoriz";
import BoltIcon from "@mui/icons-material/Bolt";
import SyncAltIcon from "@mui/icons-material/SyncAlt";
import CodeIcon from "@mui/icons-material/Code";

const METRICS = [
  { value: "5.52M", label: "ticks/sec ingestion" },
  { value: "272µs", label: "1M-row filter p50" },
  { value: "6 native feeds", label: "OPC-UA · MQTT · Kafka · FIX · ITCH · Binance" },
  { value: "kdb+-class", label: "performance without the license" },
];

const INDUSTRIES = [
  {
    id: "physical-ai",
    title: "Physical AI",
    icon: <SmartToyIcon fontSize="large" color="primary" aria-hidden />,
    summary: "OPC-UA, sensor fusion, robotics replay.",
    href: "/solutions#physical-ai",
  },
  {
    id: "finance",
    title: "Finance / HFT",
    icon: <ShowChartIcon fontSize="large" color="primary" aria-hidden />,
    summary: "Tick data, ASOF JOIN, backtesting.",
    href: "/solutions#finance",
  },
  {
    id: "game",
    title: "Game",
    icon: <SportsEsportsIcon fontSize="large" color="primary" aria-hidden />,
    summary: "Real-time telemetry, anti-cheat, live-ops analytics.",
    href: "/solutions#game",
  },
  {
    id: "iot",
    title: "IoT / Smart Factory",
    icon: <FactoryIcon fontSize="large" color="primary" aria-hidden />,
    summary: "10 kHz sensors, predictive maintenance.",
    href: "/solutions#iot",
  },
  {
    id: "more",
    title: "+ more verticals",
    icon: <MoreHorizIcon fontSize="large" color="primary" aria-hidden />,
    summary: "Observability, Crypto/DeFi, autonomous vehicles.",
    href: "/solutions",
  },
];

const WHY = [
  {
    icon: <BoltIcon color="secondary" aria-hidden />,
    title: "µs latency — not ms, not seconds",
    body: "Lock-free MPMC ring buffer + Highway SIMD + LLVM JIT. 272µs 1M-row filter, 532µs VWAP, 522ns zero-copy Python column access. No GC. No JVM. No surprises.",
  },
  {
    icon: <SyncAltIcon color="secondary" aria-hidden />,
    title: "Research → Production → Compliance",
    body: "Same engine from a quant's Jupyter notebook to the production trading desk to the audit log. No data migration between tiers. HDB tiered storage writes to Parquet and S3 for cold analytics and regulatory retention.",
  },
  {
    icon: <CodeIcon color="secondary" aria-hidden />,
    title: "Open source, standard SQL, Python zero-copy",
    body: "ClickHouse-compatible HTTP API on port 8123. ASOF JOIN, Window JOIN, EMA, DELTA, xbar, VWAP as first-class SQL. Polars-style Python DSL with 522ns numpy views straight into arena memory.",
  },
];

const SQL_SNIPPET = `-- ASOF JOIN — point-in-time quote for every trade
SELECT t.price, q.bid, q.ask
FROM trades t
ASOF JOIN quotes q
ON t.symbol = q.symbol
AND t.timestamp >= q.timestamp;

-- 5-minute OHLCV bars with xbar
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low,  last(price) AS close,
       sum(volume) AS volume
FROM trades WHERE symbol = 'AAPL'
GROUP BY xbar(timestamp, 300000000000);`;

const PY_SNIPPET = `import zeptodb
import polars as pl

# Ingest a Polars frame zero-copy into the engine
db = zeptodb.Pipeline()
db.start()
zeptodb.from_polars(db, df, table_name="sensors")

# Run analytical SQL and get back a numpy view (522 ns)
ema = db.query("""
    SELECT equipment_id,
           EMA(vibration, 0.1) OVER (
             PARTITION BY equipment_id
           ) AS ema
    FROM sensors
""")
arr = db.get_column(symbol=1, name="ema")  # no copy`;

function Metric({ value, label }: { value: string; label: string }) {
  return (
    <Card sx={{ p: 3, textAlign: "center", height: "100%" }}>
      <Typography variant="h4" color="secondary" sx={{ fontWeight: 800, mb: 0.5 }}>
        {value}
      </Typography>
      <Typography variant="body2" color="text.secondary">
        {label}
      </Typography>
    </Card>
  );
}

function CodeBlock({ title, code }: { title: string; code: string }) {
  return (
    <Card sx={{ p: 0, overflow: "hidden" }}>
      <Box sx={{ px: 2, py: 1, borderBottom: "1px solid", borderColor: "divider" }}>
        <Typography variant="subtitle2" color="text.secondary">
          {title}
        </Typography>
      </Box>
      <Box
        component="pre"
        sx={{
          m: 0,
          p: 2,
          fontFamily: "'JetBrains Mono', ui-monospace, SFMono-Regular, Menlo, Consolas, monospace",
          fontSize: "0.82rem",
          lineHeight: 1.55,
          color: "text.primary",
          overflowX: "auto",
          whiteSpace: "pre",
        }}
      >
        {code}
      </Box>
    </Card>
  );
}

export default function Home() {
  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 6, pb: 6 }}>
      {/* Hero */}
      <Box sx={{ textAlign: "center", pt: 2 }}>
        <StorageIcon color="secondary" sx={{ fontSize: 64, mb: 2 }} aria-hidden />
        <Typography variant="h2" component="h1" color="primary" sx={{ mb: 2, fontSize: { xs: "2rem", md: "2.75rem" } }}>
          The Time-Series Database for Physical AI, IoT, and Real-Time Analytics
        </Typography>
        <Typography variant="h6" color="text.secondary" sx={{ maxWidth: 860, mx: "auto", mb: 3, fontWeight: 400 }}>
          From factory-floor sensors to autonomous fleets to live game servers to trading desks — one database for every high-velocity time-series workload. Microsecond latency, standard SQL, open source.
        </Typography>
        <Stack direction={{ xs: "column", sm: "row" }} spacing={2} justifyContent="center" sx={{ mb: 1 }}>
          <Button
            component="a"
            href="https://github.com/ZeptoDB/ZeptoDB#-quick-start"
            target="_blank"
            rel="noopener noreferrer"
            variant="contained"
            size="large"
          >
            Get Started
          </Button>
          <Button component={Link} href="/solutions" variant="outlined" size="large">
            View Solutions
          </Button>
          <Button
            component="a"
            href="https://discord.gg/zeptodb"
            target="_blank"
            rel="noopener noreferrer"
            variant="outlined"
            size="large"
            sx={{
              borderColor: "#5865F2",
              color: "#5865F2",
              "&:hover": { borderColor: "#4752C4", bgcolor: "#5865F210" },
            }}
          >
            Join Discord
          </Button>
          <Button
            component="a"
            href="https://github.com/ZeptoDB/ZeptoDB"
            target="_blank"
            rel="noopener noreferrer"
            variant="outlined"
            size="large"
          >
            GitHub — Star
          </Button>
        </Stack>
      </Box>

      {/* Proof metrics strip */}
      <Box
        sx={{
          display: "grid",
          gridTemplateColumns: { xs: "1fr", sm: "repeat(2, 1fr)", md: "repeat(4, 1fr)" },
          gap: 2,
        }}
      >
        {METRICS.map((m) => (
          <Metric key={m.value} value={m.value} label={m.label} />
        ))}
      </Box>

      {/* Industry switcher / tab cards */}
      <Box>
        <Typography variant="h4" component="h2" textAlign="center" sx={{ mb: 3 }}>
          Built for every high-velocity time-series workload
        </Typography>
        <Box
          sx={{
            display: "grid",
            gridTemplateColumns: { xs: "1fr", sm: "repeat(2, 1fr)", md: "repeat(3, 1fr)", lg: "repeat(5, 1fr)" },
            gap: 3,
          }}
        >
          {INDUSTRIES.map((ind) => (
            <Card key={ind.id} sx={{ p: 3, textAlign: "center", display: "flex", flexDirection: "column", height: "100%" }}>
              <Box sx={{ mb: 1.5 }}>{ind.icon}</Box>
              <Typography variant="h6" component="h3" sx={{ mb: 1 }}>{ind.title}</Typography>
              <Typography variant="body2" color="text.secondary" sx={{ mb: 2, flexGrow: 1 }}>
                {ind.summary}
              </Typography>
              <Button
                component={Link}
                href={ind.href}
                size="small"
                variant="text"
                sx={{ alignSelf: "center" }}
                aria-label={`Learn more about ${ind.title}`}
              >
                Learn more →
              </Button>
            </Card>
          ))}
        </Box>
      </Box>

      {/* Why ZeptoDB */}
      <Box>
        <Typography variant="h4" component="h2" textAlign="center" sx={{ mb: 3 }}>
          Why ZeptoDB
        </Typography>
        <Box sx={{ display: "grid", gridTemplateColumns: { xs: "1fr", md: "repeat(3, 1fr)" }, gap: 3 }}>
          {WHY.map((w) => (
            <Card key={w.title} sx={{ p: 3 }}>
              <Box sx={{ mb: 1.5 }}>{w.icon}</Box>
              <Typography variant="h6" component="h3" sx={{ mb: 1 }}>{w.title}</Typography>
              <Typography variant="body2" color="text.secondary">{w.body}</Typography>
            </Card>
          ))}
        </Box>
      </Box>

      {/* Code snippet block */}
      <Box>
        <Typography variant="h4" component="h2" textAlign="center" sx={{ mb: 3 }}>
          SQL where you need it. Python where you work.
        </Typography>
        <Box sx={{ display: "grid", gridTemplateColumns: { xs: "1fr", md: "1fr 1fr" }, gap: 3 }}>
          <CodeBlock title="SQL — ASOF JOIN + xbar OHLCV" code={SQL_SNIPPET} />
          <CodeBlock title="Python — from_polars + zero-copy" code={PY_SNIPPET} />
        </Box>
      </Box>

      {/* Footer CTAs */}
      <Box sx={{ textAlign: "center", pt: 2 }}>
        <Stack direction={{ xs: "column", sm: "row" }} spacing={2} justifyContent="center">
          <Button
            component="a"
            href="https://github.com/ZeptoDB/ZeptoDB#-quick-start"
            target="_blank"
            rel="noopener noreferrer"
            variant="contained"
            size="large"
          >
            Get Started
          </Button>
          <Button component={Link} href="/performance" variant="outlined" size="large">
            View Benchmarks
          </Button>
          <Button component={Link} href="/pricing" variant="text" size="large">
            Pricing
          </Button>
        </Stack>
      </Box>
    </Box>
  );
}
