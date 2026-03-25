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
