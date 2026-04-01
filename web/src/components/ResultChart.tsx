"use client";
import { useMemo, useState } from "react";
import { Box, Select, MenuItem, Typography, type SelectChangeEvent } from "@mui/material";
import {
  LineChart, Line, BarChart, Bar, XAxis, YAxis, CartesianGrid, Tooltip,
  ResponsiveContainer, Legend,
} from "recharts";

interface Props {
  columns: string[];
  data: (string | number)[][];
}

const COLORS = ["#3F51B5", "#FFC107", "#4CAF50", "#FF5722", "#9C27B0", "#00BCD4", "#E91E63", "#8BC34A"];

function isNumeric(v: string | number) {
  return typeof v === "number" || (typeof v === "string" && v !== "" && !isNaN(Number(v)));
}

export default function ResultChart({ columns, data }: Props) {
  const numericCols = useMemo(() => columns.filter((_, i) => data.length > 0 && isNumeric(data[0][i])), [columns, data]);
  const labelCols = useMemo(() => columns.filter((_, i) => data.length > 0 && !isNumeric(data[0][i])), [columns, data]);

  const [chartType, setChartType] = useState<"line" | "bar">("line");
  const [xCol, setXCol] = useState(labelCols[0] ?? columns[0] ?? "");
  const [yCols, setYCols] = useState<string[]>(numericCols.slice(0, 3));

  const chartData = useMemo(() => {
    const xi = columns.indexOf(xCol);
    return data.slice(0, 500).map((row) => {
      const obj: Record<string, string | number> = { x: xi >= 0 ? row[xi] : "" };
      yCols.forEach((c) => { const ci = columns.indexOf(c); if (ci >= 0) obj[c] = Number(row[ci]); });
      return obj;
    });
  }, [columns, data, xCol, yCols]);

  if (numericCols.length === 0) {
    return <Box sx={{ p: 3, textAlign: "center" }}><Typography color="text.secondary">No numeric columns to chart</Typography></Box>;
  }

  const Chart = chartType === "bar" ? BarChart : LineChart;

  return (
    <Box sx={{ display: "flex", flexDirection: "column", flex: 1, minHeight: 0 }}>
      <Box sx={{ display: "flex", gap: 2, px: 2, py: 1, alignItems: "center", borderBottom: "1px solid", borderColor: "divider", flexWrap: "wrap" }}>
        <Select size="small" value={chartType} onChange={(e: SelectChangeEvent) => setChartType(e.target.value as "line" | "bar")} sx={{ height: 28, fontSize: 12 }}>
          <MenuItem value="line">Line</MenuItem>
          <MenuItem value="bar">Bar</MenuItem>
        </Select>
        <Typography variant="caption" color="text.secondary">X:</Typography>
        <Select size="small" value={xCol} onChange={(e: SelectChangeEvent) => setXCol(e.target.value)} sx={{ height: 28, fontSize: 12, minWidth: 80 }}>
          {columns.map((c) => <MenuItem key={c} value={c}>{c}</MenuItem>)}
        </Select>
        <Typography variant="caption" color="text.secondary">Y:</Typography>
        <Select<string[]> size="small" multiple value={yCols} onChange={(e) => { const v = e.target.value; setYCols(typeof v === "string" ? v.split(",") : v); }} sx={{ height: 28, fontSize: 12, minWidth: 120 }}>
          {numericCols.map((c) => <MenuItem key={c} value={c}>{c}</MenuItem>)}
        </Select>
        {data.length > 500 && <Typography variant="caption" color="text.secondary">(showing first 500 rows)</Typography>}
      </Box>
      <Box sx={{ flex: 1, minHeight: 200, p: 1 }}>
        <ResponsiveContainer width="100%" height="100%">
          <Chart data={chartData}>
            <CartesianGrid strokeDasharray="3 3" stroke="rgba(255,255,255,0.1)" />
            <XAxis dataKey="x" tick={{ fontSize: 10 }} />
            <YAxis tick={{ fontSize: 10 }} />
            <Tooltip contentStyle={{ backgroundColor: "#1E1E1E", border: "1px solid rgba(255,255,255,0.1)", fontSize: 12 }} />
            <Legend wrapperStyle={{ fontSize: 12 }} />
            {yCols.map((c, i) =>
              chartType === "bar"
                ? <Bar key={c} dataKey={c} fill={COLORS[i % COLORS.length]} />
                : <Line key={c} dataKey={c} stroke={COLORS[i % COLORS.length]} dot={false} strokeWidth={1.5} />
            )}
          </Chart>
        </ResponsiveContainer>
      </Box>
    </Box>
  );
}
