"use client";
import { Box, Paper, Typography, Grid, useTheme } from "@mui/material";
import { useQuery } from "@tanstack/react-query";
import { fetchStats } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import { LineChart, Line, XAxis, YAxis, Tooltip, ResponsiveContainer } from "recharts";
import { useRef, useEffect, useState } from "react";

import { Card } from "@mui/material";

function StatCard({ label, value, color = "primary.main" }: { label: string; value: string | number; color?: string }) {
  return (
    <Card sx={{ p: 3 }}>
      <Typography variant="caption" color="text.secondary" sx={{ textTransform: "uppercase", letterSpacing: "0.05em", fontWeight: 600 }}>{label}</Typography>
      <Typography variant="h4" sx={{ mt: 1, fontWeight: 700, color, fontFamily: "'JetBrains Mono', monospace", letterSpacing: "-0.02em" }}>{typeof value === "number" ? value.toLocaleString() : value}</Typography>
    </Card>
  );
}

export default function DashboardPage() {
  const theme = useTheme();
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
    <Box sx={{ display: "flex", flexDirection: "column", gap: 4 }}>
      <Typography variant="h4" sx={{ fontWeight: 700, letterSpacing: "-0.02em" }}>Dashboard Overview</Typography>
      <Grid container spacing={2}>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Ticks Ingested" value={data?.ticks_ingested ?? 0} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Ticks Stored" value={data?.ticks_stored ?? 0} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Queries Executed" value={data?.queries_executed ?? 0} color="secondary.main" /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Ticks Dropped" value={data?.ticks_dropped ?? 0} color="error.main" /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Total Rows Scanned" value={data?.total_rows_scanned ?? 0} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Partitions Created" value={data?.partitions_created ?? 0} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><StatCard label="Last Ingest Latency" value={data?.last_ingest_latency_ns != null ? `${data.last_ingest_latency_ns} ns` : "—"} /></Grid>
      </Grid>
      <Card sx={{ p: 3 }}>
        <Typography variant="subtitle2" color="text.secondary" sx={{ mb: 3, textTransform: "uppercase", letterSpacing: "0.05em", fontWeight: 600 }}>Ingestion Rate (ticks/2s)</Typography>
        <ResponsiveContainer width="100%" height={280}>
          <LineChart data={chartData}>
            <XAxis dataKey="time" tick={{ fontSize: 11, fill: theme.palette.text.secondary }} stroke={theme.palette.divider} />
            <YAxis tick={{ fontSize: 11, fill: theme.palette.text.secondary }} stroke={theme.palette.divider} />
            <Tooltip 
              contentStyle={{ backgroundColor: theme.palette.background.paper, border: `1px solid ${theme.palette.divider}`, borderRadius: 8, fontSize: 12, color: theme.palette.text.primary, WebkitBackdropFilter: "blur(12px)" }} 
              itemStyle={{ color: theme.palette.primary.main, fontWeight: 700 }} 
            />
            <Line type="monotone" dataKey="rate" stroke={theme.palette.primary.main} strokeWidth={3} dot={false} activeDot={{ r: 6, fill: theme.palette.primary.main, stroke: theme.palette.background.paper, strokeWidth: 2 }} 
              style={{ filter: `drop-shadow(0px 4px 12px ${theme.palette.primary.main}80)` }}
            />
          </LineChart>
        </ResponsiveContainer>
      </Card>
    </Box>
  );
}
