"use client";
import { useCallback } from "react";
import { useParams, useRouter } from "next/navigation";
import {
  Box, Card, Typography, Table, TableHead, TableRow, TableCell, TableBody,
  TableContainer, Chip, CircularProgress, Alert, IconButton, Tooltip,
  Breadcrumbs, Link,
} from "@mui/material";
import { useQuery } from "@tanstack/react-query";
import RefreshIcon from "@mui/icons-material/Refresh";
import ArrowBackIcon from "@mui/icons-material/ArrowBack";
import StorageIcon from "@mui/icons-material/Storage";
import { querySQL } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import PaginatedTable from "@/components/PaginatedTable";

interface SchemaCol { column: string; type: string; nullable?: string }
interface TableDetail {
  schema: SchemaCol[];
  rowCount: number;
  colStats: Record<string, { min: string; max: string; nulls: number }>;
  preview: { columns: string[]; data: (string | number)[][] } | null;
}

const MONO = { fontFamily: "'JetBrains Mono', monospace", fontSize: 13 };
const HEADER = { fontWeight: 600, fontSize: 12, textTransform: "uppercase" as const, color: "text.secondary", letterSpacing: 0.5 };

export default function TableDetailPage() {
  const { name } = useParams<{ name: string }>();
  const tableName = decodeURIComponent(name);
  const router = useRouter();
  const { auth } = useAuth();

  const load = useCallback(async (): Promise<TableDetail> => {
    const q = (sql: string) => querySQL(sql, auth?.apiKey).catch(() => null);

    const [schemaRes, countRes, previewRes] = await Promise.all([
      q(`DESCRIBE ${tableName}`),
      q(`SELECT count(*) FROM ${tableName}`),
      q(`SELECT * FROM ${tableName} LIMIT 50`),
    ]);

    const cols: SchemaCol[] = (schemaRes?.data ?? []).map((r: (string | number)[]) => ({
      column: String(r[0]), type: String(r[1] ?? "unknown"),
    }));

    const statCols = cols.filter(c =>
      /int|float|double|long|timestamp|short|byte/i.test(c.type)
    );
    const colStats: TableDetail["colStats"] = {};
    if (statCols.length > 0) {
      const exprs = statCols.flatMap(c => [
        `min(${c.column})`, `max(${c.column})`,
      ]);
      const statsRes = await q(`SELECT ${exprs.join(",")} FROM ${tableName}`);
      const row = statsRes?.data?.[0];
      if (row) {
        statCols.forEach((c, i) => {
          colStats[c.column] = {
            min: String(row[i * 2] ?? "—"),
            max: String(row[i * 2 + 1] ?? "—"),
            nulls: 0,
          };
        });
      }
    }

    return {
      schema: cols,
      rowCount: Number(countRes?.data?.[0]?.[0] ?? 0),
      preview: previewRes,
      colStats,
    };
  }, [tableName, auth]);

  const { data, isLoading: loading, error, refetch } = useQuery<TableDetail>({
    queryKey: ["table-detail", tableName, auth?.apiKey],
    queryFn: load,
  });

  if (loading) {
    return <Box sx={{ display: "flex", justifyContent: "center", py: 8 }}><CircularProgress /></Box>;
  }

  const schema = data?.schema ?? [];
  const rowCount = data?.rowCount ?? 0;
  const colStats = data?.colStats ?? {};
  const preview = data?.preview ?? null;

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
        <Tooltip title="Refresh"><IconButton size="small" onClick={() => refetch()}><RefreshIcon /></IconButton></Tooltip>
      </Box>

      {error && <Alert severity="error">{error instanceof Error ? error.message : "Failed to load table"}</Alert>}

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
