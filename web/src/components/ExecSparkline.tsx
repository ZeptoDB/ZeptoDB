"use client";
import { Box, Tooltip, Typography } from "@mui/material";

interface Props {
  times: number[]; // execution times in μs
  width?: number;
  height?: number;
}

export default function ExecSparkline({ times, width = 120, height = 24 }: Props) {
  if (times.length < 2) return null;

  const max = Math.max(...times);
  const min = Math.min(...times);
  const range = max - min || 1;
  const pad = 2;
  const w = width - pad * 2;
  const h = height - pad * 2;

  const points = times.map((t, i) => {
    const x = pad + (i / (times.length - 1)) * w;
    const y = pad + h - ((t - min) / range) * h;
    return `${x},${y}`;
  }).join(" ");

  const last = times[times.length - 1];
  const label = last >= 1000 ? `${(last / 1000).toFixed(1)}ms` : `${last}μs`;

  return (
    <Tooltip title={`Last ${times.length} queries — latest: ${label}`}>
      <Box sx={{ display: "inline-flex", alignItems: "center", gap: 0.5 }}>
        <svg width={width} height={height} style={{ display: "block" }}>
          <polyline points={points} fill="none" stroke="#4D7CFF" strokeWidth={1.5} strokeLinejoin="round" strokeLinecap="round" />
        </svg>
        <Typography variant="caption" sx={{ fontSize: 10, color: "text.secondary", fontFamily: "'JetBrains Mono', monospace" }}>
          {label}
        </Typography>
      </Box>
    </Tooltip>
  );
}
