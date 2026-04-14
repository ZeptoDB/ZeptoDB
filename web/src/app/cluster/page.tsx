"use client";
import { Box, Paper, Typography, Grid, Chip, Table, TableHead, TableRow, TableCell, TableBody, TableContainer, Tooltip as MuiTooltip, LinearProgress } from "@mui/material";
import { useQuery } from "@tanstack/react-query";
import { fetchNodes, fetchCluster, fetchMetricsHistory, fetchRebalanceStatus, fetchRebalanceHistory } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import { BarChart, Bar, LineChart, Line, XAxis, YAxis, Tooltip, ResponsiveContainer, Cell, Legend, PieChart, Pie } from "recharts";

interface NodeInfo {
  id: number;
  host: string;
  port: number;
  state: string;
  ticks_ingested: number;
  ticks_stored: number;
  queries_executed: number;
}

interface ClusterInfo {
  mode: string;
  node_count: number;
  partitions_created: number;
  partitions_evicted: number;
  ticks_ingested: number;
  ticks_stored: number;
  ticks_dropped: number;
  queries_executed: number;
  total_rows_scanned: number;
  last_ingest_latency_ns: number;
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

interface RebalanceInfo {
  state: string;
  total_moves: number;
  completed_moves: number;
  failed_moves: number;
  current_symbol: string;
}

interface RebalanceHistoryEntry {
  action: string;
  node_id: number;
  total_moves: number;
  completed_moves: number;
  failed_moves: number;
  start_time_ms: number;
  duration_ms: number;
  cancelled: boolean;
}

const stateColor: Record<string, "success" | "warning" | "error" | "default" | "info"> = {
  ACTIVE: "success", SUSPECT: "warning", DEAD: "error", JOINING: "info", LEAVING: "default",
};

const barColor: Record<string, string> = {
  ACTIVE: "#00E676", SUSPECT: "#FFB300", DEAD: "#FF1744", JOINING: "#2979FF", LEAVING: "#bdbdbd",
};

const NODE_COLORS = ["#4D7CFF", "#00E676", "#FFB300", "#FF1744", "#00F5D4", "#8EACFF", "#ff7043", "#8d6e63"];

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

function formatNs(ns: number) {
  if (ns >= 1_000_000) return `${(ns / 1_000_000).toFixed(1)}ms`;
  if (ns >= 1_000) return `${(ns / 1_000).toFixed(0)}μs`;
  return `${ns}ns`;
}

/** Ring topology with animated pulse on active nodes and health arcs */
function NodeTopology({ nodes }: { nodes: NodeInfo[] }) {
  const size = 300;
  const cx = size / 2;
  const cy = size / 2;
  const radius = 110;

  if (nodes.length === 0) return null;

  // Single node: centered layout
  if (nodes.length === 1) {
    const n = nodes[0];
    const fill = barColor[n.state] ?? "#5C6BC0";
    const pct = n.ticks_ingested > 0 ? Math.min(n.ticks_stored / n.ticks_ingested, 1) : 0;
    const arcR = 32;
    const circ = 2 * Math.PI * arcR;
    return (
      <Paper sx={{ p: 2, border: "1px solid rgba(255, 255, 255, 0.08)" }}>
        <Typography variant="body2" color="text.secondary" sx={{ mb: 1, fontWeight: 600 }}>Node Topology</Typography>
        <Box sx={{ display: "flex", justifyContent: "center" }}>
          <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`}>
            <style>{`@keyframes pulse{0%,100%{opacity:.12}50%{opacity:.3}}`}</style>
            {n.state === "ACTIVE" && <circle cx={cx} cy={cy} r={44} fill={fill} style={{ animation: "pulse 2s ease-in-out infinite" }} />}
            <circle cx={cx} cy={cy} r={arcR} fill="none" stroke="rgba(255,255,255,0.06)" strokeWidth={6} />
            <circle cx={cx} cy={cy} r={arcR} fill="none" stroke={fill} strokeWidth={6}
              strokeLinecap="round" strokeDasharray={circ} strokeDashoffset={circ * (1 - pct)}
              transform={`rotate(-90 ${cx} ${cy})`} opacity={0.7} />
            <circle cx={cx} cy={cy} r={24} fill={fill} opacity={0.9} />
            <text x={cx} y={cy + 1} textAnchor="middle" dominantBaseline="central"
              fill="#fff" fontSize={14} fontWeight={700} fontFamily="'JetBrains Mono', monospace">{n.id}</text>
            <text x={cx} y={cy + 50} textAnchor="middle" fill="rgba(255,255,255,0.6)" fontSize={10}>{n.host}:{n.port}</text>
            <text x={cx} y={cy + 64} textAnchor="middle" fill={fill} fontSize={10} fontWeight={600}>{n.state}</text>
            <text x={cx} y={cy + 78} textAnchor="middle" fill="rgba(255,255,255,0.4)" fontSize={9}>
              {(pct * 100).toFixed(0)}% stored
            </text>
          </svg>
        </Box>
      </Paper>
    );
  }

  return (
    <Paper sx={{ p: 2, border: "1px solid rgba(255, 255, 255, 0.08)" }}>
      <Typography variant="body2" color="text.secondary" sx={{ mb: 1, fontWeight: 600 }}>Node Topology</Typography>
      <Box sx={{ display: "flex", justifyContent: "center" }}>
        <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`}>
          <style>{`@keyframes pulse{0%,100%{opacity:.12}50%{opacity:.3}}`}</style>
          {/* Ring connections */}
          {nodes.map((_, i) => {
            const next = (i + 1) % nodes.length;
            const a1 = (2 * Math.PI * i) / nodes.length - Math.PI / 2;
            const a2 = (2 * Math.PI * next) / nodes.length - Math.PI / 2;
            return (
              <line key={`edge-${i}`}
                x1={cx + radius * Math.cos(a1)} y1={cy + radius * Math.sin(a1)}
                x2={cx + radius * Math.cos(a2)} y2={cy + radius * Math.sin(a2)}
                stroke="rgba(255,255,255,0.08)" strokeWidth={1.5} strokeDasharray="4 4"
              />
            );
          })}
          {/* Nodes with health arc */}
          {nodes.map((n, i) => {
            const angle = (2 * Math.PI * i) / nodes.length - Math.PI / 2;
            const x = cx + radius * Math.cos(angle);
            const y = cy + radius * Math.sin(angle);
            const fill = barColor[n.state] ?? "#5C6BC0";
            const pct = n.ticks_ingested > 0 ? Math.min(n.ticks_stored / n.ticks_ingested, 1) : 0;
            const arcR = 24;
            const circ = 2 * Math.PI * arcR;
            return (
              <g key={n.id}>
                {n.state === "ACTIVE" && <circle cx={x} cy={y} r={30} fill={fill} style={{ animation: "pulse 2s ease-in-out infinite" }} />}
                {/* Health arc background */}
                <circle cx={x} cy={y} r={arcR} fill="none" stroke="rgba(255,255,255,0.06)" strokeWidth={5} />
                {/* Health arc fill */}
                <circle cx={x} cy={y} r={arcR} fill="none" stroke={fill} strokeWidth={5}
                  strokeLinecap="round" strokeDasharray={circ} strokeDashoffset={circ * (1 - pct)}
                  transform={`rotate(-90 ${x} ${y})`} opacity={0.6} />
                <circle cx={x} cy={y} r={17} fill={fill} opacity={0.9} />
                <text x={x} y={y + 1} textAnchor="middle" dominantBaseline="central"
                  fill="#fff" fontSize={11} fontWeight={700} fontFamily="'JetBrains Mono', monospace">{n.id}</text>
                <text x={x} y={y + 36} textAnchor="middle" fill="rgba(255,255,255,0.6)" fontSize={8}>
                  {n.host}:{n.port}
                </text>
                <text x={x} y={y + 48} textAnchor="middle" fill={fill} fontSize={8} fontWeight={600}>{n.state}</text>
              </g>
            );
          })}
        </svg>
      </Box>
    </Paper>
  );
}

/** Partition distribution pie chart */
function PartitionDistribution({ nodes }: { nodes: NodeInfo[] }) {
  const data = nodes.map((n, i) => ({
    name: `Node ${n.id}`,
    value: n.ticks_stored,
    fill: NODE_COLORS[i % NODE_COLORS.length],
  }));
  const total = data.reduce((s, d) => s + d.value, 0);
  if (total === 0) return null;

  return (
    <Paper sx={{ p: 2, border: "1px solid rgba(255, 255, 255, 0.08)" }}>
      <Typography variant="body2" color="text.secondary" sx={{ mb: 1, fontWeight: 600 }}>Data Distribution</Typography>
      <ResponsiveContainer width="100%" height={240}>
        <PieChart>
          <Pie data={data} dataKey="value" nameKey="name" cx="50%" cy="50%"
            innerRadius={55} outerRadius={85} paddingAngle={2} strokeWidth={0}>
            {data.map((d, i) => <Cell key={i} fill={d.fill} />)}
          </Pie>
          <Tooltip contentStyle={tooltipStyle} />
          <Legend wrapperStyle={{ fontSize: 11 }} />
        </PieChart>
      </ResponsiveContainer>
    </Paper>
  );
}

function RebalanceStatus({ status }: { status: RebalanceInfo }) {
  if (status.state === "IDLE" && status.total_moves === 0) return null;

  const isActive = status.state === "RUNNING" || status.state === "PAUSED" || status.state === "CANCELLING";
  const progress = status.total_moves > 0
    ? ((status.completed_moves + status.failed_moves) / status.total_moves) * 100
    : 0;

  const stateChipColor: Record<string, "info" | "warning" | "error" | "success" | "default"> = {
    RUNNING: "info", PAUSED: "warning", CANCELLING: "error", IDLE: "success",
  };

  return (
    <Paper sx={{ p: 2, border: "1px solid", borderColor: isActive ? "info.main" : "rgba(255, 255, 255, 0.08)" }}>
      <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between", mb: 1.5 }}>
        <Typography variant="body2" color="text.secondary" sx={{ fontWeight: 600 }}>Rebalance Progress</Typography>
        <Chip label={status.state} size="small" variant="outlined"
          color={stateChipColor[status.state] ?? "default"}
          sx={{ ...MONO, fontSize: 10 }} />
      </Box>
      {isActive && (
        <LinearProgress variant="determinate" value={progress}
          sx={{ height: 8, borderRadius: 4, mb: 1.5, bgcolor: "rgba(255,255,255,0.06)" }} />
      )}
      <Box sx={{ display: "flex", gap: 3 }}>
        <Box>
          <Typography variant="caption" color="text.secondary" sx={{ fontSize: 10 }}>Completed</Typography>
          <Typography variant="body2" sx={{ ...MONO, fontWeight: 700 }}>
            {status.completed_moves} / {status.total_moves}
          </Typography>
        </Box>
        {status.failed_moves > 0 && (
          <Box>
            <Typography variant="caption" color="text.secondary" sx={{ fontSize: 10 }}>Failed</Typography>
            <Typography variant="body2" sx={{ ...MONO, fontWeight: 700, color: "error.main" }}>
              {status.failed_moves}
            </Typography>
          </Box>
        )}
        {status.current_symbol && (
          <Box>
            <Typography variant="caption" color="text.secondary" sx={{ fontSize: 10 }}>Current Symbol</Typography>
            <Typography variant="body2" sx={{ ...MONO, fontWeight: 700 }}>
              {status.current_symbol}
            </Typography>
          </Box>
        )}
      </Box>
    </Paper>
  );
}

function RebalanceHistory({ entries }: { entries: RebalanceHistoryEntry[] }) {
  if (entries.length === 0) return null;

  return (
    <Paper sx={{ border: "1px solid rgba(255, 255, 255, 0.08)" }}>
      <Box sx={{ px: 2, py: 1.5, borderBottom: "1px solid rgba(255, 255, 255, 0.08)" }}>
        <Typography variant="body2" color="text.secondary" sx={{ fontWeight: 600 }}>Rebalance History</Typography>
      </Box>
      <TableContainer>
        <Table size="small">
          <TableHead>
            <TableRow>
              <TableCell sx={{ fontWeight: 600 }}>Time</TableCell>
              <TableCell sx={{ fontWeight: 600 }}>Action</TableCell>
              <TableCell sx={{ fontWeight: 600 }} align="right">Node</TableCell>
              <TableCell sx={{ fontWeight: 600 }} align="right">Moves</TableCell>
              <TableCell sx={{ fontWeight: 600 }} align="right">Failed</TableCell>
              <TableCell sx={{ fontWeight: 600 }} align="right">Duration</TableCell>
              <TableCell sx={{ fontWeight: 600 }}>Result</TableCell>
            </TableRow>
          </TableHead>
          <TableBody>
            {[...entries].reverse().map((e, i) => (
              <TableRow key={i} hover>
                <TableCell sx={{ ...MONO, fontSize: 12 }}>
                  {new Date(e.start_time_ms).toLocaleString([], { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit" })}
                </TableCell>
                <TableCell sx={{ ...MONO, fontSize: 12 }}>{e.action}</TableCell>
                <TableCell align="right" sx={{ ...MONO, fontSize: 12 }}>{e.node_id || "—"}</TableCell>
                <TableCell align="right" sx={{ ...MONO, fontSize: 12 }}>{e.completed_moves}/{e.total_moves}</TableCell>
                <TableCell align="right" sx={{ ...MONO, fontSize: 12, color: e.failed_moves > 0 ? "error.main" : "text.secondary" }}>{e.failed_moves}</TableCell>
                <TableCell align="right" sx={{ ...MONO, fontSize: 12 }}>{e.duration_ms < 1000 ? `${e.duration_ms}ms` : `${(e.duration_ms / 1000).toFixed(1)}s`}</TableCell>
                <TableCell>
                  <Chip size="small" variant="outlined"
                    label={e.cancelled ? "CANCELLED" : e.failed_moves > 0 ? "PARTIAL" : "OK"}
                    color={e.cancelled ? "warning" : e.failed_moves > 0 ? "error" : "success"}
                    sx={{ ...MONO, fontSize: 9 }} />
                </TableCell>
              </TableRow>
            ))}
          </TableBody>
        </Table>
      </TableContainer>
    </Paper>
  );
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

  const { data: rebalanceData } = useQuery({
    queryKey: ["rebalance-status"],
    queryFn: () => fetchRebalanceStatus(auth?.apiKey),
    refetchInterval: 2000,
  });

  const { data: rebalanceHistory } = useQuery({
    queryKey: ["rebalance-history"],
    queryFn: () => fetchRebalanceHistory(auth?.apiKey),
    refetchInterval: 5000,
  });

  const nodes: NodeInfo[] = nodesData?.nodes ?? [];
  const cl = cluster as ClusterInfo | null;
  const activeCount = nodes.filter((n) => n.state === "ACTIVE").length;
  const totalTicks = nodes.reduce((s, n) => s + (n.ticks_ingested ?? 0), 0);
  const totalQueries = nodes.reduce((s, n) => s + (n.queries_executed ?? 0), 0);

  const barData = nodes.map((n) => ({ name: `Node ${n.id}`, ticks: n.ticks_stored, state: n.state }));

  const metrics: MetricsPoint[] = metricsRaw ?? [];
  const nodeIds = [...new Set(metrics.map((m) => m.node_id))].sort();
  const timeSeries = buildTimeSeries(metrics, nodeIds);

  const dropRate = cl && cl.ticks_ingested > 0 ? ((cl.ticks_dropped / cl.ticks_ingested) * 100) : 0;

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 3 }}>
      <Box sx={{ display: "flex", alignItems: "center", gap: 2 }}>
        <Typography variant="h6">Cluster Overview</Typography>
        {cl && (
          <Chip label={cl.mode} size="small" variant="outlined"
            color={cl.mode === "cluster" ? "primary" : "default"}
            sx={{ ...MONO, fontSize: 11, textTransform: "uppercase" }} />
        )}
      </Box>

      {/* Summary cards — row 1: core counts */}
      <Grid container spacing={2}>
        <Grid size={{ xs: 6, md: 2.4 }}>
          <StatCard label="Nodes" value={`${activeCount} / ${nodes.length}`}
            color={activeCount === nodes.length ? "success.main" : "warning.main"} />
        </Grid>
        <Grid size={{ xs: 6, md: 2.4 }}>
          <StatCard label="Ticks Ingested" value={totalTicks} />
        </Grid>
        <Grid size={{ xs: 6, md: 2.4 }}>
          <StatCard label="Queries" value={totalQueries} color="secondary.main" />
        </Grid>
        <Grid size={{ xs: 6, md: 2.4 }}>
          <StatCard label="Partitions" value={cl?.partitions_created ?? 0}
            sub={cl?.partitions_evicted ? `${cl.partitions_evicted} evicted` : undefined} />
        </Grid>
        <Grid size={{ xs: 6, md: 2.4 }}>
          <StatCard label="Ingest Latency" value={formatNs(cl?.last_ingest_latency_ns ?? 0)} color="info.main" />
        </Grid>
      </Grid>

      {/* Drop rate warning */}
      {dropRate > 0 && (
        <Paper sx={{ p: 1.5, px: 2, border: "1px solid", borderColor: dropRate > 1 ? "error.main" : "warning.main", display: "flex", alignItems: "center", gap: 1.5 }}>
          <Box sx={{ width: 8, height: 8, borderRadius: "50%", bgcolor: dropRate > 1 ? "error.main" : "warning.main", flexShrink: 0 }} />
          <Typography variant="body2" sx={{ ...MONO, fontSize: 12 }}>
            Drop rate: {dropRate.toFixed(2)}% ({cl!.ticks_dropped.toLocaleString()} / {cl!.ticks_ingested.toLocaleString()})
          </Typography>
        </Paper>
      )}

      {/* Rebalance progress */}
      {rebalanceData && <RebalanceStatus status={rebalanceData as RebalanceInfo} />}

      {/* Rebalance history */}
      {rebalanceHistory && (rebalanceHistory as RebalanceHistoryEntry[]).length > 0 && (
        <RebalanceHistory entries={rebalanceHistory as RebalanceHistoryEntry[]} />
      )}

      {/* Topology + Distribution */}
      {nodes.length > 0 && (
        <Grid container spacing={2}>
          <Grid size={{ xs: 12, md: 6 }}><NodeTopology nodes={nodes} /></Grid>
          <Grid size={{ xs: 12, md: 6 }}><PartitionDistribution nodes={nodes} /></Grid>
        </Grid>
      )}

      {/* Node status table with health bars */}
      <Paper sx={{ border: "1px solid rgba(255, 255, 255, 0.08)" }}>
        <Box sx={{ px: 2, py: 1.5, borderBottom: "1px solid rgba(255, 255, 255, 0.08)" }}>
          <Typography variant="body2" color="text.secondary" sx={{ fontWeight: 600 }}>Node Health</Typography>
        </Box>
        <TableContainer>
          <Table size="small">
            <TableHead>
              <TableRow>
                <TableCell sx={{ fontWeight: 600 }}>ID</TableCell>
                <TableCell sx={{ fontWeight: 600 }}>Host</TableCell>
                <TableCell sx={{ fontWeight: 600 }}>State</TableCell>
                <TableCell sx={{ fontWeight: 600 }} align="right">Ingested</TableCell>
                <TableCell sx={{ fontWeight: 600 }} align="right">Stored</TableCell>
                <TableCell sx={{ fontWeight: 600, minWidth: 140 }}>Store Ratio</TableCell>
                <TableCell sx={{ fontWeight: 600 }} align="right">Queries</TableCell>
              </TableRow>
            </TableHead>
            <TableBody>
              {nodes.map((n) => {
                const ratio = n.ticks_ingested > 0 ? n.ticks_stored / n.ticks_ingested : 1;
                const ratioColor = ratio >= 0.99 ? "success" : ratio >= 0.95 ? "warning" : "error";
                return (
                  <TableRow key={n.id} hover>
                    <TableCell sx={{ ...MONO, fontSize: 13 }}>{n.id}</TableCell>
                    <TableCell sx={{ ...MONO, fontSize: 12, color: "text.secondary" }}>{n.host}:{n.port}</TableCell>
                    <TableCell><Chip label={n.state} size="small" color={stateColor[n.state] ?? "default"} variant="outlined" /></TableCell>
                    <TableCell align="right" sx={{ ...MONO, fontSize: 13 }}>{n.ticks_ingested.toLocaleString()}</TableCell>
                    <TableCell align="right" sx={{ ...MONO, fontSize: 13 }}>{n.ticks_stored.toLocaleString()}</TableCell>
                    <TableCell>
                      <Box sx={{ display: "flex", alignItems: "center", gap: 1 }}>
                        <LinearProgress variant="determinate" value={ratio * 100}
                          color={ratioColor} sx={{ flex: 1, height: 6, borderRadius: 3, bgcolor: "rgba(255,255,255,0.06)" }} />
                        <Typography variant="caption" sx={{ ...MONO, fontSize: 10, minWidth: 36, textAlign: "right" }}>
                          {(ratio * 100).toFixed(1)}%
                        </Typography>
                      </Box>
                    </TableCell>
                    <TableCell align="right" sx={{ ...MONO, fontSize: 13 }}>{n.queries_executed.toLocaleString()}</TableCell>
                  </TableRow>
                );
              })}
              {nodes.length === 0 && (
                <TableRow><TableCell colSpan={7} sx={{ textAlign: "center", color: "text.secondary", py: 4 }}>
                  {nodesData === null ? "Admin access required" : "Loading..."}
                </TableCell></TableRow>
              )}
            </TableBody>
          </Table>
        </TableContainer>
      </Paper>

      {/* Ticks stored bar chart */}
      {nodes.length > 0 && (
        <Paper sx={{ p: 2, border: "1px solid rgba(255, 255, 255, 0.08)" }}>
          <Typography variant="body2" color="text.secondary" sx={{ mb: 1, fontWeight: 600 }}>Ticks Stored per Node</Typography>
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

      {/* Time-series charts */}
      {timeSeries.length > 1 && (
        <Grid container spacing={2}>
          <Grid size={{ xs: 12, md: 6 }}>
            <Paper sx={{ p: 2, border: "1px solid rgba(255, 255, 255, 0.08)" }}>
              <Typography variant="body2" color="text.secondary" sx={{ mb: 1, fontWeight: 600 }}>Ticks Ingested</Typography>
              <ResponsiveContainer width="100%" height={220}>
                <LineChart data={timeSeries}>
                  <XAxis dataKey="timestamp_ms" tickFormatter={formatTime} tick={{ fontSize: 9 }} stroke="#475569" />
                  <YAxis tick={{ fontSize: 9 }} stroke="#475569" />
                  <Tooltip labelFormatter={formatTime} contentStyle={tooltipStyle} />
                  <Legend wrapperStyle={{ fontSize: 10 }} />
                  {nodeIds.map((id, i) => (
                    <Line key={id} type="monotone" dataKey={`ingested_${id}`} name={`Node ${id}`}
                      stroke={NODE_COLORS[i % NODE_COLORS.length]} strokeWidth={2} dot={false} />
                  ))}
                </LineChart>
              </ResponsiveContainer>
            </Paper>
          </Grid>
          <Grid size={{ xs: 12, md: 6 }}>
            <Paper sx={{ p: 2, border: "1px solid rgba(255, 255, 255, 0.08)" }}>
              <Typography variant="body2" color="text.secondary" sx={{ mb: 1, fontWeight: 600 }}>Queries Executed</Typography>
              <ResponsiveContainer width="100%" height={220}>
                <LineChart data={timeSeries}>
                  <XAxis dataKey="timestamp_ms" tickFormatter={formatTime} tick={{ fontSize: 9 }} stroke="#475569" />
                  <YAxis tick={{ fontSize: 9 }} stroke="#475569" />
                  <Tooltip labelFormatter={formatTime} contentStyle={tooltipStyle} />
                  <Legend wrapperStyle={{ fontSize: 10 }} />
                  {nodeIds.map((id, i) => (
                    <Line key={id} type="monotone" dataKey={`queries_${id}`} name={`Node ${id}`}
                      stroke={NODE_COLORS[i % NODE_COLORS.length]} strokeWidth={2} dot={false} />
                  ))}
                </LineChart>
              </ResponsiveContainer>
            </Paper>
          </Grid>
          <Grid size={{ xs: 12 }}>
            <Paper sx={{ p: 2, border: "1px solid rgba(255, 255, 255, 0.08)" }}>
              <Typography variant="body2" color="text.secondary" sx={{ mb: 1, fontWeight: 600 }}>Ingest Latency (ns)</Typography>
              <ResponsiveContainer width="100%" height={220}>
                <LineChart data={timeSeries}>
                  <XAxis dataKey="timestamp_ms" tickFormatter={formatTime} tick={{ fontSize: 9 }} stroke="#475569" />
                  <YAxis tick={{ fontSize: 9 }} stroke="#475569" />
                  <Tooltip labelFormatter={formatTime} contentStyle={tooltipStyle} />
                  <Legend wrapperStyle={{ fontSize: 10 }} />
                  {nodeIds.map((id, i) => (
                    <Line key={id} type="monotone" dataKey={`latency_${id}`} name={`Node ${id}`}
                      stroke={NODE_COLORS[i % NODE_COLORS.length]} strokeWidth={2} dot={false} />
                  ))}
                </LineChart>
              </ResponsiveContainer>
            </Paper>
          </Grid>
        </Grid>
      )}
    </Box>
  );
}
