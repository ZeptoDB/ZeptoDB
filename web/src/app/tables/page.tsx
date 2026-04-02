"use client";
import { useEffect, useState, useCallback } from "react";
import {
  Box, Card, Typography, Table, TableHead, TableRow, TableCell, TableBody,
  TableContainer, Chip, CircularProgress, Alert, IconButton, Tooltip, TextField,
  InputAdornment,
} from "@mui/material";
import RefreshIcon from "@mui/icons-material/Refresh";
import SearchIcon from "@mui/icons-material/Search";
import StorageIcon from "@mui/icons-material/Storage";
import { useRouter } from "next/navigation";
import { querySQL } from "@/lib/api";
import { useAuth } from "@/lib/auth";

interface TableInfo { name: string; rows: number }

export default function TablesPage() {
  const { auth } = useAuth();
  const router = useRouter();
  const [tables, setTables] = useState<TableInfo[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [filter, setFilter] = useState("");

  const loadTables = useCallback(() => {
    setLoading(true);
    setError(null);
    querySQL("SHOW TABLES", auth?.apiKey)
      .then((r) => {
        const list: TableInfo[] = (r.data ?? []).map((row: (string | number)[]) => ({
          name: String(row[0]),
          rows: Number(row[1] ?? 0),
        }));
        setTables(list);
      })
      .catch((e) => setError(e.message))
      .finally(() => setLoading(false));
  }, [auth]);

  useEffect(() => { loadTables(); }, [loadTables]);

  const filtered = tables.filter((t) => t.name.toLowerCase().includes(filter.toLowerCase()));

  const mono = { fontFamily: "'JetBrains Mono', monospace", fontSize: 13 };
  const headerCell = { fontWeight: 600, fontSize: 12, textTransform: "uppercase" as const, color: "text.secondary", letterSpacing: 0.5 };

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 2 }}>
      {/* Header */}
      <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between" }}>
        <Box sx={{ display: "flex", alignItems: "center", gap: 1 }}>
          <StorageIcon sx={{ color: "primary.main" }} />
          <Typography variant="h4" sx={{ fontWeight: 700, letterSpacing: "-0.02em" }}>Tables</Typography>
          <Chip label={`${tables.length}`} size="small" variant="outlined" sx={{ ml: 1, borderColor: "rgba(255, 255, 255, 0.12)" }} />
        </Box>
        <Tooltip title="Refresh">
          <IconButton onClick={loadTables} size="small"><RefreshIcon /></IconButton>
        </Tooltip>
      </Box>

      {error && <Alert severity="error" onClose={() => setError(null)}>{error}</Alert>}

      {/* Table List */}
      <Card>
        <Box sx={{ px: 2, py: 1.5, borderBottom: "1px solid rgba(255, 255, 255, 0.08)" }}>
          <TextField
            size="small" fullWidth placeholder="Filter tables…" value={filter}
            onChange={(e) => setFilter(e.target.value)}
            slotProps={{ input: { startAdornment: <InputAdornment position="start"><SearchIcon sx={{ fontSize: 18, color: "text.secondary" }} /></InputAdornment> } }}
            sx={{ "& .MuiOutlinedInput-root": { fontSize: 13 } }}
          />
        </Box>
        {loading ? (
          <Box sx={{ display: "flex", justifyContent: "center", py: 4 }}><CircularProgress size={28} /></Box>
        ) : (
          <TableContainer sx={{ maxHeight: 320 }}>
            <Table size="small" stickyHeader>
              <TableHead>
                <TableRow>
                  <TableCell sx={headerCell}>Table</TableCell>
                  <TableCell sx={headerCell} align="right">Rows</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                {filtered.map((t) => (
                  <TableRow key={t.name} hover
                    onClick={() => router.push(`/tables/${encodeURIComponent(t.name)}`)}
                    sx={{ cursor: "pointer" }}>
                    <TableCell sx={mono}>{t.name}</TableCell>
                    <TableCell align="right" sx={{ ...mono, color: "text.secondary" }}>{t.rows.toLocaleString()}</TableCell>
                  </TableRow>
                ))}
                {filtered.length === 0 && (
                  <TableRow>
                    <TableCell colSpan={2} sx={{ textAlign: "center", color: "text.secondary", py: 4 }}>
                      {tables.length === 0 ? "No tables — ingest data first" : "No match"}
                    </TableCell>
                  </TableRow>
                )}
              </TableBody>
            </Table>
          </TableContainer>
        )}
      </Card>
    </Box>
  );
}
