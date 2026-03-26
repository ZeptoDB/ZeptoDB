function headers(apiKey?: string): HeadersInit {
  return apiKey?.length ? { Authorization: `Bearer ${apiKey}` } : {};
}

export async function querySQL(sql: string, apiKey?: string) {
  const res = await fetch("/api", { method: "POST", body: sql, headers: headers(apiKey) });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new Error(body?.error || `HTTP ${res.status}`);
  }
  return res.json();
}

export async function fetchStats(apiKey?: string) {
  const res = await fetch("/api/stats", { headers: headers(apiKey) });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.json();
}

export async function fetchHealth(apiKey?: string) {
  const res = await fetch("/api/health", { headers: headers(apiKey) });
  if (!res.ok) return { status: "unhealthy" };
  return res.json();
}

export async function fetchCluster(apiKey?: string) {
  const res = await fetch("/api/admin/cluster", { headers: headers(apiKey) });
  if (!res.ok) return null;
  return res.json();
}

export async function fetchNodes(apiKey?: string) {
  const res = await fetch("/api/admin/nodes", { headers: headers(apiKey) });
  if (!res.ok) return null;
  return res.json();
}

export async function fetchMetricsHistory(apiKey?: string, sinceMs?: number, limit?: number) {
  const params = new URLSearchParams();
  if (sinceMs) params.set("since", String(sinceMs));
  if (limit) params.set("limit", String(limit));
  const qs = params.toString();
  const url = qs ? `/api/admin/metrics/history?${qs}` : "/api/admin/metrics/history";
  const res = await fetch(url, { headers: headers(apiKey) });
  if (!res.ok) return null;
  return res.json();
}

// ── Admin: API Keys ─────────────────────────────────────────────────────────

export async function fetchKeys(apiKey?: string) {
  const res = await fetch("/api/admin/keys", { headers: headers(apiKey) });
  if (!res.ok) return null;
  return res.json();
}

export async function createKey(name: string, role: string, apiKey?: string) {
  const res = await fetch("/api/admin/keys", {
    method: "POST",
    headers: { ...headers(apiKey), "Content-Type": "application/json" },
    body: JSON.stringify({ name, role }),
  });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new Error(body?.error || `HTTP ${res.status}`);
  }
  return res.json();
}

export async function revokeKey(keyId: string, apiKey?: string) {
  const res = await fetch(`/api/admin/keys/${keyId}`, { method: "DELETE", headers: headers(apiKey) });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new Error(body?.error || `HTTP ${res.status}`);
  }
  return res.json();
}

// ── Admin: Queries ──────────────────────────────────────────────────────────

export async function fetchQueries(apiKey?: string) {
  const res = await fetch("/api/admin/queries", { headers: headers(apiKey) });
  if (!res.ok) return null;
  return res.json();
}

export async function killQuery(queryId: string, apiKey?: string) {
  const res = await fetch(`/api/admin/queries/${queryId}`, { method: "DELETE", headers: headers(apiKey) });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new Error(body?.error || `HTTP ${res.status}`);
  }
  return res.json();
}

// ── Admin: Audit ────────────────────────────────────────────────────────────

export async function fetchAudit(apiKey?: string, n = 100) {
  const res = await fetch(`/api/admin/audit?n=${n}`, { headers: headers(apiKey) });
  if (!res.ok) return null;
  return res.json();
}

// ── Admin: API Key Usage ────────────────────────────────────────────────────

export async function fetchKeyUsage(keyId: string, apiKey?: string) {
  const res = await fetch(`/api/admin/keys/${keyId}/usage`, { headers: headers(apiKey) });
  if (!res.ok) return null;
  return res.json();
}

// ── Admin: Tenants ──────────────────────────────────────────────────────────

export async function fetchTenants(apiKey?: string) {
  const res = await fetch("/api/admin/tenants", { headers: headers(apiKey) });
  if (!res.ok) return null;
  return res.json();
}

export async function createTenant(tenantId: string, name: string, maxConcurrentQueries: number, tableNamespace: string, apiKey?: string) {
  const res = await fetch("/api/admin/tenants", {
    method: "POST",
    headers: { ...headers(apiKey), "Content-Type": "application/json" },
    body: JSON.stringify({ tenant_id: tenantId, name, max_concurrent_queries: maxConcurrentQueries, table_namespace: tableNamespace }),
  });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new Error(body?.error || `HTTP ${res.status}`);
  }
  return res.json();
}

export async function deleteTenant(tenantId: string, apiKey?: string) {
  const res = await fetch(`/api/admin/tenants/${tenantId}`, { method: "DELETE", headers: headers(apiKey) });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new Error(body?.error || `HTTP ${res.status}`);
  }
  return res.json();
}

// ── Admin: Sessions ─────────────────────────────────────────────────────────

export async function fetchSessions(apiKey?: string) {
  const res = await fetch("/api/admin/sessions", { headers: headers(apiKey) });
  if (!res.ok) return null;
  return res.json();
}

// ── Admin: Settings ─────────────────────────────────────────────────────────

export async function fetchSettings(apiKey?: string) {
  const res = await fetch("/api/admin/settings", { headers: headers(apiKey) });
  if (!res.ok) return null;
  return res.json();
}
