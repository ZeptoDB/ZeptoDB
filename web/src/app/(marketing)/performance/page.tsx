"use client";
import {
  Box,
  Typography,
  Card,
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableRow,
} from "@mui/material";

interface Row {
  engine: string;
  accent?: boolean;
  ingestion: string;
  filter: string;
  asof: string;
  license: string;
  deployment: string;
}

const ROWS: Row[] = [
  {
    engine: "ZeptoDB",
    accent: true,
    ingestion: "5.52M events/sec",
    filter: "272 µs",
    asof: "Native (ASOF JOIN + Window JOIN)",
    license: "Free — BUSL-1.1 (Apache-2.0-compatible community edition)",
    deployment: "Single binary · Docker · Helm · Kubernetes",
  },
  {
    engine: "kdb+",
    ingestion: "~5M events/sec",
    filter: "~300 µs",
    asof: "Native (aj / wj)",
    license: "$100K–500K per core per year",
    deployment: "Self-hosted (commercial install)",
  },
  {
    engine: "ClickHouse",
    ingestion: "~100K events/sec (1 INSERT/sec recommended)",
    filter: "~10 ms",
    asof: "UDF workaround",
    license: "Free — Apache 2.0",
    deployment: "Self-hosted · ClickHouse Cloud",
  },
  {
    engine: "InfluxDB",
    ingestion: "~500K events/sec (saturates above 10 kHz per series)",
    filter: "~100 ms+",
    asof: "Not supported",
    license: "Free community · paid Cloud / Enterprise",
    deployment: "Self-hosted · InfluxDB Cloud",
  },
];

const COLUMNS: { key: keyof Row; label: string }[] = [
  { key: "engine", label: "Engine" },
  { key: "ingestion", label: "Ingestion" },
  { key: "filter", label: "1M-row filter p50" },
  { key: "asof", label: "ASOF JOIN" },
  { key: "license", label: "License cost" },
  { key: "deployment", label: "Deployment" },
];

export default function Performance() {
  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 4, pb: 6 }}>
      <Box sx={{ textAlign: "center", pt: 2, mb: 2 }}>
        <Typography variant="h3" component="h1" color="primary" sx={{ mb: 2 }}>
          Performance
        </Typography>
        <Typography variant="h6" color="text.secondary" sx={{ maxWidth: 820, mx: "auto", fontWeight: 400 }}>
          Measured, single-node, end-to-end. ZeptoDB runs at kdb+-class latency on open-source pricing, without the q language and without the INSERT throttling other real-time engines require.
        </Typography>
      </Box>

      <Card sx={{ p: 0, overflow: "hidden" }}>
        <Box sx={{ px: { xs: 3, md: 4 }, pt: { xs: 3, md: 4 }, pb: 2 }}>
          <Typography variant="h4" component="h2">Benchmark comparison</Typography>
        </Box>
        <Box sx={{ overflowX: "auto" }}>
          <Table>
            <TableHead>
              <TableRow>
                {COLUMNS.map((c) => (
                  <TableCell key={c.key} sx={{ fontWeight: 700 }}>
                    {c.label}
                  </TableCell>
                ))}
              </TableRow>
            </TableHead>
            <TableBody>
              {ROWS.map((r) => (
                <TableRow
                  key={r.engine}
                  sx={{
                    bgcolor: r.accent ? (theme) => `${theme.palette.primary.main}10` : "transparent",
                  }}
                >
                  {COLUMNS.map((c) => (
                    <TableCell
                      key={c.key}
                      sx={{
                        fontWeight: c.key === "engine" ? 700 : 400,
                        color: r.accent && c.key === "engine" ? "primary.main" : "text.primary",
                        verticalAlign: "top",
                      }}
                    >
                      {r[c.key]}
                    </TableCell>
                  ))}
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </Box>
      </Card>

      {/* ZeptoDB detail card */}
      <Card sx={{ p: { xs: 3, md: 4 } }}>
        <Typography variant="h4" component="h2" sx={{ mb: 2 }} color="primary">
          ZeptoDB in detail
        </Typography>
        <Table size="small">
          <TableBody>
            {[
              ["Ingestion throughput", "5.52M events/sec — lock-free MPMC ring buffer"],
              ["Filter 1M rows (p50)", "272 µs — Highway SIMD vectorized scan"],
              ["VWAP 1M rows (p50)", "532 µs — fused price × volume aggregation"],
              ["GROUP BY (8 threads)", "248 µs — partition-parallel scatter / gather"],
              ["EMA 1M rows", "2.2 ms — streaming exponential moving average"],
              ["xbar (1M rows → 3,334 bars)", "11 ms — time-bucketed OHLCV"],
              ["SQL parse", "1.5–4.5 µs — recursive descent, zero allocation"],
              ["Python column access", "522 ns — zero-copy numpy view"],
              ["Indexed lookup (g# / p#)", "3.3 µs — 274× faster than full scan"],
              ["HDB flush to disk", "4.8 GB/s — LZ4 compressed"],
              ["Partition routing", "2 ns — consistent hash ring"],
            ].map(([k, v]) => (
              <TableRow key={k}>
                <TableCell sx={{ fontWeight: 600, width: { xs: "50%", md: "35%" }, borderBottom: "1px solid", borderColor: "divider" }}>
                  {k}
                </TableCell>
                <TableCell sx={{ color: "text.secondary", borderBottom: "1px solid", borderColor: "divider" }}>
                  {v}
                </TableCell>
              </TableRow>
            ))}
          </TableBody>
        </Table>
      </Card>

      {/* Test environment footer */}
      <Card sx={{ p: { xs: 3, md: 4 } }}>
        <Typography variant="h6" component="h2" sx={{ mb: 1 }}>Test environment</Typography>
        <Typography variant="body2" color="text.secondary" sx={{ mb: 1 }}>
          Single-node end-to-end measurements include SQL parsing. Ingestion numbers are sustained, not burst. ZeptoDB numbers were produced on c6i-class amd64 and Graviton (arm64) hosts with identical clang-19 builds for reproducibility — see devlogs 097 and 098 for the compiler-unification and register-allocation work that stabilised the baselines.
        </Typography>
        <Typography variant="body2" color="text.secondary" sx={{ mb: 1 }}>
          Competitor numbers are published ranges from vendor benchmarks, community reports, and the ZeptoDB competitive analysis. Where vendors publish multiple figures, we cite the favourable one.
        </Typography>
        <Typography variant="body2" color="text.secondary">
          Methodology, raw logs, and full multi-node EKS runs live in <Box component="code" sx={{ fontFamily: "'JetBrains Mono', ui-monospace, monospace", bgcolor: "background.paper", px: 0.5 }}>docs/bench/</Box> — notably <Box component="code" sx={{ fontFamily: "'JetBrains Mono', ui-monospace, monospace", bgcolor: "background.paper", px: 0.5 }}>docs/bench/results_multinode.md</Box> for Round 1 / Round 2 / Round 3 horizontal-scaling findings, and <Box component="code" sx={{ fontFamily: "'JetBrains Mono', ui-monospace, monospace", bgcolor: "background.paper", px: 0.5 }}>docs/business/competitive_analysis.md</Box> for the full comparison matrix.
        </Typography>
      </Card>
    </Box>
  );
}
