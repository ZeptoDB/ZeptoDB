"use client";
import { Box, Paper, Typography, Grid, Chip, useTheme, Table, TableHead, TableRow, TableCell, TableBody, TableContainer } from "@mui/material";
import { useQuery } from "@tanstack/react-query";
import { fetchStats, fetchHealth, fetchVersion, querySQL } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import { LineChart, Line, XAxis, YAxis, Tooltip, ResponsiveContainer, BarChart, Bar, Cell } from "recharts";
import { useRef, useEffect, useState } from "react";

const MONO = { fontFamily: "'JetBrains Mono', monospace" };
const tooltipStyle = { backgroundColor: "#1B2129", border: "1px solid rgba(255, 255, 255, 0.08)", borderRadius: 8, fontSize: 12 };

function StatCard({ label, value, sub, color = "primary.main" }: { label: string; value: string | number; sub?: string; color?: string }) {
  return (
    <Paper sx={{ p: 2.5, border: "1px solid rgba(255, 255, 255, 0.08)" }}>
      <Typography variant="caption" color="text.secondary" sx={{ fontWeight: 600, textTransform: "uppercase", letterSpacing: "0.04em", fontSize: 10 }}>{label}</Typography>
      <Typography variant="h5" sx={{ fontWeight: 700, color, ...MONO, mt: 0.5 }}>
        {typeof value === "number" ? value.toLocaleString() : value}
      </Typography>
      {sub && <Typography variant="caption" color="text.secondary" sx={{ fontSize: 10 }}>{sub}</Typography>}
    </Paper>
  );
}

function formatNs(ns: number) {
  if (ns >= 1_000_000) return `${(ns / 1_000_000).toFixed(1)}ms`;
  if (ns >= 1_000) return `${(ns / 1_000).toFixed(0)}μs`;
  return `${ns}ns`;
}

interface TableInfo { name: string; columns: number; rows: number }

