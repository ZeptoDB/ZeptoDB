"use client";
import { useState, useCallback, useEffect, useRef } from "react";
import {
  Box, Button, Card, Typography, Chip, Alert, List, ListItemButton, ListItemText,
  IconButton, Tooltip, Snackbar, Menu, MenuItem, ListItemIcon, Tab, Tabs,
  ToggleButtonGroup, ToggleButton,
} from "@mui/material";
import { useTheme } from "@mui/material/styles";
import StopIcon from "@mui/icons-material/Stop";
import PlayArrowIcon from "@mui/icons-material/PlayArrow";
import HistoryIcon from "@mui/icons-material/History";
import FileDownloadIcon from "@mui/icons-material/FileDownload";
import ContentCopyIcon from "@mui/icons-material/ContentCopy";
import TableChartIcon from "@mui/icons-material/TableChart";
import DataObjectIcon from "@mui/icons-material/DataObject";
import KeyboardIcon from "@mui/icons-material/Keyboard";
import SaveIcon from "@mui/icons-material/Save";
import BookmarkIcon from "@mui/icons-material/Bookmark";
import DeleteOutlineIcon from "@mui/icons-material/DeleteOutline";
import SearchIcon from "@mui/icons-material/Search";
import PushPinIcon from "@mui/icons-material/PushPin";
import PushPinOutlinedIcon from "@mui/icons-material/PushPinOutlined";
import AddIcon from "@mui/icons-material/Add";
import CloseIcon from "@mui/icons-material/Close";
import BarChartIcon from "@mui/icons-material/BarChart";
import GridOnIcon from "@mui/icons-material/GridOn";
import CodeMirror, { type ReactCodeMirrorRef } from "@uiw/react-codemirror";
import { sql } from "@codemirror/lang-sql";
import { autocompletion } from "@codemirror/autocomplete";
import { EditorView, Decoration, type DecorationSet } from "@codemirror/view";
import { StateField, StateEffect } from "@codemirror/state";
import { querySQL, fetchQueries, killQuery } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import { zeptoCompletions } from "@/lib/zeptoCompletions";
import PaginatedTable from "@/components/PaginatedTable";
import ResultChart from "@/components/ResultChart";
import ResizeDivider from "@/components/ResizeDivider";
import SchemaSidebar from "@/components/SchemaSidebar";
import ExecSparkline from "@/components/ExecSparkline";
import ExplainView from "@/components/ExplainView";

/* ── helpers ─────────────────────────────────────────────────────────────── */

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

/** Split SQL text on `;` boundaries, ignoring `;` inside strings. */
function splitStatements(text: string): string[] {
  const stmts: string[] = [];
  let cur = "";
  let inSingle = false;
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (ch === "'" && text[i - 1] !== "\\") { inSingle = !inSingle; cur += ch; }
    else if (ch === ";" && !inSingle) { const t = cur.trim(); if (t) stmts.push(t); cur = ""; }
    else { cur += ch; }
  }
  const t = cur.trim();
  if (t) stmts.push(t);
  return stmts;
}

/* ── types ───────────────────────────────────────────────────────────────── */

/* ── error marker (QE-8) ────────────────────────────────────────────────── */

const setErrorMarker = StateEffect.define<{ from: number; to: number } | null>();

const errorLineField = StateField.define<DecorationSet>({
  create: () => Decoration.none,
  update(deco, tr) {
    for (const e of tr.effects) {
      if (e.is(setErrorMarker)) {
        if (e.value === null) return Decoration.none;
        return Decoration.set([
          Decoration.line({ attributes: { style: "background-color: rgba(255,23,68,0.15); outline: 1px solid rgba(255,23,68,0.4);" } }).range(e.value.from),
        ]);
      }
    }
    return deco;
  },
  provide: (f) => EditorView.decorations.from(f),
});

/** Try to extract line number from error message like "line 2" or "at line 3, column 5" */
function parseErrorLine(msg: string): number | null {
  const m = msg.match(/line\s+(\d+)/i);
  return m ? parseInt(m[1], 10) : null;
}

