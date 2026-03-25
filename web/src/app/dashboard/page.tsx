"use client";
import { Box, Paper, Typography, Grid } from "@mui/material";
import { useQuery } from "@tanstack/react-query";
import { fetchStats } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import { LineChart, Line, XAxis, YAxis, Tooltip, ResponsiveContainer } from "recharts";
import { useRef, useEffect, useState } from "react";

function StatCard({ label, value, color = "primary.main" }: { label: string; value: string | number; color?: string }) {
  return (
    <Paper sx={{ p: 2.5, border: "1px solid #1E293B" }}>
      <Typography variant="caption" color="text.secondary">{label}</Typography>
      <Typography variant="h5" sx={{ fontWeight: 700, color, fontFamily: "'JetBrains Mono', monospace" }}>{typeof value === "number" ? value.toLocaleString() : value}</Typography>
    </Paper>
  );
}

export default function DashboardPage() {
  const { auth } = useAuth();
  const { data } = useQuery({ queryKey: ["stats"], queryFn: () => fetchStats(auth?.apiKey), refetchInterval: 2000 });
  const history = useRef<{ time: string; rate: number }[]>([]);
  const prevIngested = useRef(0);
  const [chartData, setChartData] = useState<{ time: string; rate: number }[]>([]);

  useEffect(() => {
    if (!data) return;
    const now = new Date().toLocaleTimeString();
    const rate = Math.max(0, (data.ticks_ingested ?? 0) - prevIngested.current);
    prevIngested.current = data.ticks_ingested ?? 0;
    history.current = [...history.current.slice(-29), { time: now, rate }];
    setChartData([...history.current]);
  }, [data]);

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 3 }}>
      <Typography variant="h6">Dashboard</Typography>
      <Grid container spacing={2}>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Ticks Ingested" value={data?.ticks_ingested ?? 0} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Ticks Stored" value={data?.ticks_stored ?? 0} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Queries Executed" value={data?.queries_executed ?? 0} color="secondary.main" /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Ticks Dropped" value={data?.ticks_dropped ?? 0} color="error.main" /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Total Rows Scanned" value={data?.total_rows_scanned ?? 0} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Partitions Created" value={data?.partitions_created ?? 0} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Last Ingest Latency" value={data?.last_ingest_latency_ns != null ? `${data.last_ingest_latency_ns} ns` : "—"} /></Grid>
      </Grid>
      <Paper sx={{ p: 2, border: "1px solid #1E293B" }}>
        <Typography variant="body2" color="text.secondary" sx={{ mb: 1 }}>Ingestion Rate (ticks/2s)</Typography>
        <ResponsiveContainer width="100%" height={250}>
          <LineChart data={chartData}>
            <XAxis dataKey="time" tick={{ fontSize: 10 }} stroke="#475569" />
            <YAxis tick={{ fontSize: 10 }} stroke="#475569" />
            <Tooltip contentStyle={{ backgroundColor: "#111827", border: "1px solid #1E293B", borderRadius: 8, fontSize: 12 }} />
            <Line type="monotone" dataKey="rate" stroke="#5C6BC0" strokeWidth={2} dot={false} />
          </LineChart>
        </ResponsiveContainer>
      </Paper>
    </Box>
  );
}
