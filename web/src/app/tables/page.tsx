"use client";
import { useEffect, useState, useCallback } from "react";
import {
  Box, Card, Typography, Table, TableHead, TableRow, TableCell, TableBody,
  TableContainer, Chip, CircularProgress, Alert, IconButton, Tooltip, TextField,
  InputAdornment,
} from "@mui/material";
import RefreshIcon from "@mui/icons-material/Refresh";
import SearchIcon from "@mui/icons-material/Search";
import FirstPageIcon from "@mui/icons-material/FirstPage";
import LastPageIcon from "@mui/icons-material/LastPage";
import ChevronLeftIcon from "@mui/icons-material/ChevronLeft";
import ChevronRightIcon from "@mui/icons-material/ChevronRight";
import StorageIcon from "@mui/icons-material/Storage";
import { querySQL } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import PaginatedTable from "@/components/PaginatedTable";

interface TableInfo { name: string; rows: number }
interface SchemaCol { column: string; type: string }
interface PreviewData { columns: string[]; data: (string | number)[][]; rows: number; execution_time_us?: number }

export default function TablesPage() {
  const { auth } = useAuth();
  const [tables, setTables] = useState<TableInfo[]>([]);
  const [selected, setSelected] = useState<string | null>(null);
  const [schema, setSchema] = useState<SchemaCol[]>([]);
  const [preview, setPreview] = useState<PreviewData | null>(null);
  const [loading, setLoading] = useState(true);
  const [detailLoading, setDetailLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [filter, setFilter] = useState("");

  const [previewPage, setPreviewPage] = useState(0);
  const [previewTotal, setPreviewTotal] = useState(0);
  const previewPageSize = 50;

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

  useEffect(() => {
    if (!selected) { setSchema([]); setPreview(null); setPreviewTotal(0); setPreviewPage(0); return; }
    setDetailLoading(true);
    setPreviewPage(0);
    Promise.all([
      querySQL(`DESCRIBE ${selected}`, auth?.apiKey).catch(() => ({ data: [] })),
      querySQL(`SELECT count(*) FROM ${selected}`, auth?.apiKey).catch(() => ({ data: [[0]] })),
    ]).then(([schemaRes, countRes]) => {
      setSchema((schemaRes.data ?? []).map((row: (string | number)[]) => ({
        column: String(row[0]), type: String(row[1] ?? "unknown"),
      })));
      setPreviewTotal(Number(countRes.data?.[0]?.[0] ?? 0));
    }).finally(() => setDetailLoading(false));
  }, [selected, auth]);

  // Server-side paginated preview
  useEffect(() => {
    if (!selected) return;
    setDetailLoading(true);
    querySQL(`SELECT * FROM ${selected} LIMIT ${previewPageSize} OFFSET ${previewPage * previewPageSize}`, auth?.apiKey)
      .then(setPreview)
      .catch(() => setPreview(null))
      .finally(() => setDetailLoading(false));
  }, [selected, auth, previewPage]);

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
                  <TableRow key={t.name} hover selected={selected === t.name}
                    onClick={() => { setSelected(t.name); }}
                    sx={{ cursor: "pointer", "&.Mui-selected": { bgcolor: "rgba(92,107,192,0.12)" } }}>
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

      {/* Schema */}
      {selected && (
        <Card>
          <Box sx={{ px: 2, py: 1, borderBottom: "1px solid rgba(255, 255, 255, 0.08)", display: "flex", gap: 1, alignItems: "center" }}>
            <Chip label={selected} size="small" color="primary" variant="outlined" />
            <Typography variant="caption" color="text.secondary">Schema</Typography>
            {detailLoading && <CircularProgress size={14} sx={{ ml: 1 }} />}
          </Box>
          <TableContainer>
            <Table size="small">
              <TableHead>
                <TableRow>
                  <TableCell sx={headerCell}>Column</TableCell>
                  <TableCell sx={headerCell}>Type</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                {schema.map((c) => (
                  <TableRow key={c.column}>
                    <TableCell sx={mono}>{c.column}</TableCell>
                    <TableCell>
                      <Chip label={c.type} size="small" variant="outlined"
                        sx={{ ...mono, height: 22, borderColor: "#334155" }} />
                    </TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </TableContainer>
        </Card>
      )}

      {/* Preview */}
      {selected && preview && (
        <Card>
          <Box sx={{ px: 2, py: 1, borderBottom: "1px solid rgba(255, 255, 255, 0.08)", display: "flex", justifyContent: "space-between", alignItems: "center" }}>
            <Typography variant="caption" color="text.secondary">
              Data Preview — {previewTotal.toLocaleString()} rows
              {preview.execution_time_us != null && ` · ${preview.execution_time_us.toLocaleString()}μs`}
            </Typography>
            {previewTotal > previewPageSize && (
              <Box sx={{ display: "flex", alignItems: "center", gap: 0.5 }}>
                <Typography variant="caption" color="text.secondary">
                  {previewPage * previewPageSize + 1}–{Math.min((previewPage + 1) * previewPageSize, previewTotal)} of {previewTotal.toLocaleString()}
                </Typography>
                <IconButton size="small" disabled={previewPage === 0} onClick={() => setPreviewPage(0)}><FirstPageIcon fontSize="small" /></IconButton>
                <IconButton size="small" disabled={previewPage === 0} onClick={() => setPreviewPage(previewPage - 1)}><ChevronLeftIcon fontSize="small" /></IconButton>
                <IconButton size="small" disabled={(previewPage + 1) * previewPageSize >= previewTotal} onClick={() => setPreviewPage(previewPage + 1)}><ChevronRightIcon fontSize="small" /></IconButton>
                <IconButton size="small" disabled={(previewPage + 1) * previewPageSize >= previewTotal} onClick={() => setPreviewPage(Math.ceil(previewTotal / previewPageSize) - 1)}><LastPageIcon fontSize="small" /></IconButton>
              </Box>
            )}
          </Box>
          <PaginatedTable columns={preview.columns} data={preview.data} maxHeight={400} />
        </Card>
      )}
    </Box>
  );
}
