"use client";
import { Box, Paper, Typography, Grid, Chip, IconButton, Tooltip, LinearProgress } from "@mui/material";
import ArrowBackIcon from "@mui/icons-material/ArrowBack";
import { useQuery } from "@tanstack/react-query";
import { fetchTenant } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import { useParams, useRouter } from "next/navigation";
import { useRef } from "react";

const MONO = { fontFamily: "'JetBrains Mono', monospace" };

/** Circular gauge with animated arc */
function Gauge({ label, value, max, unit = "", color = "#4D7CFF" }: {
  label: string; value: number; max: number; unit?: string; color?: string;
}) {
  const pct = max > 0 ? Math.min(value / max, 1) : 0;
  const r = 54;
  const circ = 2 * Math.PI * r;
  const dashOffset = circ * (1 - pct);
  const overQuota = max > 0 && value >= max;

  return (
    <Paper sx={{ p: 2.5, border: "1px solid rgba(255, 255, 255, 0.08)", textAlign: "center" }}>
      <Typography variant="caption" color="text.secondary" sx={{ fontWeight: 600, textTransform: "uppercase", letterSpacing: "0.05em", fontSize: 10 }}>
        {label}
      </Typography>
      <Box sx={{ display: "flex", justifyContent: "center", my: 1.5 }}>
        <svg width={130} height={130} viewBox="0 0 130 130">
          <circle cx={65} cy={65} r={r} fill="none" stroke="rgba(255,255,255,0.06)" strokeWidth={10} />
          <circle cx={65} cy={65} r={r} fill="none"
            stroke={overQuota ? "#FF1744" : color} strokeWidth={10}
            strokeLinecap="round" strokeDasharray={circ} strokeDashoffset={dashOffset}
            transform="rotate(-90 65 65)"
            style={{ transition: "stroke-dashoffset 0.6s ease" }}
          />
          {overQuota && (
            <circle cx={65} cy={65} r={r} fill="none"
              stroke="#FF1744" strokeWidth={10} opacity={0.15}
              strokeLinecap="round" strokeDasharray={circ} strokeDashoffset={0}
              transform="rotate(-90 65 65)"
              style={{ animation: "pulse-ring 2s ease-in-out infinite" }}
            />
          )}
          <text x={65} y={58} textAnchor="middle" fill="#fff" fontSize={20} fontWeight={700}
            fontFamily="'JetBrains Mono', monospace">
            {value.toLocaleString()}
          </text>
          <text x={65} y={78} textAnchor="middle" fill="rgba(255,255,255,0.45)" fontSize={11}>
            / {max > 0 ? max.toLocaleString() : "∞"}{unit && ` ${unit}`}
          </text>
          <text x={65} y={96} textAnchor="middle" fill={overQuota ? "#FF1744" : color} fontSize={12} fontWeight={600}>
            {max > 0 ? `${(pct * 100).toFixed(0)}%` : "—"}
          </text>
        </svg>
      </Box>
    </Paper>
  );
}

function InfoCard({ label, value, color }: { label: string; value: string; color?: string }) {
  return (
    <Paper sx={{ p: 2.5, border: "1px solid rgba(255, 255, 255, 0.08)" }}>
      <Typography variant="caption" color="text.secondary" sx={{ fontWeight: 600, textTransform: "uppercase", letterSpacing: "0.04em", fontSize: 10 }}>{label}</Typography>
      <Typography variant="h6" sx={{ fontWeight: 700, ...MONO, mt: 0.5, color: color ?? "text.primary" }}>{value}</Typography>
    </Paper>
  );
}

/** Mini sparkline from query rate history */
function RateSparkline({ history, color = "#4D7CFF" }: { history: number[]; color?: string }) {
  if (history.length < 2) return null;
  const max = Math.max(...history, 1);
  const w = 160;
  const h = 32;
  const pad = 2;
  const iw = w - pad * 2;
  const ih = h - pad * 2;
  const points = history.map((v, i) => {
    const x = pad + (i / (history.length - 1)) * iw;
    const y = pad + ih - (v / max) * ih;
    return `${x},${y}`;
  }).join(" ");
  const last = history[history.length - 1];

  return (
    <Box sx={{ display: "inline-flex", alignItems: "center", gap: 0.5 }}>
      <svg width={w} height={h}>
        <polyline points={points} fill="none" stroke={color} strokeWidth={1.5} strokeLinejoin="round" strokeLinecap="round" />
      </svg>
      <Typography variant="caption" sx={{ ...MONO, fontSize: 10, color: "text.secondary" }}>
        {last}/s
      </Typography>
    </Box>
  );
}

