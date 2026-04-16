// API base path: empty when served from same origin (Docker/static),
// "/api" when running behind Next.js dev proxy.
const API = typeof window !== "undefined" && window.location.pathname.startsWith("/ui") ? "" : "/api";

function headers(apiKey?: string): HeadersInit {
  return apiKey?.length ? { Authorization: `Bearer ${apiKey}` } : {};
}

function fetchOpts(apiKey?: string, signal?: AbortSignal): RequestInit {
  return { headers: headers(apiKey), credentials: "include" as const, signal };
}

export async function querySQL(sql: string, apiKey?: string, signal?: AbortSignal) {
  const res = await fetch(`${API}/`, { method: "POST", body: sql, ...fetchOpts(apiKey, signal) });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new Error(body?.error || `HTTP ${res.status}`);
  }
  return res.json();
}

export async function fetchStats(apiKey?: string) {
  const res = await fetch(`${API}/stats`, fetchOpts(apiKey));
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.json();
}

export async function fetchHealth(apiKey?: string) {
  const res = await fetch(`${API}/health`, fetchOpts(apiKey));
  if (!res.ok) return { status: "unhealthy" };
  return res.json();
}

export async function fetchVersion(apiKey?: string) {
  const res = await fetch(`${API}/admin/version`, fetchOpts(apiKey));
  if (!res.ok) return null;
  return res.json();
}

export async function fetchCluster(apiKey?: string) {
  const res = await fetch(`${API}/admin/cluster`, fetchOpts(apiKey));
  if (!res.ok) return null;
  return res.json();
}

export async function fetchNodes(apiKey?: string) {
  const res = await fetch(`${API}/admin/nodes`, fetchOpts(apiKey));
  if (!res.ok) return null;
  return res.json();
}

export async function fetchMetricsHistory(apiKey?: string, sinceMs?: number, limit?: number) {
  const params = new URLSearchParams();
  if (sinceMs) params.set("since", String(sinceMs));
  if (limit) params.set("limit", String(limit));
  const qs = params.toString();
  const url = qs ? `${API}/admin/metrics/history?${qs}` : `${API}/admin/metrics/history`;
  const res = await fetch(url, fetchOpts(apiKey));
  if (!res.ok) return null;
  return res.json();
}

// ── Admin: API Keys ─────────────────────────────────────────────────────────

export async function fetchKeys(apiKey?: string) {
  const res = await fetch(`${API}/admin/keys`, fetchOpts(apiKey));
  if (!res.ok) return null;
  return res.json();
}

export async function createKey(
  name: string,
  role: string,
  apiKey?: string,
  symbols?: string[],
  tables?: string[],
  tenantId?: string,
  expiresAtNs?: number,
) {
  const body: Record<string, unknown> = { name, role };
  if (symbols?.length) body.symbols = symbols;
  if (tables?.length) body.tables = tables;
  if (tenantId) body.tenant_id = tenantId;
  if (expiresAtNs) body.expires_at_ns = expiresAtNs;

  const res = await fetch(`${API}/admin/keys`, {
    method: "POST",
    headers: { ...headers(apiKey), "Content-Type": "application/json" },
    credentials: "include",
    body: JSON.stringify(body),
  });
  if (!res.ok) {
    const b = await res.json().catch(() => null);
    throw new Error(b?.error || `HTTP ${res.status}`);
  }
  return res.json();
}

export async function updateKey(
  keyId: string,
  patch: {
    symbols?: string[];
    tables?: string[];
    enabled?: boolean;
    tenant_id?: string;
    expires_at_ns?: number;
  },
  apiKey?: string,
) {
  const res = await fetch(`${API}/admin/keys/${keyId}`, {
    method: "PATCH",
    headers: { ...headers(apiKey), "Content-Type": "application/json" },
    credentials: "include",
    body: JSON.stringify(patch),
  });
  if (!res.ok) {
    const b = await res.json().catch(() => null);
    throw new Error(b?.error || `HTTP ${res.status}`);
  }
  return res.json();
}

export async function revokeKey(keyId: string, apiKey?: string) {
  const res = await fetch(`${API}/admin/keys/${keyId}`, { method: "DELETE", ...fetchOpts(apiKey) });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new Error(body?.error || `HTTP ${res.status}`);
  }
  return res.json();
}

