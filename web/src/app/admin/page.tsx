"use client";
import { useState, useCallback, useEffect, useMemo } from "react";
import {
  Box, Paper, Typography, Table, TableHead, TableRow, TableCell, TableBody,
  TableContainer, Chip, Button, TextField, Select, MenuItem, IconButton,
  Tooltip, Alert, Tabs, Tab, Dialog, DialogTitle, DialogContent, DialogActions,
  InputAdornment, Stack,
} from "@mui/material";
import DeleteIcon from "@mui/icons-material/Delete";
import AddIcon from "@mui/icons-material/Add";
import ContentCopyIcon from "@mui/icons-material/ContentCopy";
import RefreshIcon from "@mui/icons-material/Refresh";
import StopIcon from "@mui/icons-material/Stop";
import SearchIcon from "@mui/icons-material/Search";
import VisibilityIcon from "@mui/icons-material/Visibility";
import EditIcon from "@mui/icons-material/Edit";
import FileDownloadIcon from "@mui/icons-material/FileDownload";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import {
  fetchKeys, createKey, revokeKey, updateKey, fetchQueries, killQuery, fetchAudit,
  fetchTenants, createTenant, deleteTenant, fetchSessions, fetchKeyUsage
} from "@/lib/api";
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
  const [newSymbols, setNewSymbols] = useState("");
  const [newTables, setNewTables] = useState("");
  const [newTenantId, setNewTenantId] = useState("");
  const [newExpiryDays, setNewExpiryDays] = useState("");
  const [createdKey, setCreatedKey] = useState<string | null>(null);

  // ── Edit Key Dialog ──
  const [editOpen, setEditOpen] = useState(false);
  const [editKey, setEditKey] = useState<any>(null);
  const [editSymbols, setEditSymbols] = useState("");
  const [editTables, setEditTables] = useState("");
  const [editTenantId, setEditTenantId] = useState("");
  const [editExpiryDays, setEditExpiryDays] = useState("");
  const [editEnabled, setEditEnabled] = useState(true);

  // ── Key Usage Dialog ──
  const [usageOpen, setUsageOpen] = useState(false);
  const [selectedKeyId, setSelectedKeyId] = useState<string | null>(null);

  // ── Create Tenant Dialog ──
  const [tenantDlgOpen, setTenantDlgOpen] = useState(false);
  const [tId, setTId] = useState("");
  const [tName, setTName] = useState("");
  const [tMcq, setTMcq] = useState("10");
  const [tNs, setTNs] = useState("");

  // ── Audit Filters ──
  const [auditSearch, setAuditSearch] = useState("");
  const [auditRoleFilter, setAuditRoleFilter] = useState("ALL");

  const { data: keys, refetch: refetchKeys } = useQuery({
    queryKey: ["admin-keys"], queryFn: () => fetchKeys(auth?.apiKey), refetchInterval: 10000,
  });
  const { data: queries, refetch: refetchQueries } = useQuery({
    queryKey: ["admin-queries"], queryFn: () => fetchQueries(auth?.apiKey), refetchInterval: 3000,
  });
  const { data: audit, refetch: refetchAudit } = useQuery({
    queryKey: ["admin-audit"], queryFn: () => fetchAudit(auth?.apiKey, 500), refetchInterval: 5000,
  });
  const { data: tenants, refetch: refetchTenants } = useQuery({
    queryKey: ["admin-tenants"], queryFn: () => fetchTenants(auth?.apiKey), refetchInterval: 10000,
  });
  const { data: sessions, refetch: refetchSessions } = useQuery({
    queryKey: ["admin-sessions"], queryFn: () => fetchSessions(auth?.apiKey), refetchInterval: 5000,
  });

  const { data: keyUsage } = useQuery({
    queryKey: ["admin-key-usage", selectedKeyId],
    queryFn: () => selectedKeyId ? fetchKeyUsage(selectedKeyId, auth?.apiKey) : null,
    enabled: !!selectedKeyId,
  });

  const handleCreate = useCallback(async () => {
    setError(null);
    try {
      const symbols = newSymbols.trim() ? newSymbols.split(",").map(s => s.trim()).filter(Boolean) : undefined;
      const tables = newTables.trim() ? newTables.split(",").map(s => s.trim()).filter(Boolean) : undefined;
      const tenantId = newTenantId.trim() || undefined;
      const expiresAtNs = newExpiryDays.trim()
        ? Date.now() * 1e6 + parseInt(newExpiryDays) * 86400 * 1e9
        : undefined;
      const r = await createKey(newName, newRole, auth?.apiKey, symbols, tables, tenantId, expiresAtNs);
      setCreatedKey(r.key);
      setNewName(""); setNewSymbols(""); setNewTables(""); setNewTenantId(""); setNewExpiryDays("");
      refetchKeys();
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : "Failed to create key");
    }
  }, [newName, newRole, newSymbols, newTables, newTenantId, newExpiryDays, auth, refetchKeys]);

  const handleEdit = useCallback(async () => {
    if (!editKey) return;
    setError(null);
    try {
      const patch: Record<string, unknown> = {};
      patch.symbols = editSymbols.trim() ? editSymbols.split(",").map(s => s.trim()).filter(Boolean) : [];
      patch.tables = editTables.trim() ? editTables.split(",").map(s => s.trim()).filter(Boolean) : [];
      patch.tenant_id = editTenantId.trim();
      patch.enabled = editEnabled;
      patch.expires_at_ns = editExpiryDays.trim()
        ? Date.now() * 1e6 + parseInt(editExpiryDays) * 86400 * 1e9
        : 0;
      await updateKey(editKey.id, patch, auth?.apiKey);
      setSuccess("Key updated");
      setEditOpen(false);
      refetchKeys();
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : "Failed to update key");
    }
  }, [editKey, editSymbols, editTables, editTenantId, editEnabled, editExpiryDays, auth, refetchKeys]);

  const openEdit = useCallback((k: any) => {
    setEditKey(k);
    setEditSymbols((k.allowed_symbols ?? []).join(", "));
    setEditTables((k.allowed_tables ?? []).join(", "));
    setEditTenantId(k.tenant_id ?? "");
    setEditEnabled(k.enabled);
    setEditExpiryDays(k.expires_at_ns > 0 ? String(Math.max(1, Math.round((k.expires_at_ns - Date.now() * 1e6) / 86400e9))) : "");
    setEditOpen(true);
  }, []);

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

  const handleCreateTenant = useCallback(async () => {
    setError(null);
    try {
      await createTenant(tId, tName, parseInt(tMcq) || 0, tNs, auth?.apiKey);
      setSuccess("Tenant created");
      setTenantDlgOpen(false);
      setTId(""); setTName(""); setTNs(""); setTMcq("10");
      refetchTenants();
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : "Failed to create tenant");
    }
  }, [tId, tName, tMcq, tNs, auth, refetchTenants]);

  const handleDeleteTenant = useCallback(async (id: string) => {
    setError(null);
    try {
      await deleteTenant(id, auth?.apiKey);
      setSuccess("Tenant deleted");
      refetchTenants();
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : "Failed to delete tenant");
    }
  }, [auth, refetchTenants]);

  // Derived filtered audit
  const filteredAudit = useMemo(() => {
    if (!audit) return [];
    let list = audit;
    if (auditRoleFilter !== "ALL") {
      list = list.filter((e: any) => e.role === auditRoleFilter);
    }
    if (auditSearch.trim()) {
      const q = auditSearch.toLowerCase();
      list = list.filter((e: any) =>
        e.subject.toLowerCase().includes(q) ||
        e.action.toLowerCase().includes(q) ||
        e.detail.toLowerCase().includes(q) ||
        e.from.toLowerCase().includes(q)
      );
    }
    return list;
  }, [audit, auditSearch, auditRoleFilter]);

  const handleDownloadCsv = () => {
    if (!filteredAudit.length) return;
    const header = "Time,Subject,Role,Action,Detail,From\n";
    const csv = filteredAudit.map((e: any) =>
      `${e.ts},"${e.subject}","${e.role}","${e.action}","${e.detail.replace(/"/g, '""')}","${e.from}"`
    ).join("\n");
    const blob = new Blob([header + csv], { type: "text/csv;charset=utf-8;" });
    const link = document.createElement("a");
    const url = URL.createObjectURL(blob);
    link.href = url;
    link.download = `audit_${Date.now()}.csv`;
    link.click();
    URL.revokeObjectURL(url);
  };

  useEffect(() => {
    if (!success) return;
    const t = setTimeout(() => setSuccess(null), 3000);
    return () => clearTimeout(t);
  }, [success]);

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 3, maxWidth: 1200, mx: "auto" }}>
      <Typography variant="h5" fontWeight={600} color="text.primary">Admin & Governance</Typography>

      {error && <Alert severity="error" onClose={() => setError(null)}>{error}</Alert>}
      {success && <Alert severity="success" onClose={() => setSuccess(null)}>{success}</Alert>}

      <Tabs value={tab} onChange={(_, v) => setTab(v)} variant="scrollable" scrollButtons="auto">
        <Tab label={keys ? `API Keys (${keys.length})` : "API Keys"} sx={{ textTransform: "none" }} />
        <Tab label={queries ? `Queries (${queries.length})` : "Queries"} sx={{ textTransform: "none" }} />
        <Tab label="Audit Log" sx={{ textTransform: "none" }} />
        <Tab label="Roles & Permissions" sx={{ textTransform: "none" }} />
        <Tab label={tenants ? `Tenants (${tenants.length})` : "Tenants"} sx={{ textTransform: "none" }} />
        <Tab label={sessions ? `Sessions (${sessions.length})` : "Sessions"} sx={{ textTransform: "none" }} />
      </Tabs>

      {/* ── API Keys Tab ── */}
      {tab === 0 && (
        <Paper>
          <Box sx={{ px: 2, py: 1.5, borderBottom: "1px solid", borderColor: "divider", display: "flex", justifyContent: "space-between", alignItems: "center" }}>
            <Typography variant="body2" color="text.secondary">API Keys</Typography>
            <Box sx={{ display: "flex", gap: 1 }}>
              <Tooltip title="Refresh"><IconButton size="small" onClick={() => refetchKeys()}><RefreshIcon fontSize="small" /></IconButton></Tooltip>
              <Button size="small" variant="contained" startIcon={<AddIcon />} onClick={() => { setDlgOpen(true); setCreatedKey(null); }} sx={{ textTransform: "none" }}>
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
                  <TableCell sx={HEADER}>Scope</TableCell>
                  <TableCell sx={HEADER}>Tenant</TableCell>
                  <TableCell sx={HEADER}>Expires</TableCell>
                  <TableCell sx={HEADER}>Last Used</TableCell>
                  <TableCell sx={HEADER} align="right">Actions</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                {(keys ?? []).map((k: any) => {
                  const expired = k.expires_at_ns > 0 && Date.now() * 1e6 > k.expires_at_ns;
                  const scopeCount = (k.allowed_symbols?.length ?? 0) + (k.allowed_tables?.length ?? 0);
                  return (
                    <TableRow key={k.id} hover sx={expired ? { opacity: 0.5 } : undefined}>
                      <TableCell sx={MONO}>{k.name}</TableCell>
                      <TableCell sx={{ ...MONO, color: "text.secondary" }}>{k.id}</TableCell>
                      <TableCell><Chip label={k.role} size="small" color={roleColor(k.role)} variant="outlined" /></TableCell>
                      <TableCell>
                        <Chip
                          label={expired ? "Expired" : k.enabled ? "Active" : "Revoked"}
                          size="small"
                          color={expired ? "warning" : k.enabled ? "success" : "default"}
                          variant="outlined"
                        />
                      </TableCell>
                      <TableCell>
                        {scopeCount > 0 ? (
                          <Tooltip title={[
                            ...(k.allowed_symbols?.length ? [`Symbols: ${k.allowed_symbols.join(", ")}`] : []),
                            ...(k.allowed_tables?.length ? [`Tables: ${k.allowed_tables.join(", ")}`] : []),
                          ].join("\n")}>
                            <Chip label={`${scopeCount} restriction${scopeCount > 1 ? "s" : ""}`} size="small" variant="outlined" />
                          </Tooltip>
                        ) : (
                          <Typography variant="caption" color="text.secondary">Unrestricted</Typography>
                        )}
                      </TableCell>
                      <TableCell sx={{ ...MONO, color: "text.secondary", fontSize: 12 }}>{k.tenant_id || "—"}</TableCell>
                      <TableCell sx={{ fontSize: 12, color: expired ? "error.main" : "text.secondary" }}>
                        {k.expires_at_ns > 0 ? timeAgo(k.expires_at_ns).replace(" ago", " left").replace("-", "") : "Never"}
                      </TableCell>
                      <TableCell sx={{ fontSize: 12, color: "text.secondary" }}>{k.last_used_ns ? timeAgo(k.last_used_ns) : "Never"}</TableCell>
                      <TableCell align="right">
                        <Tooltip title="View Usage">
                          <IconButton size="small" color="primary" onClick={() => { setSelectedKeyId(k.id); setUsageOpen(true); }}><VisibilityIcon fontSize="small" /></IconButton>
                        </Tooltip>
                        {k.enabled && !expired && (
                          <Tooltip title="Edit">
                            <IconButton size="small" color="info" onClick={() => openEdit(k)}><EditIcon fontSize="small" /></IconButton>
                          </Tooltip>
                        )}
                        {k.enabled && (
                          <Tooltip title="Revoke">
                            <IconButton size="small" color="error" onClick={() => handleRevoke(k.id)}><DeleteIcon fontSize="small" /></IconButton>
                          </Tooltip>
                        )}
                      </TableCell>
                    </TableRow>
                  );
                })}
                {(!keys || keys.length === 0) && (
                  <TableRow><TableCell colSpan={9} sx={{ textAlign: "center", color: "text.secondary", py: 4 }}>
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
        <Paper>
          <Box sx={{ px: 2, py: 1.5, borderBottom: "1px solid", borderColor: "divider", display: "flex", justifyContent: "space-between", alignItems: "center" }}>
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
                {(queries ?? []).map((q: any) => (
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
        <Paper>
          <Box sx={{ p: 2, borderBottom: "1px solid", borderColor: "divider", display: "flex", flexWrap: "wrap", justifyContent: "space-between", alignItems: "center", gap: 2 }}>
            <Stack direction="row" spacing={2} sx={{ flex: 1, minWidth: 300 }}>
              <TextField 
                size="small" 
                placeholder="Search subject, action, detail..." 
                value={auditSearch} 
                onChange={(e) => setAuditSearch(e.target.value)}
                autoComplete="off"
                sx={{ flex: 1, maxWidth: 400 }}
                InputProps={{
                  startAdornment: <InputAdornment position="start"><SearchIcon fontSize="small" /></InputAdornment>,
                }}
              />
              <Select size="small" value={auditRoleFilter} onChange={(e) => setAuditRoleFilter(e.target.value)} sx={{ minWidth: 120 }}>
                <MenuItem value="ALL">All Roles</MenuItem>
                {ROLES.map((r) => <MenuItem key={r} value={r}>{r}</MenuItem>)}
              </Select>
            </Stack>
            <Box sx={{ display: "flex", gap: 1 }}>
              <Button size="small" variant="outlined" startIcon={<FileDownloadIcon />} onClick={handleDownloadCsv}>Export CSV</Button>
              <Tooltip title="Refresh"><IconButton size="small" onClick={() => refetchAudit()}><RefreshIcon fontSize="small" /></IconButton></Tooltip>
            </Box>
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
                {filteredAudit.map((e: any, i: number) => (
                  <TableRow key={i} hover>
                    <TableCell sx={{ fontSize: 11, color: "text.secondary", whiteSpace: "nowrap" }}>{e.ts ? timeAgo(e.ts) : "—"}</TableCell>
                    <TableCell sx={MONO}>{e.subject}</TableCell>
                    <TableCell><Chip label={e.role} size="small" variant="outlined" sx={{ height: 20 }} /></TableCell>
                    <TableCell sx={MONO}>{e.action}</TableCell>
                    <TableCell sx={{ ...MONO, maxWidth: 300, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>{e.detail}</TableCell>
                    <TableCell sx={{ ...MONO, color: "text.secondary" }}>{e.from}</TableCell>
                  </TableRow>
                ))}
                {filteredAudit.length === 0 && (
                  <TableRow><TableCell colSpan={6} sx={{ textAlign: "center", color: "text.secondary", py: 4 }}>
                    {audit === null ? "Admin access required" : "No audit events matching criteria"}
                  </TableCell></TableRow>
                )}
              </TableBody>
            </Table>
          </TableContainer>
        </Paper>
      )}

      {/* ── Roles & Permissions Tab ── */}
      {tab === 3 && (
        <Paper>
          <Box sx={{ px: 2, py: 1.5, borderBottom: "1px solid", borderColor: "divider" }}>
             <Typography variant="body2" color="text.secondary">Default Role Permissions Matrix</Typography>
          </Box>
          <TableContainer>
            <Table size="small">
              <TableHead>
                <TableRow>
                  <TableCell sx={HEADER}>Role</TableCell>
                  <TableCell sx={HEADER}>Read (SELECT)</TableCell>
                  <TableCell sx={HEADER}>Write (INSERT/DELETE)</TableCell>
                  <TableCell sx={HEADER}>DDL (CREATE/DROP)</TableCell>
                  <TableCell sx={HEADER}>Admin</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                <TableRow hover>
                  <TableCell><Chip label="admin" size="small" color="error" variant="outlined" /></TableCell>
                  <TableCell>✅</TableCell><TableCell>✅</TableCell><TableCell>✅</TableCell><TableCell>✅ (Keys, Tenants, Clusters)</TableCell>
                </TableRow>
                <TableRow hover>
                  <TableCell><Chip label="writer" size="small" color="warning" variant="outlined" /></TableCell>
                  <TableCell>✅</TableCell><TableCell>✅</TableCell><TableCell>✅</TableCell><TableCell>❌</TableCell>
                </TableRow>
                <TableRow hover>
                  <TableCell><Chip label="reader" size="small" color="info" variant="outlined" /></TableCell>
                  <TableCell>✅</TableCell><TableCell>❌</TableCell><TableCell>❌</TableCell><TableCell>❌</TableCell>
                </TableRow>
                <TableRow hover>
                  <TableCell><Chip label="analyst" size="small" color="default" variant="outlined" /></TableCell>
                  <TableCell>✅</TableCell><TableCell>❌</TableCell><TableCell>❌</TableCell><TableCell>❌</TableCell>
                </TableRow>
                <TableRow hover>
                  <TableCell><Chip label="metrics" size="small" color="default" variant="outlined" /></TableCell>
                  <TableCell>❌</TableCell><TableCell>❌</TableCell><TableCell>❌</TableCell><TableCell>❌ (System stats only)</TableCell>
                </TableRow>
              </TableBody>
            </Table>
          </TableContainer>
        </Paper>
      )}

      {/* ── Tenants Tab ── */}
      {tab === 4 && (
        <Paper>
          <Box sx={{ px: 2, py: 1.5, borderBottom: "1px solid", borderColor: "divider", display: "flex", justifyContent: "space-between", alignItems: "center" }}>
            <Typography variant="body2" color="text.secondary">Tenants & Resource Quotas</Typography>
            <Box sx={{ display: "flex", gap: 1 }}>
              <Tooltip title="Refresh"><IconButton size="small" onClick={() => refetchTenants()}><RefreshIcon fontSize="small" /></IconButton></Tooltip>
              <Button size="small" variant="contained" startIcon={<AddIcon />} onClick={() => setTenantDlgOpen(true)} sx={{ textTransform: "none" }}>
                Create Tenant
              </Button>
            </Box>
          </Box>
          <TableContainer>
            <Table size="small">
              <TableHead>
                <TableRow>
                  <TableCell sx={HEADER}>Tenant ID</TableCell>
                  <TableCell sx={HEADER}>Name</TableCell>
                  <TableCell sx={HEADER}>Namespace</TableCell>
                  <TableCell sx={HEADER}>Concurrent Q</TableCell>
                  <TableCell sx={HEADER}>Total/Active</TableCell>
                  <TableCell sx={HEADER}>Rejected</TableCell>
                  <TableCell sx={HEADER} align="right">Actions</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                {(tenants ?? []).map((t: any) => (
                  <TableRow key={t.tenant_id} hover>
                    <TableCell sx={MONO}>{t.tenant_id}</TableCell>
                    <TableCell>{t.name}</TableCell>
                    <TableCell sx={{ ...MONO, color: "text.secondary" }}>{t.table_namespace || "—"}</TableCell>
                    <TableCell>{t.max_concurrent_queries || "∞"}</TableCell>
                    <TableCell>{t.usage ? `${t.usage.total_queries} / ${t.usage.active_queries}` : "—"}</TableCell>
                    <TableCell sx={{ color: t.usage?.rejected_queries > 0 ? "error.main" : "text.secondary" }}>{t.usage?.rejected_queries || 0}</TableCell>
                    <TableCell align="right">
                      <Tooltip title="Drop Tenant">
                        <IconButton size="small" color="error" onClick={() => handleDeleteTenant(t.tenant_id)}><DeleteIcon fontSize="small" /></IconButton>
                      </Tooltip>
                    </TableCell>
                  </TableRow>
                ))}
                {(!tenants || tenants.length === 0) && (
                  <TableRow><TableCell colSpan={7} sx={{ textAlign: "center", color: "text.secondary", py: 4 }}>
                    {tenants === null ? "Admin access required" : "No tenants created"}
                  </TableCell></TableRow>
                )}
              </TableBody>
            </Table>
          </TableContainer>
        </Paper>
      )}

      {/* ── Sessions Tab ── */}
      {tab === 5 && (
        <Paper>
          <Box sx={{ px: 2, py: 1.5, borderBottom: "1px solid", borderColor: "divider", display: "flex", justifyContent: "space-between", alignItems: "center" }}>
            <Typography variant="body2" color="text.secondary">Active Connections</Typography>
            <Tooltip title="Refresh"><IconButton size="small" onClick={() => refetchSessions()}><RefreshIcon fontSize="small" /></IconButton></Tooltip>
          </Box>
          <TableContainer>
            <Table size="small">
              <TableHead>
                <TableRow>
                  <TableCell sx={HEADER}>User / Subject</TableCell>
                  <TableCell sx={HEADER}>Client IP</TableCell>
                  <TableCell sx={HEADER}>Connected</TableCell>
                  <TableCell sx={HEADER}>Last Active</TableCell>
                  <TableCell sx={HEADER}>Operations</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                {(sessions ?? []).map((s: any, i: number) => (
                  <TableRow key={i} hover>
                    <TableCell sx={MONO}>{s.user}</TableCell>
                    <TableCell sx={{ ...MONO, color: "text.secondary" }}>{s.remote_addr}</TableCell>
                    <TableCell sx={{ fontSize: 12, color: "text.secondary" }}>{timeAgo(s.connected_at_ns)}</TableCell>
                    <TableCell sx={{ fontSize: 12, color: "text.secondary" }}>{timeAgo(s.last_active_ns)}</TableCell>
                    <TableCell sx={MONO}>{s.query_count}</TableCell>
                  </TableRow>
                ))}
                {(!sessions || sessions.length === 0) && (
                  <TableRow><TableCell colSpan={5} sx={{ textAlign: "center", color: "text.secondary", py: 4 }}>
                    {sessions === null ? "Admin access required" : "No active sessions"}
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
              <Box sx={{ display: "flex", alignItems: "center", gap: 1, p: 1.5, bgcolor: "background.paper", borderRadius: 1, border: "1px solid", borderColor: "divider" }}>
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
              <TextField label="Allowed Symbols (comma-separated)" size="small" value={newSymbols}
                onChange={(e) => setNewSymbols(e.target.value)} placeholder="e.g. AAPL, MSFT (empty = all)" />
              <TextField label="Allowed Tables (comma-separated)" size="small" value={newTables}
                onChange={(e) => setNewTables(e.target.value)} placeholder="e.g. trades, quotes (empty = all)" />
              <TextField label="Tenant ID" size="small" value={newTenantId}
                onChange={(e) => setNewTenantId(e.target.value)} placeholder="e.g. hft_desk_1 (empty = no tenant)" />
              <TextField label="Expires in (days)" size="small" type="number" value={newExpiryDays}
                onChange={(e) => setNewExpiryDays(e.target.value)} placeholder="e.g. 90 (empty = never)" />
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

      {/* ── Edit Key Dialog ── */}
      <Dialog open={editOpen} onClose={() => setEditOpen(false)} maxWidth="sm" fullWidth>
        <DialogTitle>Edit API Key — {editKey?.name}</DialogTitle>
        <DialogContent sx={{ display: "flex", flexDirection: "column", gap: 2, pt: "16px !important" }}>
          <Box>
            <Typography variant="caption" color="text.secondary">Key ID</Typography>
            <Typography sx={MONO}>{editKey?.id}</Typography>
          </Box>
          <TextField label="Allowed Symbols (comma-separated)" size="small" value={editSymbols}
            onChange={(e) => setEditSymbols(e.target.value)} placeholder="empty = unrestricted" />
          <TextField label="Allowed Tables (comma-separated)" size="small" value={editTables}
            onChange={(e) => setEditTables(e.target.value)} placeholder="empty = unrestricted" />
          <TextField label="Tenant ID" size="small" value={editTenantId}
            onChange={(e) => setEditTenantId(e.target.value)} placeholder="empty = no tenant" />
          <TextField label="Extend expiry (days from now)" size="small" type="number" value={editExpiryDays}
            onChange={(e) => setEditExpiryDays(e.target.value)} placeholder="empty = never expires" />
          <Box sx={{ display: "flex", alignItems: "center", gap: 1 }}>
            <Typography variant="body2">Enabled:</Typography>
            <Chip
              label={editEnabled ? "Active" : "Disabled"}
              size="small"
              color={editEnabled ? "success" : "default"}
              variant="outlined"
              onClick={() => setEditEnabled(!editEnabled)}
              sx={{ cursor: "pointer" }}
            />
          </Box>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setEditOpen(false)} sx={{ textTransform: "none" }}>Cancel</Button>
          <Button variant="contained" onClick={handleEdit} sx={{ textTransform: "none" }}>Save Changes</Button>
        </DialogActions>
      </Dialog>
      
      {/* ── Key Usage Dialog ── */}
      <Dialog open={usageOpen} onClose={() => setUsageOpen(false)} maxWidth="sm" fullWidth>
        <DialogTitle>API Key Details</DialogTitle>
        <DialogContent dividers sx={{ display: "flex", flexDirection: "column", gap: 2 }}>
          {keyUsage ? (
            <Stack spacing={2}>
              <Box>
                <Typography variant="caption" color="text.secondary" display="block">Key ID</Typography>
                <Typography sx={MONO}>{keyUsage.id}</Typography>
              </Box>
              <Box>
                <Typography variant="caption" color="text.secondary" display="block">Key Name</Typography>
                <Typography sx={MONO}>{keyUsage.name}</Typography>
              </Box>
              <Box>
                <Typography variant="caption" color="text.secondary" display="block">Last Used</Typography>
                <Typography sx={MONO}>{keyUsage.last_used_ns ? timeAgo(keyUsage.last_used_ns) : "Never used"}</Typography>
              </Box>
              <Box>
                <Typography variant="caption" color="text.secondary" display="block">Allowed Symbols (Filter)</Typography>
                {keyUsage.allowed_symbols && keyUsage.allowed_symbols.length > 0 ? (
                  <Stack direction="row" gap={1} flexWrap="wrap" mt={1}>
                    {keyUsage.allowed_symbols.map((sym: string) => (
                      <Chip key={sym} label={sym} size="small" variant="outlined" />
                    ))}
                  </Stack>
                ) : (
                  <Typography sx={{ ...MONO, color: "text.secondary" }}>Unrestricted (All symbols)</Typography>
                )}
              </Box>
            </Stack>
          ) : (
            <Typography variant="body2" color="text.secondary" textAlign="center" py={4}>Loading usage info...</Typography>
          )}
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setUsageOpen(false)} sx={{ textTransform: "none" }}>Close</Button>
        </DialogActions>
      </Dialog>

      {/* ── Create Tenant Dialog ── */}
      <Dialog open={tenantDlgOpen} onClose={() => setTenantDlgOpen(false)} maxWidth="sm" fullWidth>
        <DialogTitle>Create Tenant</DialogTitle>
        <DialogContent sx={{ display: "flex", flexDirection: "column", gap: 3, pt: "16px !important" }}>
          <TextField label="Tenant ID (no spaces)" size="small" value={tId} onChange={(e) => setTId(e.target.value)} placeholder="e.g. hft_desk_1" autoFocus />
          <TextField label="Display Name" size="small" value={tName} onChange={(e) => setTName(e.target.value)} placeholder="e.g. High Frequency Desk" />
          <TextField label="Table Namespace (Prefix)" size="small" value={tNs} onChange={(e) => setTNs(e.target.value)} placeholder="e.g. trading." helperText="Empty means unrestricted access to all tables" />
          <TextField label="Max Concurrent Queries (0 = unlimited)" size="small" type="number" value={tMcq} onChange={(e) => setTMcq(e.target.value)} />
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setTenantDlgOpen(false)} sx={{ textTransform: "none" }}>Cancel</Button>
          <Button variant="contained" onClick={handleCreateTenant} disabled={!tId.trim()} sx={{ textTransform: "none" }}>Create Tenant</Button>
        </DialogActions>
      </Dialog>
    </Box>
  );
}