export default function TenantDetailPage() {
  const { auth } = useAuth();
  const router = useRouter();
  const params = useParams<{ id: string }>();
  const tenantId = params.id;

  // Track query rate over time
  const prevRef = useRef<{ total: number; ts: number } | null>(null);
  const rateHistoryRef = useRef<number[]>([]);

  const { data: tenant } = useQuery({
    queryKey: ["tenant", tenantId],
    queryFn: () => fetchTenant(tenantId, auth?.apiKey),
    refetchInterval: 3000,
  });

  // Compute query rate delta
  if (tenant?.usage) {
    const now = Date.now();
    const total = tenant.usage.total_queries ?? 0;
    if (prevRef.current) {
      const dt = (now - prevRef.current.ts) / 1000;
      if (dt > 0.5) {
        const rate = Math.max(0, Math.round((total - prevRef.current.total) / dt));
        const hist = rateHistoryRef.current;
        hist.push(rate);
        if (hist.length > 30) hist.shift();
        prevRef.current = { total, ts: now };
      }
    } else {
      prevRef.current = { total, ts: now };
    }
  }

  if (tenant === null) {
    return (
      <Box sx={{ display: "flex", flexDirection: "column", gap: 2 }}>
        <Box sx={{ display: "flex", alignItems: "center", gap: 1 }}>
          <IconButton size="small" onClick={() => router.push("/tenants")}><ArrowBackIcon /></IconButton>
          <Typography variant="h6">Tenant: {tenantId}</Typography>
        </Box>
        <Paper sx={{ p: 4, textAlign: "center", border: "1px solid rgba(255, 255, 255, 0.08)" }}>
          <Typography color="text.secondary">Tenant not found or admin access required</Typography>
        </Paper>
      </Box>
    );
  }

  if (!tenant) {
    return (
      <Box sx={{ p: 4, textAlign: "center" }}>
        <Typography color="text.secondary">Loading...</Typography>
      </Box>
    );
  }

  const usage = tenant.usage ?? { active_queries: 0, total_queries: 0, rejected_queries: 0 };
  const maxConcurrent = tenant.max_concurrent_queries ?? 0;
  const rejectionRate = usage.total_queries > 0 ? (usage.rejected_queries / usage.total_queries) * 100 : 0;
  const concurrencyPct = maxConcurrent > 0 ? (usage.active_queries / maxConcurrent) * 100 : 0;

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 3 }}>
      <style>{`@keyframes pulse-ring{0%,100%{opacity:.1}50%{opacity:.25}}`}</style>

      {/* Header */}
      <Box sx={{ display: "flex", alignItems: "center", gap: 1 }}>
        <Tooltip title="Back to Tenants">
          <IconButton size="small" onClick={() => router.push("/tenants")}><ArrowBackIcon /></IconButton>
        </Tooltip>
        <Typography variant="h6">Tenant: {tenant.name || tenantId}</Typography>
        <Chip label={tenantId} size="small" variant="outlined" sx={{ ml: 1, ...MONO, fontSize: 11 }} />
      </Box>

      {/* Quick status bar */}
      <Paper sx={{ p: 2, border: "1px solid rgba(255, 255, 255, 0.08)", display: "flex", alignItems: "center", gap: 3, flexWrap: "wrap" }}>
        <Box sx={{ display: "flex", alignItems: "center", gap: 1 }}>
          <Box sx={{ width: 8, height: 8, borderRadius: "50%", bgcolor: concurrencyPct >= 100 ? "error.main" : concurrencyPct >= 80 ? "warning.main" : "success.main" }} />
          <Typography variant="caption" sx={{ fontWeight: 600, textTransform: "uppercase", letterSpacing: "0.03em", fontSize: 10 }}>
            Concurrency: {usage.active_queries} / {maxConcurrent || "∞"}
          </Typography>
        </Box>
        {maxConcurrent > 0 && (
          <LinearProgress variant="determinate" value={Math.min(concurrencyPct, 100)}
            color={concurrencyPct >= 100 ? "error" : concurrencyPct >= 80 ? "warning" : "primary"}
            sx={{ flex: 1, minWidth: 120, height: 6, borderRadius: 3, bgcolor: "rgba(255,255,255,0.06)" }} />
        )}
        {rejectionRate > 0 && (
          <Chip label={`${rejectionRate.toFixed(1)}% rejected`} size="small" color="error" variant="outlined" sx={{ ...MONO, fontSize: 10 }} />
        )}
        {rateHistoryRef.current.length > 1 && (
          <RateSparkline history={rateHistoryRef.current} color="#00F5D4" />
        )}
      </Paper>

      {/* Info cards */}
      <Grid container spacing={2}>
        <Grid size={{ xs: 6, md: 3 }}><InfoCard label="Tenant ID" value={tenantId} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><InfoCard label="Name" value={tenant.name || "—"} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}><InfoCard label="Namespace" value={tenant.table_namespace || "global"} /></Grid>
        <Grid size={{ xs: 6, md: 3 }}>
          <InfoCard label="Concurrent Limit" value={maxConcurrent > 0 ? String(maxConcurrent) : "∞"}
            color={concurrencyPct >= 100 ? "#FF1744" : undefined} />
        </Grid>
      </Grid>

      {/* Usage gauges */}
      <Typography variant="body2" color="text.secondary" sx={{ fontWeight: 600 }}>Quota vs Actual Usage</Typography>
      <Grid container spacing={2}>
        <Grid size={{ xs: 12, sm: 4 }}>
          <Gauge label="Active Queries" value={usage.active_queries} max={maxConcurrent} color="#4D7CFF" />
        </Grid>
        <Grid size={{ xs: 12, sm: 4 }}>
          <Gauge label="Total Queries" value={usage.total_queries} max={0} color="#00E676" />
        </Grid>
        <Grid size={{ xs: 12, sm: 4 }}>
          <Gauge label="Rejected Queries" value={usage.rejected_queries} max={usage.total_queries || 1} color="#FF1744" />
        </Grid>
      </Grid>

      {/* Stats summary */}
      <Grid container spacing={2}>
        <Grid size={{ xs: 6, md: 4 }}>
          <InfoCard label="Total Queries" value={usage.total_queries.toLocaleString()} color="#00E676" />
        </Grid>
        <Grid size={{ xs: 6, md: 4 }}>
          <InfoCard label="Rejected" value={usage.rejected_queries.toLocaleString()}
            color={usage.rejected_queries > 0 ? "#FF1744" : undefined} />
        </Grid>
        <Grid size={{ xs: 6, md: 4 }}>
          <InfoCard label="Rejection Rate" value={rejectionRate > 0 ? `${rejectionRate.toFixed(2)}%` : "0%"}
            color={rejectionRate > 5 ? "#FF1744" : rejectionRate > 1 ? "#FFB300" : undefined} />
        </Grid>
      </Grid>
    </Box>
  );
}
