"use client";
import { Box, Typography, Card, Stack } from "@mui/material";
import InputIcon from "@mui/icons-material/Input";
import QueryStatsIcon from "@mui/icons-material/QueryStats";
import CloudQueueIcon from "@mui/icons-material/CloudQueue";
import LockIcon from "@mui/icons-material/Lock";
import CheckCircleOutlineIcon from "@mui/icons-material/CheckCircleOutline";

interface Group {
  title: string;
  icon: React.ReactNode;
  subtitle: string;
  items: string[];
}

const GROUPS: Group[] = [
  {
    title: "Ingest",
    subtitle: "Every modern time-series feed, at line rate.",
    icon: <InputIcon color="primary" sx={{ fontSize: 32 }} aria-hidden />,
    items: [
      "Native consumers: Kafka, MQTT (QoS 0/1/2, topic wildcards), OPC-UA (Basic256Sha256, reconnect, quality mapping), NASDAQ ITCH (250 ns parser), FIX (350 ns parser), Binance.",
      "5.52M ticks/sec sustained on a single node via lock-free MPMC ring buffer.",
      "Durability via write-ahead log + quorum replication; auto-snapshot every 60 s with replay on restart (≤ 60 s data loss).",
      "Backpressure retry, per-feed Prometheus metrics, and client-side partition routing so feeds scale linearly with pod count.",
    ],
  },
  {
    title: "Query",
    subtitle: "Standard SQL with the financial primitives you actually need.",
    icon: <QueryStatsIcon color="primary" sx={{ fontSize: 32 }} aria-hidden />,
    items: [
      "Standard SQL through a ClickHouse-compatible HTTP API on port 8123. Parses in 1.5–4.5 µs, zero allocation.",
      "JOIN families: ASOF JOIN, Hash JOIN, Window JOIN, UNION JOIN (uj), PLUS JOIN (pj), AJ0.",
      "Window functions: EMA, DELTA, RATIO, LAG, LEAD, ROW_NUMBER, RANK, DENSE_RANK, rolling SUM / AVG / MIN / MAX.",
      "Time-series primitives: xbar, OHLCV (first/last/max/min), VWAP — all first-class SQL, no UDFs.",
      "JIT-compiled expressions + Google Highway SIMD kernels. Cost-based planner (HyperLogLog stats, predicate / projection pushdown, HASH_JOIN build-side selection).",
    ],
  },
  {
    title: "Deploy",
    subtitle: "Single binary to Kubernetes, without rewriting your stack.",
    icon: <CloudQueueIcon color="primary" sx={{ fontSize: 32 }} aria-hidden />,
    items: [
      "Official Helm chart with rolling upgrades, PodDisruptionBudget, antiAffinity, and topologySpreadConstraints.",
      "Multi-node cluster with consistent-hash routing (2 ns per lookup), auto failover, and Coordinator HA lease.",
      "HDB tiered storage: Hot (memory) → Warm (SSD) → Cold (S3) with ALTER TABLE SET STORAGE POLICY and TTL-driven eviction.",
      "Prometheus /metrics, structured JSON access logs, slow-query log — wires straight into Grafana and Loki.",
      "Zero-downtime live rebalancing with bandwidth throttling; stateless zepto_ingest_node pods scale ingest independently of storage.",
    ],
  },
  {
    title: "Secure",
    subtitle: "Enterprise controls built in, not bolted on.",
    icon: <LockIcon color="primary" sx={{ fontSize: 32 }} aria-hidden />,
    items: [
      "TLS / HTTPS for HTTP API and inter-node RPC.",
      "API Key + JWT / OIDC single sign-on; pluggable identity providers.",
      "RBAC with 5 roles (admin, writer, reader, analyst, metrics) and per-tenant table namespace isolation.",
      "Audit log compatible with SOC 2, EMIR, and MiFID II retention; Vault-backed API key store.",
      "Per-key rate limiting; multi-tenancy with per-tenant quotas and X-Zepto-Allowed-Tables ACL on every DDL and DML path.",
    ],
  },
];

export default function Features() {
  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 4, pb: 6 }}>
      <Box sx={{ textAlign: "center", pt: 2, mb: 2 }}>
        <Typography variant="h3" component="h1" color="primary" sx={{ mb: 2 }}>
          Platform Features
        </Typography>
        <Typography variant="h6" color="text.secondary" sx={{ maxWidth: 780, mx: "auto", fontWeight: 400 }}>
          Four capability groups, one engine. Ingest from any modern feed, query it in standard SQL, deploy on Kubernetes, and meet enterprise security requirements out of the box.
        </Typography>
      </Box>

      <Box
        sx={{
          display: "grid",
          gridTemplateColumns: { xs: "1fr", md: "repeat(2, 1fr)" },
          gap: 3,
        }}
      >
        {GROUPS.map((g) => (
          <Card key={g.title} sx={{ p: { xs: 3, md: 4 } }}>
            <Box sx={{ display: "flex", alignItems: "center", gap: 1.5, mb: 1 }}>
              {g.icon}
              <Typography variant="h4" component="h2">{g.title}</Typography>
            </Box>
            <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>
              {g.subtitle}
            </Typography>
            <Stack spacing={1.5}>
              {g.items.map((item) => (
                <Box key={item} sx={{ display: "flex", gap: 1.5, alignItems: "flex-start" }}>
                  <CheckCircleOutlineIcon color="secondary" sx={{ mt: "2px", fontSize: 20, flexShrink: 0 }} />
                  <Typography variant="body2" color="text.primary">{item}</Typography>
                </Box>
              ))}
            </Stack>
          </Card>
        ))}
      </Box>
    </Box>
  );
}
