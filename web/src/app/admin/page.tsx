"use client";
import { useState, useCallback, useEffect } from "react";
import {
  Box, Paper, Typography, Table, TableHead, TableRow, TableCell, TableBody,
  TableContainer, Chip, Button, TextField, Select, MenuItem, IconButton,
  Tooltip, Alert, Tabs, Tab, Dialog, DialogTitle, DialogContent, DialogActions,
} from "@mui/material";
import DeleteIcon from "@mui/icons-material/Delete";
import AddIcon from "@mui/icons-material/Add";
import ContentCopyIcon from "@mui/icons-material/ContentCopy";
import RefreshIcon from "@mui/icons-material/Refresh";
import StopIcon from "@mui/icons-material/Stop";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { fetchKeys, createKey, revokeKey, fetchQueries, killQuery, fetchAudit } from "@/lib/api";
import { useAuth } from "@/lib/auth";

const MONO = { fontFamily: "'JetBrains Mono', monospace", fontSize: 13 };
const HEADER = { fontWeight: 600, fontSize: 12, textTransform: "uppercase" as const, color: "text.secondary" };
const ROLES = ["admin", "writer", "reader", "analyst", "metrics"];

function roleColor(role: string): "error" | "warning" | "info" | "success" | "default" {
  if (role === "admin") return "error";
  if (role === "writer") return "warning";
  if (role === "reader") return "info";
  return "default";
}

function timeAgo(ns: number) {
  const sec = (Date.now() * 1e6 - ns) / 1e9;
  if (sec < 60) return `${Math.floor(sec)}s ago`;
  if (sec < 3600) return `${Math.floor(sec / 60)}m ago`;
  if (sec < 86400) return `${Math.floor(sec / 3600)}h ago`;
  return `${Math.floor(sec / 86400)}d ago`;
}

