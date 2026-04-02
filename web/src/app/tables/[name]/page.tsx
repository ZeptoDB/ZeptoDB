"use client";
import { useEffect, useState, useCallback } from "react";
import { useParams, useRouter } from "next/navigation";
import {
  Box, Card, Typography, Table, TableHead, TableRow, TableCell, TableBody,
  TableContainer, Chip, CircularProgress, Alert, IconButton, Tooltip,
  Breadcrumbs, Link, Stack, Divider,
} from "@mui/material";
import RefreshIcon from "@mui/icons-material/Refresh";
import ArrowBackIcon from "@mui/icons-material/ArrowBack";
import StorageIcon from "@mui/icons-material/Storage";
import { querySQL } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import PaginatedTable from "@/components/PaginatedTable";

interface SchemaCol { column: string; type: string; nullable?: string }

const MONO = { fontFamily: "'JetBrains Mono', monospace", fontSize: 13 };
const HEADER = { fontWeight: 600, fontSize: 12, textTransform: "uppercase" as const, color: "text.secondary", letterSpacing: 0.5 };

export default function TableDetailPage() {
  const { name } = useParams<{ name: string }>();
  const tableName = decodeURIComponent(name);
  const router = useRouter();
  const { auth } = useAuth();

  const [schema, setSchema] = useState<SchemaCol[]>([]);
  const [rowCount, setRowCount] = useState<number | null>(null);
  const [colStats, setColStats] = useState<Record<string, { min: string; max: string; nulls: number }>>({});
  const [preview, setPreview] = useState<{ columns: string[]; data: (string | number)[][] } | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(() => {
    setLoading(true);
    setError(null);
    const q = (sql: string) => querySQL(sql, auth?.apiKey).catch(() => null);

    Promise.all([
      q(`DESCRIBE ${tableName}`),
      q(`SELECT count(*) FROM ${tableName}`),
      q(`SELECT * FROM ${tableName} LIMIT 50`),
    ]).then(([schemaRes, countRes, previewRes]) => {
      const cols: SchemaCol[] = (schemaRes?.data ?? []).map((r: (string | number)[]) => ({
        column: String(r[0]), type: String(r[1] ?? "unknown"),
      }));
      setSchema(cols);
      setRowCount(Number(countRes?.data?.[0]?.[0] ?? 0));
      if (previewRes) setPreview(previewRes);

      // Fetch per-column min/max/null stats for numeric/timestamp columns
      const statCols = cols.filter(c =>
        /int|float|double|long|timestamp|short|byte/i.test(c.type)
      );
      if (statCols.length > 0) {
        const exprs = statCols.flatMap(c => [
          `min(${c.column})`, `max(${c.column})`,
        ]);
        q(`SELECT ${exprs.join(",")} FROM ${tableName}`).then(statsRes => {
          if (!statsRes?.data?.[0]) return;
          const row = statsRes.data[0];
          const map: typeof colStats = {};
          statCols.forEach((c, i) => {
            map[c.column] = {
              min: String(row[i * 2] ?? "—"),
              max: String(row[i * 2 + 1] ?? "—"),
              nulls: 0,
            };
          });
          setColStats(map);
        });
      }
    }).catch(e => setError(e.message)).finally(() => setLoading(false));
  }, [tableName, auth]);

  useEffect(() => { load(); }, [load]);

  if (loading) {
    return <Box sx={{ display: "flex", justifyContent: "center", py: 8 }}><CircularProgress /></Box>;
  }

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 2 }}>
      {/* Breadcrumb + header */}
      <Box sx={{ display: "flex", alignItems: "center", gap: 1 }}>
        <Tooltip title="Back to Tables">
          <IconButton size="small" onClick={() => router.push("/tables")}><ArrowBackIcon /></IconButton>
        </Tooltip>
        <Breadcrumbs sx={{ fontSize: 14 }}>
          <Link underline="hover" color="text.secondary" sx={{ cursor: "pointer" }} onClick={() => router.push("/tables")}>Tables</Link>
          <Typography color="text.primary" fontSize={14} fontWeight={600}>{tableName}</Typography>
        </Breadcrumbs>
        <Box sx={{ flex: 1 }} />
        <Tooltip title="Refresh"><IconButton size="small" onClick={load}><RefreshIcon /></IconButton></Tooltip>
      </Box>

      {error && <Alert severity="error" onClose={() => setError(null)}>{error}</Alert>}

      {/* Summary cards */}
      <Box sx={{ display: "flex", gap: 2, flexWrap: "wrap" }}>
        <Card sx={{ p: 2.5, minWidth: 160 }}>
          <Typography variant="caption" color="text.secondary" sx={{ textTransform: "uppercase", fontWeight: 600, letterSpacing: "0.05em" }}>Rows</Typography>
          <Typography variant="h5" sx={{ mt: 0.5, fontWeight: 700, ...MONO }}>{rowCount?.toLocaleString() ?? "—"}</Typography>
        </Card>
        <Card sx={{ p: 2.5, minWidth: 160 }}>
          <Typography variant="caption" color="text.secondary" sx={{ textTransform: "uppercase", fontWeight: 600, letterSpacing: "0.05em" }}>Columns</Typography>
          <Typography variant="h5" sx={{ mt: 0.5, fontWeight: 700, ...MONO }}>{schema.length}</Typography>
        </Card>
      </Box>

      {/* Schema with stats */}
      <Card>
        <Box sx={{ px: 2, py: 1.5, borderBottom: "1px solid rgba(255,255,255,0.08)", display: "flex", alignItems: "center", gap: 1 }}>
          <StorageIcon sx={{ fontSize: 18, color: "primary.main" }} />
          <Typography variant="subtitle2" fontWeight={600}>Schema &amp; Column Stats</Typography>
        </Box>
        <TableContainer>
          <Table size="small">
            <TableHead>
              <TableRow>
                <TableCell sx={HEADER}>Column</TableCell>
                <TableCell sx={HEADER}>Type</TableCell>
                <TableCell sx={HEADER}>Min</TableCell>
                <TableCell sx={HEADER}>Max</TableCell>
              </TableRow>
            </TableHead>
            <TableBody>
              {schema.map(c => {
                const s = colStats[c.column];
                return (
                  <TableRow key={c.column}>
                    <TableCell sx={MONO}>{c.column}</TableCell>
                    <TableCell>
                      <Chip label={c.type} size="small" variant="outlined" sx={{ ...MONO, height: 22, borderColor: "rgba(255,255,255,0.08)" }} />
                    </TableCell>
                    <TableCell sx={{ ...MONO, color: "text.secondary" }}>{s?.min ?? "—"}</TableCell>
                    <TableCell sx={{ ...MONO, color: "text.secondary" }}>{s?.max ?? "—"}</TableCell>
                  </TableRow>
                );
              })}
            </TableBody>
          </Table>
        </TableContainer>
      </Card>

      {/* Data preview */}
      {preview && (
        <Card>
          <Box sx={{ px: 2, py: 1.5, borderBottom: "1px solid rgba(255,255,255,0.08)" }}>
            <Typography variant="subtitle2" fontWeight={600}>Data Preview (first 50 rows)</Typography>
          </Box>
          <PaginatedTable columns={preview.columns} data={preview.data} maxHeight={400} />
        </Card>
      )}
    </Box>
  );
}
