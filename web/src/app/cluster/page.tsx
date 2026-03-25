"use client";
import { Box, Paper, Typography, Grid, Chip, Table, TableHead, TableRow, TableCell, TableBody, TableContainer } from "@mui/material";
import { useQuery } from "@tanstack/react-query";
import { fetchNodes, fetchCluster, fetchMetricsHistory } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import { BarChart, Bar, LineChart, Line, XAxis, YAxis, Tooltip, ResponsiveContainer, Cell, Legend } from "recharts";

interface NodeInfo {
  id: number;
  host: string;
  port: number;
  state: string;
  ticks_ingested: number;
  ticks_stored: number;
  queries_executed: number;
}

interface MetricsPoint {
  timestamp_ms: number;
  node_id: number;
  ticks_ingested: number;
  ticks_stored: number;
  ticks_dropped: number;
  queries_executed: number;
  total_rows_scanned: number;
  partitions_created: number;
  last_ingest_latency_ns: number;
}

const stateColor: Record<string, "success" | "warning" | "error" | "default" | "info"> = {
  ACTIVE: "success", SUSPECT: "warning", DEAD: "error", JOINING: "info", LEAVING: "default",
};

const barColor: Record<string, string> = {
  ACTIVE: "#66bb6a", SUSPECT: "#ffa726", DEAD: "#ef5350", JOINING: "#42a5f5", LEAVING: "#bdbdbd",
};

const NODE_COLORS = ["#5C6BC0", "#66bb6a", "#ffa726", "#ef5350", "#ab47bc", "#26c6da", "#ff7043", "#8d6e63"];

function StatCard({ label, value, color = "primary.main" }: { label: string; value: string | number; color?: string }) {
  return (
    <Paper sx={{ p: 2.5, border: "1px solid #1E293B" }}>
      <Typography variant="caption" color="text.secondary">{label}</Typography>
      <Typography variant="h5" sx={{ fontWeight: 700, color, fontFamily: "'JetBrains Mono', monospace" }}>
        {typeof value === "number" ? value.toLocaleString() : value}
      </Typography>
    </Paper>
  );
}

/** Convert raw metrics array into time-series keyed by timestamp for multi-node line chart */
export function buildTimeSeries(metrics: MetricsPoint[], nodeIds: number[]) {
  const byTime = new Map<number, Record<string, number>>();
  for (const m of metrics) {
    let entry = byTime.get(m.timestamp_ms);
    if (!entry) {
      entry = { timestamp_ms: m.timestamp_ms };
      byTime.set(m.timestamp_ms, entry);
    }
    entry[`ingested_${m.node_id}`] = m.ticks_ingested;
    entry[`queries_${m.node_id}`] = m.queries_executed;
    entry[`latency_${m.node_id}`] = m.last_ingest_latency_ns;
  }
  return Array.from(byTime.values()).sort((a, b) => a.timestamp_ms - b.timestamp_ms);
}

