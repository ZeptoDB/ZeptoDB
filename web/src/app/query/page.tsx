"use client";
import { useState, useCallback, useEffect } from "react";
import { Box, Button, Paper, Typography, Table, TableHead, TableRow, TableCell, TableBody, TableContainer, Chip, Alert, List, ListItemButton, ListItemText, IconButton, Tooltip, Snackbar, Menu, MenuItem, ListItemIcon } from "@mui/material";
import PlayArrowIcon from "@mui/icons-material/PlayArrow";
import HistoryIcon from "@mui/icons-material/History";
import FileDownloadIcon from "@mui/icons-material/FileDownload";
import ContentCopyIcon from "@mui/icons-material/ContentCopy";
import TableChartIcon from "@mui/icons-material/TableChart";
import DataObjectIcon from "@mui/icons-material/DataObject";
import CodeMirror from "@uiw/react-codemirror";
import { sql } from "@codemirror/lang-sql";
import { querySQL } from "@/lib/api";
import { useAuth } from "@/lib/auth";

function downloadFile(content: string, filename: string, mime: string) {
  const blob = new Blob([content], { type: mime });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}

function toCSV(columns: string[], data: (string | number)[][]) {
  const escape = (v: string | number) => {
    const s = String(v);
    return s.includes(",") || s.includes('"') || s.includes("\n") ? `"${s.replace(/"/g, '""')}"` : s;
  };
  return [columns.map(escape).join(","), ...data.map((r) => r.map(escape).join(","))].join("\n");
}

function toJSON(columns: string[], data: (string | number)[][]) {
  return JSON.stringify(data.map((r) => Object.fromEntries(columns.map((c, i) => [c, r[i]]))), null, 2);
}

const HISTORY_KEY = "zepto_query_history";
const MAX_HISTORY = 50;

export function loadHistory(): string[] {
  try { return JSON.parse(localStorage.getItem(HISTORY_KEY) || "[]"); } catch { return []; }
}

function saveHistory(history: string[]) {
  localStorage.setItem(HISTORY_KEY, JSON.stringify(history.slice(0, MAX_HISTORY)));
}

