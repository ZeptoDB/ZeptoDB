"use client";
import { Box, Typography, Chip } from "@mui/material";

interface Props {
  planLines: string[];
}

const COLORS: Record<string, string> = {
  Operation: "#4D7CFF",
  Path: "#00F5D4",
  Table: "#FFC107",
  Filter: "#FF5722",
  GroupBy: "#9C27B0",
  Aggregates: "#4CAF50",
  OrderBy: "#E91E63",
  Join: "#4D7CFF",
  Partitions: "#8BC34A",
  TotalRows: "#8BC34A",
  Limit: "#00BCD4",
  Offset: "#00BCD4",
  XbarBucket: "#FF9800",
};

export default function ExplainView({ planLines }: Props) {
  const parsed = planLines.map((line) => {
    const m = line.match(/^(\w+):\s+(.+)$/);
    return m ? { key: m[1], value: m[2] } : { key: "", value: line };
  });

  const operation = parsed.find((p) => p.key === "Operation")?.value ?? "Unknown";
  const path = parsed.find((p) => p.key === "Path")?.value;
  const table = parsed.find((p) => p.key === "Table")?.value;
  const stats = parsed.filter((p) => ["Partitions", "TotalRows"].includes(p.key));
  const details = parsed.filter((p) => !["Operation", "Path", "Table", "Partitions", "TotalRows"].includes(p.key) && p.key);

  return (
    <Box sx={{ p: 2, display: "flex", flexDirection: "column", gap: 2 }}>
      {/* Operation node */}
      <Box sx={{ display: "flex", flexDirection: "column", alignItems: "center", gap: 1 }}>
        <Box sx={{ px: 3, py: 1.5, borderRadius: 2, bgcolor: "rgba(77,124,255,0.15)", border: "1px solid rgba(77,124,255,0.4)", textAlign: "center" }}>
          <Typography variant="body2" sx={{ fontWeight: 700, color: "#4D7CFF", fontFamily: "'JetBrains Mono', monospace" }}>
            {operation}
          </Typography>
          {path && (
            <Typography variant="caption" sx={{ color: "#00F5D4", fontFamily: "'JetBrains Mono', monospace" }}>
              {path}
            </Typography>
          )}
        </Box>
        <Box sx={{ width: 2, height: 16, bgcolor: "divider" }} />
      </Box>

      {/* Details */}
      {details.length > 0 && (
        <Box sx={{ display: "flex", flexDirection: "column", alignItems: "center", gap: 1 }}>
          <Box sx={{ px: 3, py: 1, borderRadius: 2, bgcolor: "rgba(255,255,255,0.04)", border: "1px solid", borderColor: "divider" }}>
            {details.map((d, i) => (
              <Box key={i} sx={{ display: "flex", gap: 1, py: 0.25 }}>
                <Typography variant="caption" sx={{ fontWeight: 600, color: COLORS[d.key] ?? "text.secondary", minWidth: 80, fontFamily: "'JetBrains Mono', monospace" }}>
                  {d.key}
                </Typography>
                <Typography variant="caption" sx={{ fontFamily: "'JetBrains Mono', monospace" }}>
                  {d.value}
                </Typography>
              </Box>
            ))}
          </Box>
          <Box sx={{ width: 2, height: 16, bgcolor: "divider" }} />
        </Box>
      )}

      {/* Table scan node */}
      {table && (
        <Box sx={{ display: "flex", flexDirection: "column", alignItems: "center", gap: 1 }}>
          <Box sx={{ px: 3, py: 1, borderRadius: 2, bgcolor: "rgba(255,193,7,0.1)", border: "1px solid rgba(255,193,7,0.3)", textAlign: "center" }}>
            <Typography variant="caption" sx={{ fontWeight: 600, color: "#FFC107", fontFamily: "'JetBrains Mono', monospace" }}>
              📋 {table}
            </Typography>
            <Box sx={{ display: "flex", gap: 1, mt: 0.5, justifyContent: "center" }}>
              {stats.map((s) => (
                <Chip key={s.key} label={`${s.key}: ${s.value}`} size="small" variant="outlined"
                  sx={{ fontSize: 10, height: 20, fontFamily: "'JetBrains Mono', monospace" }} />
              ))}
            </Box>
          </Box>
        </Box>
      )}
    </Box>
  );
}