export default function AdminPage() {
  const { auth } = useAuth();
  const qc = useQueryClient();
  const [tab, setTab] = useState(0);
  const [error, setError] = useState<string | null>(null);
  const [success, setSuccess] = useState<string | null>(null);

  // ── Create Key Dialog ──
  const [dlgOpen, setDlgOpen] = useState(false);
  const [newName, setNewName] = useState("");
  const [newRole, setNewRole] = useState("reader");
  const [createdKey, setCreatedKey] = useState<string | null>(null);

  const { data: keys, refetch: refetchKeys } = useQuery({
    queryKey: ["admin-keys"], queryFn: () => fetchKeys(auth?.apiKey), refetchInterval: 10000,
  });
  const { data: queries, refetch: refetchQueries } = useQuery({
    queryKey: ["admin-queries"], queryFn: () => fetchQueries(auth?.apiKey), refetchInterval: 3000,
  });
  const { data: audit, refetch: refetchAudit } = useQuery({
    queryKey: ["admin-audit"], queryFn: () => fetchAudit(auth?.apiKey), refetchInterval: 5000,
  });

  const handleCreate = useCallback(async () => {
    setError(null);
    try {
      const r = await createKey(newName, newRole, auth?.apiKey);
      setCreatedKey(r.key);
      setNewName("");
      refetchKeys();
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : "Failed to create key");
    }
  }, [newName, newRole, auth, refetchKeys]);

  const handleRevoke = useCallback(async (id: string) => {
    setError(null);
    try {
      await revokeKey(id, auth?.apiKey);
      setSuccess("Key revoked");
      refetchKeys();
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : "Failed to revoke key");
    }
  }, [auth, refetchKeys]);

  const handleKill = useCallback(async (id: string) => {
    setError(null);
    try {
      await killQuery(id, auth?.apiKey);
      setSuccess("Query cancelled");
      refetchQueries();
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : "Failed to kill query");
    }
  }, [auth, refetchQueries]);

  // Clear success after 3s
  useEffect(() => {
    if (!success) return;
    const t = setTimeout(() => setSuccess(null), 3000);
    return () => clearTimeout(t);
  }, [success]);

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 2 }}>
      <Typography variant="h6">Admin</Typography>

      {error && <Alert severity="error" onClose={() => setError(null)}>{error}</Alert>}
      {success && <Alert severity="success" onClose={() => setSuccess(null)}>{success}</Alert>}

      <Tabs value={tab} onChange={(_, v) => setTab(v)}>
        <Tab label={`API Keys${keys ? ` (${keys.length})` : ""}`} sx={{ textTransform: "none" }} />
        <Tab label={`Queries${queries ? ` (${queries.length})` : ""}`} sx={{ textTransform: "none" }} />
        <Tab label="Audit Log" sx={{ textTransform: "none" }} />
      </Tabs>

      {/* ── API Keys Tab ── */}
      {tab === 0 && (
        <Paper sx={{ border: "1px solid #1E293B" }}>
          <Box sx={{ px: 2, py: 1, borderBottom: "1px solid #1E293B", display: "flex", justifyContent: "space-between", alignItems: "center" }}>
            <Typography variant="body2" color="text.secondary">API Keys</Typography>
            <Box sx={{ display: "flex", gap: 1 }}>
              <Tooltip title="Refresh"><IconButton size="small" onClick={() => refetchKeys()}><RefreshIcon fontSize="small" /></IconButton></Tooltip>
              <Button size="small" startIcon={<AddIcon />} onClick={() => { setDlgOpen(true); setCreatedKey(null); }} sx={{ textTransform: "none" }}>
                Create Key
              </Button>
            </Box>
          </Box>
          <TableContainer>
            <Table size="small">
              <TableHead>
                <TableRow>
                  <TableCell sx={HEADER}>Name</TableCell>
                  <TableCell sx={HEADER}>ID</TableCell>
                  <TableCell sx={HEADER}>Role</TableCell>
                  <TableCell sx={HEADER}>Status</TableCell>
                  <TableCell sx={HEADER}>Created</TableCell>
                  <TableCell sx={HEADER} align="right">Actions</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                {(keys ?? []).map((k: { id: string; name: string; role: string; enabled: boolean; created_at_ns: number }) => (
                  <TableRow key={k.id} hover>
                    <TableCell sx={MONO}>{k.name}</TableCell>
                    <TableCell sx={{ ...MONO, color: "text.secondary" }}>{k.id.slice(0, 12)}…</TableCell>
                    <TableCell><Chip label={k.role} size="small" color={roleColor(k.role)} variant="outlined" /></TableCell>
                    <TableCell><Chip label={k.enabled ? "Active" : "Revoked"} size="small" color={k.enabled ? "success" : "default"} variant="outlined" /></TableCell>
                    <TableCell sx={{ fontSize: 12, color: "text.secondary" }}>{k.created_at_ns ? timeAgo(k.created_at_ns) : "—"}</TableCell>
                    <TableCell align="right">
                      {k.enabled && (
                        <Tooltip title="Revoke">
                          <IconButton size="small" color="error" onClick={() => handleRevoke(k.id)}><DeleteIcon fontSize="small" /></IconButton>
                        </Tooltip>
                      )}
                    </TableCell>
                  </TableRow>
                ))}
                {(!keys || keys.length === 0) && (
                  <TableRow><TableCell colSpan={6} sx={{ textAlign: "center", color: "text.secondary", py: 4 }}>
                    {keys === null ? "Admin access required" : "No API keys"}
                  </TableCell></TableRow>
                )}
              </TableBody>
            </Table>
          </TableContainer>
        </Paper>
      )}

      {/* ── Active Queries Tab ── */}
      {tab === 1 && (
        <Paper sx={{ border: "1px solid #1E293B" }}>
          <Box sx={{ px: 2, py: 1, borderBottom: "1px solid #1E293B", display: "flex", justifyContent: "space-between", alignItems: "center" }}>
            <Typography variant="body2" color="text.secondary">Active Queries</Typography>
            <Tooltip title="Refresh"><IconButton size="small" onClick={() => refetchQueries()}><RefreshIcon fontSize="small" /></IconButton></Tooltip>
          </Box>
          <TableContainer>
            <Table size="small">
              <TableHead>
                <TableRow>
                  <TableCell sx={HEADER}>Query ID</TableCell>
                  <TableCell sx={HEADER}>Subject</TableCell>
                  <TableCell sx={HEADER}>SQL</TableCell>
                  <TableCell sx={HEADER}>Started</TableCell>
                  <TableCell sx={HEADER} align="right">Actions</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                {(queries ?? []).map((q: { id: string; subject: string; sql: string; started_at_ns: number }) => (
                  <TableRow key={q.id} hover>
                    <TableCell sx={{ ...MONO, color: "text.secondary" }}>{q.id}</TableCell>
                    <TableCell sx={MONO}>{q.subject}</TableCell>
                    <TableCell sx={{ ...MONO, maxWidth: 400, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>{q.sql}</TableCell>
                    <TableCell sx={{ fontSize: 12, color: "text.secondary" }}>{q.started_at_ns ? timeAgo(q.started_at_ns) : "—"}</TableCell>
                    <TableCell align="right">
                      <Tooltip title="Kill query">
                        <IconButton size="small" color="error" onClick={() => handleKill(q.id)}><StopIcon fontSize="small" /></IconButton>
                      </Tooltip>
                    </TableCell>
                  </TableRow>
                ))}
                {(!queries || queries.length === 0) && (
                  <TableRow><TableCell colSpan={5} sx={{ textAlign: "center", color: "text.secondary", py: 4 }}>
                    {queries === null ? "Admin access required" : "No active queries"}
                  </TableCell></TableRow>
                )}
              </TableBody>
            </Table>
          </TableContainer>
        </Paper>
      )}

      {/* ── Audit Log Tab ── */}
      {tab === 2 && (
        <Paper sx={{ border: "1px solid #1E293B" }}>
          <Box sx={{ px: 2, py: 1, borderBottom: "1px solid #1E293B", display: "flex", justifyContent: "space-between", alignItems: "center" }}>
            <Typography variant="body2" color="text.secondary">Audit Log (last 100)</Typography>
            <Tooltip title="Refresh"><IconButton size="small" onClick={() => refetchAudit()}><RefreshIcon fontSize="small" /></IconButton></Tooltip>
          </Box>
          <TableContainer sx={{ maxHeight: 500 }}>
            <Table size="small" stickyHeader>
              <TableHead>
                <TableRow>
                  <TableCell sx={HEADER}>Time</TableCell>
                  <TableCell sx={HEADER}>Subject</TableCell>
                  <TableCell sx={HEADER}>Role</TableCell>
                  <TableCell sx={HEADER}>Action</TableCell>
                  <TableCell sx={HEADER}>Detail</TableCell>
                  <TableCell sx={HEADER}>From</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                {(audit ?? []).map((e: { ts: number; subject: string; role: string; action: string; detail: string; from: string }, i: number) => (
                  <TableRow key={i} hover>
                    <TableCell sx={{ fontSize: 11, color: "text.secondary", whiteSpace: "nowrap" }}>{e.ts ? timeAgo(e.ts) : "—"}</TableCell>
                    <TableCell sx={MONO}>{e.subject}</TableCell>
                    <TableCell><Chip label={e.role} size="small" variant="outlined" sx={{ height: 20 }} /></TableCell>
                    <TableCell sx={MONO}>{e.action}</TableCell>
                    <TableCell sx={{ ...MONO, maxWidth: 300, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>{e.detail}</TableCell>
                    <TableCell sx={{ ...MONO, color: "text.secondary" }}>{e.from}</TableCell>
                  </TableRow>
                ))}
                {(!audit || audit.length === 0) && (
                  <TableRow><TableCell colSpan={6} sx={{ textAlign: "center", color: "text.secondary", py: 4 }}>
                    {audit === null ? "Admin access required" : "No audit events"}
                  </TableCell></TableRow>
                )}
              </TableBody>
            </Table>
          </TableContainer>
        </Paper>
      )}

      {/* ── Create Key Dialog ── */}
      <Dialog open={dlgOpen} onClose={() => setDlgOpen(false)} maxWidth="sm" fullWidth>
        <DialogTitle>Create API Key</DialogTitle>
        <DialogContent sx={{ display: "flex", flexDirection: "column", gap: 2, pt: "16px !important" }}>
          {createdKey ? (
            <Box>
              <Alert severity="success" sx={{ mb: 2 }}>Key created — copy it now, it won't be shown again.</Alert>
              <Box sx={{ display: "flex", alignItems: "center", gap: 1, p: 1.5, bgcolor: "#111827", borderRadius: 1, border: "1px solid #1E293B" }}>
                <Typography sx={{ ...MONO, flex: 1, wordBreak: "break-all" }}>{createdKey}</Typography>
                <Tooltip title="Copy">
                  <IconButton size="small" onClick={() => navigator.clipboard.writeText(createdKey)}>
                    <ContentCopyIcon fontSize="small" />
                  </IconButton>
                </Tooltip>
              </Box>
            </Box>
          ) : (
            <>
              <TextField label="Name" size="small" value={newName} onChange={(e) => setNewName(e.target.value)}
                placeholder="e.g. algo-service" autoFocus />
              <Select size="small" value={newRole} onChange={(e) => setNewRole(e.target.value)}>
                {ROLES.map((r) => <MenuItem key={r} value={r}>{r}</MenuItem>)}
              </Select>
            </>
          )}
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setDlgOpen(false)} sx={{ textTransform: "none" }}>
            {createdKey ? "Done" : "Cancel"}
          </Button>
          {!createdKey && (
            <Button variant="contained" onClick={handleCreate} disabled={!newName.trim()} sx={{ textTransform: "none" }}>
              Create
            </Button>
          )}
        </DialogActions>
      </Dialog>
    </Box>
  );
}