export default function QueryPage() {
  const { auth } = useAuth();
  const [code, setCode] = useState("SELECT * FROM trades WHERE symbol = 1 LIMIT 100");
  const [result, setResult] = useState<{ columns: string[]; data: (string | number)[][]; rows: number; execution_time_us: number } | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [history, setHistory] = useState<string[]>([]);
  const [showHistory, setShowHistory] = useState(false);
  const [snackMsg, setSnackMsg] = useState<string | null>(null);
  const [exportAnchor, setExportAnchor] = useState<null | HTMLElement>(null);

  useEffect(() => { setHistory(loadHistory()); }, []);

  const copyToClipboard = useCallback(() => {
    if (!result) return;
    const tsv = [result.columns.join("\t"), ...result.data.map((r) => r.map(String).join("\t"))].join("\n");
    navigator.clipboard.writeText(tsv).then(() => setSnackMsg("Copied to clipboard"));
  }, [result]);

  const exportCSV = useCallback(() => {
    if (!result) return;
    downloadFile(toCSV(result.columns, result.data), "query_result.csv", "text/csv");
    setExportAnchor(null);
  }, [result]);

  const exportJSON = useCallback(() => {
    if (!result) return;
    downloadFile(toJSON(result.columns, result.data), "query_result.json", "application/json");
    setExportAnchor(null);
  }, [result]);

  const run = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const r = await querySQL(code, auth?.apiKey);
      setResult(r);
      const updated = [code, ...history.filter((h) => h !== code)].slice(0, MAX_HISTORY);
      setHistory(updated);
      saveHistory(updated);
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : "Unknown error");
      setResult(null);
    } finally {
      setLoading(false);
    }
  }, [code, auth, history]);

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 2, height: "calc(100vh - 96px)" }}>
      <Paper sx={{ p: 0, overflow: "hidden", border: "1px solid #1E293B" }}>
        <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between", px: 2, py: 1, borderBottom: "1px solid #1E293B" }}>
          <Typography variant="body2" color="text.secondary">SQL Editor</Typography>
          <Box sx={{ display: "flex", gap: 1 }}>
            <Button size="small" startIcon={<HistoryIcon />} onClick={() => setShowHistory(!showHistory)} sx={{ textTransform: "none" }}>
              History
            </Button>
            <Button variant="contained" size="small" startIcon={<PlayArrowIcon />} onClick={run} disabled={loading} sx={{ textTransform: "none" }}>
              {loading ? "Running..." : "Run"} <Typography variant="caption" sx={{ ml: 1, opacity: 0.7 }}>⌘↵</Typography>
            </Button>
          </Box>
        </Box>
        {showHistory && history.length > 0 && (
          <Box sx={{ maxHeight: 150, overflow: "auto", borderBottom: "1px solid #1E293B" }}>
            <List dense disablePadding>
              {history.map((h, i) => (
                <ListItemButton key={i} onClick={() => { setCode(h); setShowHistory(false); }}>
                  <ListItemText primary={h} primaryTypographyProps={{ fontSize: 12, fontFamily: "'JetBrains Mono', monospace", noWrap: true }} />
                </ListItemButton>
              ))}
            </List>
          </Box>
        )}
        <CodeMirror
          value={code}
          height="180px"
          extensions={[sql()]}
          theme="dark"
          onChange={setCode}
          onKeyDown={(e) => { if ((e.metaKey || e.ctrlKey) && e.key === "Enter") { e.preventDefault(); run(); } }}
          basicSetup={{ lineNumbers: true, foldGutter: false }}
          style={{ fontSize: 14, fontFamily: "'JetBrains Mono', monospace" }}
        />
      </Paper>

      {error && <Alert severity="error" variant="outlined">{error}</Alert>}

      {result && (
        <Paper sx={{ flex: 1, overflow: "hidden", border: "1px solid #1E293B", display: "flex", flexDirection: "column" }}>
          <Box sx={{ px: 2, py: 1, borderBottom: "1px solid #1E293B", display: "flex", gap: 2, alignItems: "center" }}>
            <Chip label={`${result.rows} rows`} size="small" variant="outlined" />
            <Chip label={`${result.execution_time_us} μs`} size="small" color="primary" variant="outlined" />
            <Chip label={`${result.columns.length} cols`} size="small" variant="outlined" />
            <Box sx={{ flex: 1 }} />
            <Tooltip title="Copy as TSV">
              <IconButton size="small" onClick={copyToClipboard}><ContentCopyIcon fontSize="small" /></IconButton>
            </Tooltip>
            <Tooltip title="Export">
              <IconButton size="small" onClick={(e) => setExportAnchor(e.currentTarget)}><FileDownloadIcon fontSize="small" /></IconButton>
            </Tooltip>
            <Menu anchorEl={exportAnchor} open={Boolean(exportAnchor)} onClose={() => setExportAnchor(null)}>
              <MenuItem onClick={exportCSV}><ListItemIcon><TableChartIcon fontSize="small" /></ListItemIcon>Export CSV</MenuItem>
              <MenuItem onClick={exportJSON}><ListItemIcon><DataObjectIcon fontSize="small" /></ListItemIcon>Export JSON</MenuItem>
            </Menu>
          </Box>
          <TableContainer sx={{ flex: 1 }}>
            <Table size="small" stickyHeader>
              <TableHead>
                <TableRow>
                  {result.columns.map((c) => (
                    <TableCell key={c} sx={{ fontWeight: 600, fontFamily: "'JetBrains Mono', monospace", fontSize: 12, bgcolor: "#0D1220" }}>{c}</TableCell>
                  ))}
                </TableRow>
              </TableHead>
              <TableBody>
                {result.data.map((row, i) => (
                  <TableRow key={i} hover>
                    {row.map((cell, j) => (
                      <TableCell key={j} sx={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 12 }}>{String(cell)}</TableCell>
                    ))}
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </TableContainer>
        </Paper>
      )}

      <Snackbar open={Boolean(snackMsg)} autoHideDuration={2000} onClose={() => setSnackMsg(null)} message={snackMsg} />
    </Box>
  );
}