function formatTime(ms: unknown) {
  return new Date(Number(ms)).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

export default function ClusterPage() {
  const { auth } = useAuth();

  const { data: cluster } = useQuery({
    queryKey: ["cluster"],
    queryFn: () => fetchCluster(auth?.apiKey),
    refetchInterval: 3000,
  });

  const { data: nodesData } = useQuery({
    queryKey: ["nodes"],
    queryFn: () => fetchNodes(auth?.apiKey),
    refetchInterval: 3000,
  });

  const { data: metricsRaw } = useQuery({
    queryKey: ["metrics-history"],
    queryFn: () => {
      const thirtyMinAgo = Date.now() - 30 * 60 * 1000;
      return fetchMetricsHistory(auth?.apiKey, thirtyMinAgo, 600);
    },
    refetchInterval: 3000,
  });

  const nodes: NodeInfo[] = nodesData?.nodes ?? [];
  const activeCount = nodes.filter((n) => n.state === "ACTIVE").length;
  const totalTicks = nodes.reduce((s, n) => s + (n.ticks_ingested ?? 0), 0);
  const totalQueries = nodes.reduce((s, n) => s + (n.queries_executed ?? 0), 0);

  const barData = nodes.map((n) => ({ name: `Node ${n.id}`, ticks: n.ticks_stored, state: n.state }));

  const metrics: MetricsPoint[] = metricsRaw ?? [];
  const nodeIds = [...new Set(metrics.map((m) => m.node_id))].sort();
  const timeSeries = buildTimeSeries(metrics, nodeIds);

  const tooltipStyle = { backgroundColor: "#111827", border: "1px solid #1E293B", borderRadius: 8, fontSize: 12 };

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 3 }}>
      <Typography variant="h6">Cluster Overview</Typography>

      {/* Summary cards */}
      <Grid container spacing={2}>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Mode" value={cluster?.mode ?? "—"} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Nodes" value={`${activeCount} / ${nodes.length}`} color={activeCount === nodes.length ? "success.main" : "warning.main"} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Total Ticks Ingested" value={totalTicks} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Total Queries" value={totalQueries} color="secondary.main" /></Grid>
      </Grid>

      {/* Node status table */}
      <Paper sx={{ border: "1px solid #1E293B" }}>
        <Box sx={{ px: 2, py: 1.5, borderBottom: "1px solid #1E293B" }}>
          <Typography variant="body2" color="text.secondary">Node Status</Typography>
        </Box>
        <TableContainer>
          <Table size="small">
            <TableHead>
              <TableRow>
                <TableCell sx={{ fontWeight: 600 }}>ID</TableCell>
                <TableCell sx={{ fontWeight: 600 }}>Host</TableCell>
                <TableCell sx={{ fontWeight: 600 }}>Port</TableCell>
                <TableCell sx={{ fontWeight: 600 }}>State</TableCell>
                <TableCell sx={{ fontWeight: 600 }} align="right">Ticks Ingested</TableCell>
                <TableCell sx={{ fontWeight: 600 }} align="right">Ticks Stored</TableCell>
                <TableCell sx={{ fontWeight: 600 }} align="right">Queries</TableCell>
              </TableRow>
            </TableHead>
            <TableBody>
              {nodes.map((n) => (
                <TableRow key={n.id} hover>
                  <TableCell sx={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 13 }}>{n.id}</TableCell>
                  <TableCell sx={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 13 }}>{n.host}</TableCell>
                  <TableCell sx={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 13 }}>{n.port}</TableCell>
                  <TableCell><Chip label={n.state} size="small" color={stateColor[n.state] ?? "default"} variant="outlined" /></TableCell>
                  <TableCell align="right" sx={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 13 }}>{n.ticks_ingested.toLocaleString()}</TableCell>
                  <TableCell align="right" sx={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 13 }}>{n.ticks_stored.toLocaleString()}</TableCell>
                  <TableCell align="right" sx={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 13 }}>{n.queries_executed.toLocaleString()}</TableCell>
                </TableRow>
              ))}
              {nodes.length === 0 && (
                <TableRow><TableCell colSpan={7} sx={{ textAlign: "center", color: "text.secondary", py: 4 }}>{nodesData === null ? "Admin access required" : "Loading..."}</TableCell></TableRow>
              )}
            </TableBody>
          </Table>
        </TableContainer>
      </Paper>

      {/* Ticks stored bar chart */}
      {nodes.length > 0 && (
        <Paper sx={{ p: 2, border: "1px solid #1E293B" }}>
          <Typography variant="body2" color="text.secondary" sx={{ mb: 1 }}>Ticks Stored per Node</Typography>
          <ResponsiveContainer width="100%" height={200}>
            <BarChart data={barData}>
              <XAxis dataKey="name" tick={{ fontSize: 11 }} stroke="#475569" />
              <YAxis tick={{ fontSize: 11 }} stroke="#475569" />
              <Tooltip contentStyle={tooltipStyle} />
              <Bar dataKey="ticks" radius={[4, 4, 0, 0]}>
                {barData.map((entry, i) => <Cell key={i} fill={barColor[entry.state] ?? "#5C6BC0"} />)}
              </Bar>
            </BarChart>
          </ResponsiveContainer>
        </Paper>
      )}

      {/* Ingestion history line chart (per-node time series) */}
      {timeSeries.length > 1 && (
        <Paper sx={{ p: 2, border: "1px solid #1E293B" }}>
          <Typography variant="body2" color="text.secondary" sx={{ mb: 1 }}>Ticks Ingested (History)</Typography>
          <ResponsiveContainer width="100%" height={250}>
            <LineChart data={timeSeries}>
              <XAxis dataKey="timestamp_ms" tickFormatter={formatTime} tick={{ fontSize: 10 }} stroke="#475569" />
              <YAxis tick={{ fontSize: 10 }} stroke="#475569" />
              <Tooltip labelFormatter={formatTime} contentStyle={tooltipStyle} />
              <Legend />
              {nodeIds.map((id, i) => (
                <Line key={id} type="monotone" dataKey={`ingested_${id}`} name={`Node ${id}`} stroke={NODE_COLORS[i % NODE_COLORS.length]} strokeWidth={2} dot={false} />
              ))}
            </LineChart>
          </ResponsiveContainer>
        </Paper>
      )}

      {/* Queries executed history */}
      {timeSeries.length > 1 && (
        <Paper sx={{ p: 2, border: "1px solid #1E293B" }}>
          <Typography variant="body2" color="text.secondary" sx={{ mb: 1 }}>Queries Executed (History)</Typography>
          <ResponsiveContainer width="100%" height={250}>
            <LineChart data={timeSeries}>
              <XAxis dataKey="timestamp_ms" tickFormatter={formatTime} tick={{ fontSize: 10 }} stroke="#475569" />
              <YAxis tick={{ fontSize: 10 }} stroke="#475569" />
              <Tooltip labelFormatter={formatTime} contentStyle={tooltipStyle} />
              <Legend />
              {nodeIds.map((id, i) => (
                <Line key={id} type="monotone" dataKey={`queries_${id}`} name={`Node ${id}`} stroke={NODE_COLORS[i % NODE_COLORS.length]} strokeWidth={2} dot={false} />
              ))}
            </LineChart>
          </ResponsiveContainer>
        </Paper>
      )}

      {/* Ingest latency history */}
      {timeSeries.length > 1 && (
        <Paper sx={{ p: 2, border: "1px solid #1E293B" }}>
          <Typography variant="body2" color="text.secondary" sx={{ mb: 1 }}>Ingest Latency (ns) (History)</Typography>
          <ResponsiveContainer width="100%" height={250}>
            <LineChart data={timeSeries}>
              <XAxis dataKey="timestamp_ms" tickFormatter={formatTime} tick={{ fontSize: 10 }} stroke="#475569" />
              <YAxis tick={{ fontSize: 10 }} stroke="#475569" />
              <Tooltip labelFormatter={formatTime} contentStyle={tooltipStyle} />
              <Legend />
              {nodeIds.map((id, i) => (
                <Line key={id} type="monotone" dataKey={`latency_${id}`} name={`Node ${id}`} stroke={NODE_COLORS[i % NODE_COLORS.length]} strokeWidth={2} dot={false} />
              ))}
            </LineChart>
          </ResponsiveContainer>
        </Paper>
      )}
    </Box>
  );
}