export default function DashboardPage() {
  const theme = useTheme();
  const { auth } = useAuth();
  const { data } = useQuery({ queryKey: ["stats"], queryFn: () => fetchStats(auth?.apiKey), refetchInterval: 2000 });
  const { data: health } = useQuery({ queryKey: ["health"], queryFn: () => fetchHealth(auth?.apiKey), refetchInterval: 5000 });
  const { data: version } = useQuery({ queryKey: ["version"], queryFn: () => fetchVersion(auth?.apiKey) });

  // Tables summary
  const { data: tables } = useQuery<TableInfo[]>({
    queryKey: ["dashboard-tables"],
    queryFn: async () => {
      const r = await querySQL("SHOW TABLES", auth?.apiKey);
      const names: string[] = (r.data ?? []).map((row: (string | number)[]) => String(row[0]));
      return Promise.all(names.map(async (name) => {
        try {
          const d = await querySQL(`DESCRIBE ${name}`, auth?.apiKey);
          const c = await querySQL(`SELECT count(*) FROM ${name}`, auth?.apiKey);
          const rows = c.data?.[0]?.[0] ?? 0;
          return { name, columns: d.data?.length ?? 0, rows: Number(rows) };
        } catch { return { name, columns: 0, rows: 0 }; }
      }));
    },
    refetchInterval: 10000,
  });

  const healthy = health?.status === "healthy";
  const history = useRef<{ time: string; rate: number }[]>([]);
  const prevIngested = useRef(0);
  const [chartData, setChartData] = useState<{ time: string; rate: number }[]>([]);

  useEffect(() => {
    if (!data) return;
    const now = new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
    const rate = Math.max(0, (data.ticks_ingested ?? 0) - prevIngested.current);
    prevIngested.current = data.ticks_ingested ?? 0;
    history.current = [...history.current.slice(-29), { time: now, rate }];
    setChartData([...history.current]);
  }, [data]);

  const dropRate = data && data.ticks_ingested > 0 ? ((data.ticks_dropped / data.ticks_ingested) * 100) : 0;
  const totalTableRows = tables?.reduce((s, t) => s + t.rows, 0) ?? 0;

  const tableBarData = (tables ?? []).map((t) => ({ name: t.name, rows: t.rows }));
  const BAR_COLORS = ["#4D7CFF", "#00E676", "#FFB300", "#FF1744", "#00F5D4", "#8EACFF", "#ff7043", "#8d6e63"];

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 3 }}>
      {/* Header */}
      <Box sx={{ display: "flex", alignItems: "center", gap: 2 }}>
        <Typography variant="h6">Dashboard</Typography>
        <Box sx={{ display: "flex", alignItems: "center", gap: 1 }}>
          <Box sx={{ width: 8, height: 8, borderRadius: "50%", bgcolor: healthy ? "success.main" : "error.main", boxShadow: `0 0 8px ${healthy ? theme.palette.success.main : theme.palette.error.main}80` }} />
          <Typography variant="body2" sx={{ fontWeight: 600, fontSize: 12 }}>{healthy ? "Healthy" : "Unhealthy"}</Typography>
        </Box>
        {version && <Chip label={`${version.engine ?? "ZeptoDB"} v${version.version ?? "?"}`} size="small" variant="outlined" sx={{ ...MONO, fontSize: 11 }} />}
      </Box>

      {/* Stat cards */}
      <Grid container spacing={2}>
        <Grid size={{ xs: 6, md: 2.4 }}><StatCard label="Ticks Ingested" value={data?.ticks_ingested ?? 0} /></Grid>
        <Grid size={{ xs: 6, md: 2.4 }}><StatCard label="Ticks Stored" value={data?.ticks_stored ?? 0} /></Grid>
        <Grid size={{ xs: 6, md: 2.4 }}><StatCard label="Queries Executed" value={data?.queries_executed ?? 0} color="secondary.main" /></Grid>
        <Grid size={{ xs: 6, md: 2.4 }}><StatCard label="Partitions" value={data?.partitions_created ?? 0} sub={data?.partitions_evicted ? `${data.partitions_evicted} evicted` : undefined} /></Grid>
        <Grid size={{ xs: 6, md: 2.4 }}><StatCard label="Ingest Latency" value={formatNs(data?.last_ingest_latency_ns ?? 0)} color="info.main" /></Grid>
      </Grid>

      {/* Drop rate warning */}
      {dropRate > 0 && (
        <Paper sx={{ p: 1.5, px: 2, border: "1px solid", borderColor: dropRate > 1 ? "error.main" : "warning.main", display: "flex", alignItems: "center", gap: 1.5 }}>
          <Box sx={{ width: 8, height: 8, borderRadius: "50%", bgcolor: dropRate > 1 ? "error.main" : "warning.main", flexShrink: 0 }} />
          <Typography variant="body2" sx={{ ...MONO, fontSize: 12 }}>
            Drop rate: {dropRate.toFixed(2)}% ({(data?.ticks_dropped ?? 0).toLocaleString()} / {(data?.ticks_ingested ?? 0).toLocaleString()})
          </Typography>
        </Paper>
      )}

      <Grid container spacing={2}>
        {/* Ingestion rate chart */}
        <Grid size={{ xs: 12, md: 7 }}>
          <Paper sx={{ p: 2, border: "1px solid rgba(255, 255, 255, 0.08)", height: "100%" }}>
            <Typography variant="body2" color="text.secondary" sx={{ mb: 1, fontWeight: 600 }}>Ingestion Rate (ticks/2s)</Typography>
            <ResponsiveContainer width="100%" height={240}>
              <LineChart data={chartData}>
                <XAxis dataKey="time" tick={{ fontSize: 9, fill: theme.palette.text.secondary }} stroke={theme.palette.divider} />
                <YAxis tick={{ fontSize: 9, fill: theme.palette.text.secondary }} stroke={theme.palette.divider} />
                <Tooltip contentStyle={tooltipStyle} />
                <Line type="monotone" dataKey="rate" stroke={theme.palette.primary.main} strokeWidth={2.5} dot={false}
                  style={{ filter: `drop-shadow(0px 3px 8px ${theme.palette.primary.main}60)` }} />
              </LineChart>
            </ResponsiveContainer>
          </Paper>
        </Grid>

        {/* Tables summary */}
        <Grid size={{ xs: 12, md: 5 }}>
          <Paper sx={{ p: 2, border: "1px solid rgba(255, 255, 255, 0.08)", height: "100%" }}>
            <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between", mb: 1 }}>
              <Typography variant="body2" color="text.secondary" sx={{ fontWeight: 600 }}>Tables</Typography>
              <Chip label={`${tables?.length ?? 0} tables · ${totalTableRows.toLocaleString()} rows`} size="small" variant="outlined" sx={{ ...MONO, fontSize: 10 }} />
            </Box>
            {tables && tables.length > 0 ? (
              <TableContainer sx={{ maxHeight: 220 }}>
                <Table size="small" stickyHeader>
                  <TableHead>
                    <TableRow>
                      <TableCell sx={{ fontWeight: 600, fontSize: 11 }}>Name</TableCell>
                      <TableCell sx={{ fontWeight: 600, fontSize: 11 }} align="right">Columns</TableCell>
                      <TableCell sx={{ fontWeight: 600, fontSize: 11 }} align="right">Rows</TableCell>
                    </TableRow>
                  </TableHead>
                  <TableBody>
                    {tables.map((t) => (
                      <TableRow key={t.name} hover>
                        <TableCell sx={{ ...MONO, fontSize: 12 }}>{t.name}</TableCell>
                        <TableCell sx={{ ...MONO, fontSize: 12 }} align="right">{t.columns}</TableCell>
                        <TableCell sx={{ ...MONO, fontSize: 12 }} align="right">{t.rows.toLocaleString()}</TableCell>
                      </TableRow>
                    ))}
                  </TableBody>
                </Table>
              </TableContainer>
            ) : (
              <Box sx={{ display: "flex", alignItems: "center", justifyContent: "center", height: 200, color: "text.secondary" }}>
                <Typography variant="body2">No tables yet</Typography>
              </Box>
            )}
          </Paper>
        </Grid>
      </Grid>

      {/* Rows per table bar chart */}
      {tableBarData.length > 0 && (
        <Paper sx={{ p: 2, border: "1px solid rgba(255, 255, 255, 0.08)" }}>
          <Typography variant="body2" color="text.secondary" sx={{ mb: 1, fontWeight: 600 }}>Rows per Table</Typography>
          <ResponsiveContainer width="100%" height={200}>
            <BarChart data={tableBarData}>
              <XAxis dataKey="name" tick={{ fontSize: 11 }} stroke={theme.palette.divider} />
              <YAxis tick={{ fontSize: 11 }} stroke={theme.palette.divider} />
              <Tooltip contentStyle={tooltipStyle} />
              <Bar dataKey="rows" radius={[4, 4, 0, 0]}>
                {tableBarData.map((_, i) => <Cell key={i} fill={BAR_COLORS[i % BAR_COLORS.length]} />)}
              </Bar>
            </BarChart>
          </ResponsiveContainer>
        </Paper>
      )}

      {/* Rows scanned */}
      <Grid container spacing={2}>
        <Grid size={{ xs: 6, md: 4 }}><StatCard label="Total Rows Scanned" value={data?.total_rows_scanned ?? 0} /></Grid>
        <Grid size={{ xs: 6, md: 4 }}><StatCard label="Ticks Dropped" value={data?.ticks_dropped ?? 0} color={dropRate > 0 ? "error.main" : "text.secondary"} /></Grid>
        <Grid size={{ xs: 6, md: 4 }}>
          <StatCard label="Avg Query Cost" value={data?.queries_executed ? `${Math.round((data?.total_rows_scanned ?? 0) / data.queries_executed).toLocaleString()} rows` : "—"} color="text.secondary" />
        </Grid>
      </Grid>
    </Box>
  );
}