/* ── types (continued) ──────────────────────────────────────────────────── */

interface QueryResult {
  columns: string[];
  data: (string | number)[][];
  rows: number;
  execution_time_us: number;
}

interface StatementResult {
  sql: string;
  result?: QueryResult;
  error?: string;
}

interface EditorTab {
  id: string;
  name: string;
  code: string;
  results: StatementResult[];
  activeResultIdx: number;
}

interface SavedQuery {
  name: string;
  sql: string;
}

/* ── history ─────────────────────────────────────────────────────────────── */

const HISTORY_KEY = "zepto_query_history";
const TABS_KEY = "zepto_editor_tabs";
const PINS_KEY = "zepto_query_pins";
const EXEC_TIMES_KEY = "zepto_exec_times";
const SAVED_KEY = "zepto_saved_queries";
const MAX_HISTORY = 50;
const MAX_EXEC_TIMES = 20;

export function loadHistory(): string[] {
  try { return JSON.parse(localStorage.getItem(HISTORY_KEY) || "[]"); } catch { return []; }
}
function saveHistory(history: string[]) {
  localStorage.setItem(HISTORY_KEY, JSON.stringify(history.slice(0, MAX_HISTORY)));
}

function loadPins(): Set<string> {
  try { return new Set(JSON.parse(localStorage.getItem(PINS_KEY) || "[]")); } catch { return new Set(); }
}
function savePins(pins: Set<string>) {
  localStorage.setItem(PINS_KEY, JSON.stringify([...pins]));
}

function loadExecTimes(): number[] {
  try { return JSON.parse(localStorage.getItem(EXEC_TIMES_KEY) || "[]"); } catch { return []; }
}
function saveExecTimes(times: number[]) {
  localStorage.setItem(EXEC_TIMES_KEY, JSON.stringify(times.slice(-MAX_EXEC_TIMES)));
}

function loadSaved(): SavedQuery[] {
  try { return JSON.parse(localStorage.getItem(SAVED_KEY) || "[]"); } catch { return []; }
}
function saveSavedQueries(queries: SavedQuery[]) {
  localStorage.setItem(SAVED_KEY, JSON.stringify(queries));
}

function loadTabs(): EditorTab[] | null {
  try {
    const raw = localStorage.getItem(TABS_KEY);
    if (!raw) return null;
    return JSON.parse(raw);
  } catch { return null; }
}
function saveTabs(tabs: EditorTab[]) {
  localStorage.setItem(TABS_KEY, JSON.stringify(tabs.map((t) => ({ ...t, results: [] }))));
}

let tabCounter = 0;
function newTab(name?: string, code?: string): EditorTab {
  tabCounter++;
  return { id: `tab-${Date.now()}-${tabCounter}`, name: name ?? `Query ${tabCounter}`, code: code ?? "", results: [], activeResultIdx: 0 };
}

/* ── component ───────────────────────────────────────────────────────────── */