// ── Admin: Queries ──────────────────────────────────────────────────────────

export async function fetchQueries(apiKey?: string) {
  const res = await fetch(`${API}/admin/queries`, fetchOpts(apiKey));
  if (!res.ok) return null;
  return res.json();
}

export async function killQuery(queryId: string, apiKey?: string) {
  const res = await fetch(`${API}/admin/queries/${queryId}`, { method: "DELETE", ...fetchOpts(apiKey) });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new Error(body?.error || `HTTP ${res.status}`);
  }
  return res.json();
}

// ── Admin: Audit ────────────────────────────────────────────────────────────

export async function fetchAudit(apiKey?: string, n = 100) {
  const res = await fetch(`${API}/admin/audit?n=${n}`, fetchOpts(apiKey));
  if (!res.ok) return null;
  return res.json();
}

// ── Admin: API Key Usage ────────────────────────────────────────────────────

export async function fetchKeyUsage(keyId: string, apiKey?: string) {
  const res = await fetch(`${API}/admin/keys/${keyId}/usage`, fetchOpts(apiKey));
  if (!res.ok) return null;
  return res.json();
}

// ── Admin: Tenants ──────────────────────────────────────────────────────────

export async function fetchTenants(apiKey?: string) {
  const res = await fetch(`${API}/admin/tenants`, fetchOpts(apiKey));
  if (!res.ok) return null;
  return res.json();
}

export async function fetchTenant(tenantId: string, apiKey?: string) {
  const all = await fetchTenants(apiKey);
  if (!all) return null;
  return all.find((t: { tenant_id: string }) => t.tenant_id === tenantId) ?? null;
}

export async function createTenant(tenantId: string, name: string, maxConcurrentQueries: number, tableNamespace: string, apiKey?: string) {
  const res = await fetch(`${API}/admin/tenants`, {
    method: "POST",
    headers: { ...headers(apiKey), "Content-Type": "application/json" },
    credentials: "include",
    body: JSON.stringify({ tenant_id: tenantId, name, max_concurrent_queries: maxConcurrentQueries, table_namespace: tableNamespace }),
  });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new Error(body?.error || `HTTP ${res.status}`);
  }
  return res.json();
}

export async function deleteTenant(tenantId: string, apiKey?: string) {
  const res = await fetch(`${API}/admin/tenants/${tenantId}`, { method: "DELETE", ...fetchOpts(apiKey) });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new Error(body?.error || `HTTP ${res.status}`);
  }
  return res.json();
}

// ── Admin: Sessions ─────────────────────────────────────────────────────────

export async function fetchSessions(apiKey?: string) {
  const res = await fetch(`${API}/admin/sessions`, fetchOpts(apiKey));
  if (!res.ok) return null;
  return res.json();
}

// ── Admin: Settings ─────────────────────────────────────────────────────────

export async function fetchSettings(apiKey?: string) {
  const res = await fetch(`${API}/admin/settings`, fetchOpts(apiKey));
  if (!res.ok) return null;
  return res.json();
}

// ── Admin: Rebalance ────────────────────────────────────────────────────────

export async function fetchRebalanceStatus(apiKey?: string) {
  const res = await fetch(`${API}/admin/rebalance/status`, fetchOpts(apiKey));
  if (!res.ok) return null;
  return res.json();
}

export async function fetchRebalanceHistory(apiKey?: string) {
  const res = await fetch(`${API}/admin/rebalance/history`, fetchOpts(apiKey));
  if (!res.ok) return null;
  return res.json();
}

// ── License ─────────────────────────────────────────────────────────────────

export interface LicenseInfo {
  edition: string;
  features: string[];
  max_nodes: number;
  trial: boolean;
  expired: boolean;
  upgrade_url: string;
  company?: string;
  expires?: string;
}

export async function fetchLicense(apiKey?: string): Promise<LicenseInfo | null> {
  try {
    const res = await fetch(`${API}/api/license`, fetchOpts(apiKey));
    if (!res.ok) return null;
    return res.json();
  } catch { return null; }
}