export default function QueryPage() {
  const { auth } = useAuth();
  const muiTheme = useTheme();
  const cmTheme = muiTheme.palette.mode === "dark" ? "dark" : "light";

  // Tabs (QE-1)
  const [tabs, setTabs] = useState<EditorTab[]>(() => {
    const saved = loadTabs();
    if (saved && saved.length > 0) { tabCounter = saved.length; return saved; }
    return [newTab("Query 1", "SELECT * FROM trades WHERE symbol = 1 LIMIT 100")];
  });
  const [activeTabId, setActiveTabId] = useState(tabs[0].id);
  const activeTab = tabs.find((t) => t.id === activeTabId) ?? tabs[0];

  const updateTab = useCallback((id: string, patch: Partial<EditorTab>) => {
    setTabs((prev) => { const next = prev.map((t) => (t.id === id ? { ...t, ...patch } : t)); saveTabs(next); return next; });
  }, []);

  const addTab = useCallback(() => {
    const t = newTab();
    setTabs((prev) => { const next = [...prev, t]; saveTabs(next); return next; });
    setActiveTabId(t.id);
  }, []);

  const closeTab = useCallback((id: string) => {
    setTabs((prev) => {
      if (prev.length <= 1) return prev;
      const next = prev.filter((t) => t.id !== id);
      saveTabs(next);
      if (activeTabId === id) setActiveTabId(next[Math.max(0, prev.findIndex((t) => t.id === id) - 1)].id);
      return next;
    });
  }, [activeTabId]);

  // UI state
  const [loading, setLoading] = useState(false);
  const [history, setHistory] = useState<string[]>([]);
  const [showHistory, setShowHistory] = useState(false);
  const [snackMsg, setSnackMsg] = useState<string | null>(null);
  const [exportAnchor, setExportAnchor] = useState<null | HTMLElement>(null);
  const [showShortcuts, setShowShortcuts] = useState(false);
  const [sqlSchema, setSqlSchema] = useState<Record<string, string[]>>({});
  const [historySearch, setHistorySearch] = useState("");
  const [pinnedQueries, setPinnedQueries] = useState<Set<string>>(() => loadPins());
  const [execTimes, setExecTimes] = useState<number[]>(() => loadExecTimes());
  const [savedQueries, setSavedQueries] = useState<SavedQuery[]>(() => loadSaved());
  const [showSaved, setShowSaved] = useState(false);
  const [editorHeight, setEditorHeight] = useState(180);
  const [viewMode, setViewMode] = useState<"table" | "chart">("table");
  const editorRef = useRef<ReactCodeMirrorRef>(null);
  const abortRef = useRef<AbortController | null>(null);

  const handleResize = useCallback((dy: number) => { setEditorHeight((h) => Math.max(80, Math.min(600, h + dy))); }, []);

  useEffect(() => { setHistory(loadHistory()); }, []);

  // Schema fetch
  const fetchSchema = useCallback(() => {
    querySQL("SHOW TABLES", auth?.apiKey)
      .then((r) => {
        const tables = (r.data ?? []).map((row: (string | number)[]) => String(row[0]));
        return Promise.all(
          tables.map((t: string) =>
            querySQL(`DESCRIBE ${t}`, auth?.apiKey)
              .then((d) => [t, (d.data ?? []).map((row: (string | number)[]) => String(row[0]))] as [string, string[]])
              .catch(() => [t, []] as [string, string[]])
          )
        );
      })
      .then((entries) => setSqlSchema(Object.fromEntries(entries)))
      .catch(() => {});
  }, [auth]);

  useEffect(() => { fetchSchema(); }, [fetchSchema]);

  const insertAtCursor = useCallback((text: string) => {
    const view = editorRef.current?.view;
    if (view) { const pos = view.state.selection.main.head; view.dispatch({ changes: { from: pos, insert: text } }); view.focus(); }
  }, []);

  const getSelectedOrFullText = useCallback(() => {
    const view = editorRef.current?.view;
    if (view) {
      const sel = view.state.sliceDoc(view.state.selection.main.from, view.state.selection.main.to);
      if (sel.trim()) return sel.trim();
    }
    return activeTab.code;
  }, [activeTab.code]);

  // Current result for export/copy
  const currentResult = activeTab.results[activeTab.activeResultIdx]?.result ?? null;

  const copyToClipboard = useCallback(() => {
    if (!currentResult) return;
    const tsv = [currentResult.columns.join("\t"), ...currentResult.data.map((r) => r.map(String).join("\t"))].join("\n");
    navigator.clipboard.writeText(tsv).then(() => setSnackMsg("Copied to clipboard"));
  }, [currentResult]);

  const exportCSV = useCallback(() => {
    if (!currentResult) return;
    downloadFile(toCSV(currentResult.columns, currentResult.data), "query_result.csv", "text/csv");
    setExportAnchor(null);
  }, [currentResult]);

  const exportJSON = useCallback(() => {
    if (!currentResult) return;
    downloadFile(toJSON(currentResult.columns, currentResult.data), "query_result.json", "application/json");
    setExportAnchor(null);
  }, [currentResult]);

  // Run — QE-9: multi-statement sequential execution
  const run = useCallback(async () => {
    const queryText = getSelectedOrFullText();
    const statements = splitStatements(queryText);
    if (statements.length === 0) return;

    const controller = new AbortController();
    abortRef.current = controller;
    setLoading(true);
    const results: StatementResult[] = [];

    for (const stmt of statements) {
      if (controller.signal.aborted) {
        results.push({ sql: stmt, error: "Cancelled" });
        break;
      }
      try {
        const r = await querySQL(stmt, auth?.apiKey, controller.signal);
        results.push({ sql: stmt, result: r });
      } catch (e: unknown) {
        if (e instanceof DOMException && e.name === "AbortError") {
          results.push({ sql: stmt, error: "Cancelled" });
          break;
        }
        results.push({ sql: stmt, error: e instanceof Error ? e.message : "Unknown error" });
      }
    }

    abortRef.current = null;

    for (const stmt of statements) {
      try {
        const r = await querySQL(stmt, auth?.apiKey);
        results.push({ sql: stmt, result: r });
      } catch (e: unknown) {
        results.push({ sql: stmt, error: e instanceof Error ? e.message : "Unknown error" });
      }
    }

    updateTab(activeTab.id, { results, activeResultIdx: results.length - 1 });

    // QE-8: highlight error line in editor
    const view = editorRef.current?.view;
    if (view) {
      const firstError = results.find((r) => r.error);
      if (firstError) {
        const lineNum = parseErrorLine(firstError.error!);
        if (lineNum && lineNum <= view.state.doc.lines) {
          const lineObj = view.state.doc.line(lineNum);
          view.dispatch({ effects: setErrorMarker.of({ from: lineObj.from, to: lineObj.to }) });
        } else {
          view.dispatch({ effects: setErrorMarker.of(null) });
        }
      } else {
        view.dispatch({ effects: setErrorMarker.of(null) });
      }
    }

    // Record exec times (QE-15)
    const newTimes = results.filter((r) => r.result).map((r) => r.result!.execution_time_us);
    if (newTimes.length > 0) {
      setExecTimes((prev) => { const next = [...prev, ...newTimes].slice(-MAX_EXEC_TIMES); saveExecTimes(next); return next; });
    }

    // Save to history
    const updated = [queryText, ...history.filter((h) => h !== queryText)].slice(0, MAX_HISTORY);
    setHistory(updated);
    saveHistory(updated);
    setLoading(false);
  }, [getSelectedOrFullText, auth, history, activeTab.id, updateTab]);

  const cancelRun = useCallback(() => {
    abortRef.current?.abort();
  }, []);

  // Rename tab on double-click
  const [renamingTabId, setRenamingTabId] = useState<string | null>(null);
  const [renameValue, setRenameValue] = useState("");

  return (
    <Box sx={{ display: "flex", height: "calc(100vh - 96px)" }}>
      <SchemaSidebar schema={sqlSchema} onRefresh={fetchSchema} onInsert={insertAtCursor} />
      <Box sx={{ display: "flex", flexDirection: "column", gap: 2, flex: 1, minWidth: 0 }}>

        {/* ── Tab bar (QE-1) ─────────────────────────────────────────────── */}
        <Box sx={{ display: "flex", alignItems: "center", borderBottom: "1px solid", borderColor: "divider" }}>
          <Tabs
            value={activeTabId}
            onChange={(_, v) => setActiveTabId(v)}
            variant="scrollable"
            scrollButtons="auto"
            sx={{ flex: 1, minHeight: 32, "& .MuiTab-root": { minHeight: 32, py: 0, px: 1.5, fontSize: 12, textTransform: "none" } }}
          >
            {tabs.map((t) => (
              <Tab
                key={t.id}
                value={t.id}
                label={
                  renamingTabId === t.id ? (
                    <input
                      autoFocus
                      value={renameValue}
                      onChange={(e) => setRenameValue(e.target.value)}
                      onBlur={() => { updateTab(t.id, { name: renameValue || t.name }); setRenamingTabId(null); }}
                      onKeyDown={(e) => { if (e.key === "Enter") { updateTab(t.id, { name: renameValue || t.name }); setRenamingTabId(null); } }}
                      style={{ background: "transparent", border: "none", color: "inherit", fontSize: 12, width: 80, outline: "none" }}
                    />
                  ) : (
                    <Box sx={{ display: "flex", alignItems: "center", gap: 0.5 }}
                      onDoubleClick={(e) => { e.stopPropagation(); setRenamingTabId(t.id); setRenameValue(t.name); }}>
                      <span>{t.name}</span>
                      {tabs.length > 1 && (
                        <CloseIcon
                          sx={{ fontSize: 14, opacity: 0.5, "&:hover": { opacity: 1 } }}
                          onClick={(e) => { e.stopPropagation(); closeTab(t.id); }}
                        />
                      )}
                    </Box>
                  )
                }
              />
            ))}
          </Tabs>
          <Tooltip title="New tab">
            <IconButton size="small" onClick={addTab} sx={{ mr: 1 }}><AddIcon fontSize="small" /></IconButton>
          </Tooltip>
        </Box>

        {/* ── Editor card ────────────────────────────────────────────────── */}
        <Card sx={{ p: 0, overflow: "hidden" }}>
          <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between", px: 2, py: 1, borderBottom: "1px solid rgba(255, 255, 255, 0.08)" }}>
            <Typography variant="body2" color="text.secondary">SQL Editor</Typography>
            <Box sx={{ display: "flex", gap: 1 }}>
              <Tooltip title="Keyboard shortcuts">
                <IconButton size="small" onClick={() => setShowShortcuts(!showShortcuts)}><KeyboardIcon fontSize="small" /></IconButton>
              </Tooltip>
              <Button size="small" startIcon={<HistoryIcon />} onClick={() => setShowHistory(!showHistory)} sx={{ textTransform: "none" }}>History</Button>
              <Button size="small" startIcon={<BookmarkIcon />} onClick={() => setShowSaved(!showSaved)} sx={{ textTransform: "none" }}>Saved</Button>
              <Tooltip title="Save current query">
                <IconButton size="small" onClick={() => {
                  const code = activeTab.code.trim();
                  if (!code) return;
                  const name = prompt("Query name:");
                  if (!name) return;
                  setSavedQueries((prev) => { const next = [...prev.filter((q) => q.name !== name), { name, sql: code }]; saveSavedQueries(next); return next; });
                  setSnackMsg(`Saved "${name}"`);
                }}><SaveIcon fontSize="small" /></IconButton>
              </Tooltip>
              <Button variant="contained" size="small" startIcon={loading ? <StopIcon /> : <PlayArrowIcon />} onClick={loading ? cancelRun : run} color={loading ? "error" : "primary"} sx={{ textTransform: "none" }}>
                {loading ? "Cancel" : "Run"} {!loading && <Typography variant="caption" sx={{ ml: 1, opacity: 0.7 }}>⌘↵</Typography>}
              </Button>
            </Box>
          </Box>
          {showShortcuts && (
            <Box sx={{ px: 2, py: 1.5, borderBottom: "1px solid rgba(255, 255, 255, 0.08)", bgcolor: "rgba(255, 255, 255, 0.03)" }}>
              {[["⌘/Ctrl + Enter", "Run query (or selected text)"], ["Ctrl + Space", "Autocomplete"], ["⌘/Ctrl + /", "Toggle comment"]].map(([key, desc]) => (
                <Box key={key} sx={{ display: "flex", gap: 2, py: 0.3 }}>
                  <Typography variant="caption" sx={{ fontFamily: "'JetBrains Mono', monospace", fontWeight: 700, minWidth: 180 }}>{key}</Typography>
                  <Typography variant="caption" color="text.secondary">{desc}</Typography>
                </Box>
              ))}
            </Box>
          )}
          {showHistory && history.length > 0 && (
            <Box sx={{ maxHeight: 200, overflow: "auto", borderBottom: "1px solid rgba(255, 255, 255, 0.08)" }}>
              <Box sx={{ px: 1, pt: 1, pb: 0.5 }}>
                <Box sx={{ display: "flex", alignItems: "center", gap: 0.5, px: 1, py: 0.5, borderRadius: 1, bgcolor: "action.hover" }}>
                  <SearchIcon sx={{ fontSize: 16, color: "text.secondary" }} />
                  <input
                    placeholder="Search history…"
                    value={historySearch}
                    onChange={(e) => setHistorySearch(e.target.value)}
                    style={{ background: "transparent", border: "none", color: "inherit", fontSize: 12, flex: 1, outline: "none", fontFamily: "'JetBrains Mono', monospace" }}
                  />
                </Box>
              </Box>
              <List dense disablePadding>
                {/* Pinned first, then rest */}
                {[...history]
                  .sort((a, b) => (pinnedQueries.has(b) ? 1 : 0) - (pinnedQueries.has(a) ? 1 : 0))
                  .filter((h) => !historySearch || h.toLowerCase().includes(historySearch.toLowerCase()))
                  .map((h, i) => (
                  <ListItemButton key={i} onClick={() => { updateTab(activeTab.id, { code: h }); setShowHistory(false); setHistorySearch(""); }}>
                    <IconButton
                      size="small"
                      sx={{ mr: 0.5, p: 0.25 }}
                      onClick={(e) => {
                        e.stopPropagation();
                        setPinnedQueries((prev) => {
                          const next = new Set(prev);
                          if (next.has(h)) next.delete(h); else next.add(h);
                          savePins(next);
                          return next;
                        });
                      }}
                    >
                      {pinnedQueries.has(h)
                        ? <PushPinIcon sx={{ fontSize: 14, color: "primary.main" }} />
                        : <PushPinOutlinedIcon sx={{ fontSize: 14, opacity: 0.4 }} />}
                    </IconButton>
                    <ListItemText primary={h} primaryTypographyProps={{ fontSize: 12, fontFamily: "'JetBrains Mono', monospace", noWrap: true }} />
                  </ListItemButton>
                ))}
              </List>
            </Box>
          )}
          {showSaved && savedQueries.length > 0 && (
            <Box sx={{ maxHeight: 150, overflow: "auto", borderBottom: "1px solid rgba(255, 255, 255, 0.08)" }}>
              <List dense disablePadding>
                {savedQueries.map((sq) => (
                  <ListItemButton key={sq.name} onClick={() => { updateTab(activeTab.id, { code: sq.sql }); setShowSaved(false); }}>
                    <ListItemText
                      primary={sq.name}
                      secondary={sq.sql}
                      primaryTypographyProps={{ fontSize: 12, fontWeight: 600 }}
                      secondaryTypographyProps={{ fontSize: 11, fontFamily: "'JetBrains Mono', monospace", noWrap: true }}
                    />
                    <IconButton size="small" onClick={(e) => {
                      e.stopPropagation();
                      setSavedQueries((prev) => { const next = prev.filter((q) => q.name !== sq.name); saveSavedQueries(next); return next; });
                    }}><DeleteOutlineIcon sx={{ fontSize: 14 }} /></IconButton>
                  </ListItemButton>
                ))}
              </List>
            </Box>
          )}
          <CodeMirror
            ref={editorRef}
            value={activeTab.code}
            height={`${editorHeight}px`}
            extensions={[sql({ schema: sqlSchema }), autocompletion({ override: [zeptoCompletions] }), errorLineField]}
            theme={cmTheme}
            onChange={(v) => updateTab(activeTab.id, { code: v })}
            onKeyDown={(e) => { if ((e.metaKey || e.ctrlKey) && e.key === "Enter") { e.preventDefault(); run(); } }}
            basicSetup={{ lineNumbers: true, foldGutter: false }}
            style={{ fontSize: 14, fontFamily: "'JetBrains Mono', monospace" }}
          />
        </Card>

        <ResizeDivider onResize={handleResize} />

        {/* ── Results area ───────────────────────────────────────────────── */}
        {activeTab.results.length > 0 && (
          <Card sx={{ flex: 1, overflow: "hidden", display: "flex", flexDirection: "column" }}>
            {/* Multi-statement result tabs (QE-9) */}
            {activeTab.results.length > 1 && (
              <Tabs
                value={activeTab.activeResultIdx}
                onChange={(_, v) => updateTab(activeTab.id, { activeResultIdx: v })}
                variant="scrollable"
                scrollButtons="auto"
                sx={{ minHeight: 28, borderBottom: "1px solid", borderColor: "divider", "& .MuiTab-root": { minHeight: 28, py: 0, px: 1.5, fontSize: 11, textTransform: "none" } }}
              >
                {activeTab.results.map((sr, i) => (
                  <Tab key={i} label={
                    <Box sx={{ display: "flex", alignItems: "center", gap: 0.5 }}>
                      <Chip size="small" label={`#${i + 1}`} sx={{ height: 16, fontSize: 10 }} color={sr.error ? "error" : "default"} />
                      <Typography variant="caption" noWrap sx={{ maxWidth: 120 }}>{sr.sql}</Typography>
                    </Box>
                  } />
                ))}
              </Tabs>
            )}

            {/* Current statement result */}
            {(() => {
              const sr = activeTab.results[activeTab.activeResultIdx];
              if (!sr) return null;
              if (sr.error) return <Alert severity="error" variant="outlined" sx={{ m: 1 }}>{sr.error}</Alert>;
              if (!sr.result) return null;
              const r = sr.result;
              return (
                <>
                  <Box sx={{ px: 2, py: 1, borderBottom: "1px solid rgba(255, 255, 255, 0.08)", display: "flex", gap: 2, alignItems: "center" }}>
                    <Chip label={`${r.rows} rows`} size="small" variant="outlined" />
                    <Chip label={`${r.execution_time_us} μs`} size="small" color="primary" variant="outlined" />
                    <Chip label={`${r.columns.length} cols`} size="small" variant="outlined" />
                    <ExecSparkline times={execTimes} />
                    <Box sx={{ flex: 1 }} />
                    {/* QE-5: table/chart toggle */}
                    <ToggleButtonGroup size="small" exclusive value={viewMode} onChange={(_, v) => { if (v) setViewMode(v); }}>
                      <ToggleButton value="table" sx={{ py: 0.25, px: 1 }}><GridOnIcon sx={{ fontSize: 16 }} /></ToggleButton>
                      <ToggleButton value="chart" sx={{ py: 0.25, px: 1 }}><BarChartIcon sx={{ fontSize: 16 }} /></ToggleButton>
                    </ToggleButtonGroup>
                    <Tooltip title="Copy as TSV"><IconButton size="small" onClick={copyToClipboard}><ContentCopyIcon fontSize="small" /></IconButton></Tooltip>
                    <Tooltip title="Export"><IconButton size="small" onClick={(e) => setExportAnchor(e.currentTarget)}><FileDownloadIcon fontSize="small" /></IconButton></Tooltip>
                    <Menu anchorEl={exportAnchor} open={Boolean(exportAnchor)} onClose={() => setExportAnchor(null)}>
                      <MenuItem onClick={exportCSV}><ListItemIcon><TableChartIcon fontSize="small" /></ListItemIcon>Export CSV</MenuItem>
                      <MenuItem onClick={exportJSON}><ListItemIcon><DataObjectIcon fontSize="small" /></ListItemIcon>Export JSON</MenuItem>
                    </Menu>
                  </Box>
                  {viewMode === "table" ? (
                    r.columns.length === 1 && r.columns[0] === "plan" ? (
                      <ExplainView planLines={r.data.map((row) => String(row[0]))} />
                    ) : (
                      <PaginatedTable columns={r.columns} data={r.data} />
                    )
                  ) : (
                    <ResultChart columns={r.columns} data={r.data} />
                  )}
                </>
              );
            })()}
          </Card>
        )}

        <Snackbar open={Boolean(snackMsg)} autoHideDuration={2000} onClose={() => setSnackMsg(null)} message={snackMsg} />
      </Box>
    </Box>
  );
}
