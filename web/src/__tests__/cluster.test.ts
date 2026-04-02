import { describe, it, expect, vi, beforeEach } from "vitest";
import { fetchNodes, fetchCluster, fetchMetricsHistory } from "@/lib/api";

const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

beforeEach(() => mockFetch.mockReset());

// ── fetchNodes ──────────────────────────────────────────────────────────────

describe("fetchNodes", () => {
  it("returns nodes array on success", async () => {
    const data = {
      nodes: [
        { id: 0, host: "10.0.1.1", port: 8123, state: "ACTIVE", ticks_ingested: 5000, ticks_stored: 4900, queries_executed: 100 },
        { id: 1, host: "10.0.1.2", port: 8123, state: "ACTIVE", ticks_ingested: 4800, ticks_stored: 4700, queries_executed: 90 },
        { id: 2, host: "10.0.1.3", port: 8123, state: "SUSPECT", ticks_ingested: 3000, ticks_stored: 2900, queries_executed: 50 },
      ],
    };
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve(data) });

    const result = await fetchNodes("zepto_admin_key");
    expect(mockFetch).toHaveBeenCalledWith("/api/admin/nodes", {
      headers: { Authorization: "Bearer zepto_admin_key" },
      credentials: "include",
      signal: undefined,
    });
    expect(result.nodes).toHaveLength(3);
    expect(result.nodes[2].state).toBe("SUSPECT");
  });

  it("returns null on auth failure", async () => {
    mockFetch.mockResolvedValue({ ok: false, status: 403 });
    expect(await fetchNodes("reader_key")).toBeNull();
  });
});

// ── fetchCluster ────────────────────────────────────────────────────────────

describe("fetchCluster", () => {
  it("returns cluster overview", async () => {
    const data = { mode: "cluster", node_count: 3, ticks_ingested: 12800 };
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve(data) });
    const result = await fetchCluster("zepto_admin_key");
    expect(result.mode).toBe("cluster");
  });
});

// ── fetchMetricsHistory ─────────────────────────────────────────────────────

describe("fetchMetricsHistory", () => {
  it("calls /admin/metrics/history without params", async () => {
    const data = [{ timestamp_ms: 1000, node_id: 0, ticks_ingested: 100 }];
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve(data) });

    const result = await fetchMetricsHistory("zepto_key");
    expect(mockFetch).toHaveBeenCalledWith("/api/admin/metrics/history", {
      headers: { Authorization: "Bearer zepto_key" },
      credentials: "include",
      signal: undefined,
    });
    expect(result).toHaveLength(1);
  });

  it("appends since query param when provided", async () => {
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve([]) });

    await fetchMetricsHistory("zepto_key", 1711234567000);
    expect(mockFetch).toHaveBeenCalledWith(
      "/api/admin/metrics/history?since=1711234567000",
      { headers: { Authorization: "Bearer zepto_key" }, credentials: "include", signal: undefined },
    );
  });

  it("appends both since and limit params", async () => {
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve([]) });

    await fetchMetricsHistory("zepto_key", 1711234567000, 100);
    expect(mockFetch).toHaveBeenCalledWith(
      "/api/admin/metrics/history?since=1711234567000&limit=100",
      { headers: { Authorization: "Bearer zepto_key" }, credentials: "include", signal: undefined },
    );
  });

  it("appends only limit when since is not provided", async () => {
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve([]) });

    await fetchMetricsHistory("zepto_key", undefined, 50);
    expect(mockFetch).toHaveBeenCalledWith(
      "/api/admin/metrics/history?limit=50",
      { headers: { Authorization: "Bearer zepto_key" }, credentials: "include", signal: undefined },
    );
  });

  it("returns null on auth failure", async () => {
    mockFetch.mockResolvedValue({ ok: false, status: 403 });
    expect(await fetchMetricsHistory("bad")).toBeNull();
  });
});

// ── buildTimeSeries (data transform logic) ──────────────────────────────────

describe("buildTimeSeries", () => {
  it("groups metrics by timestamp with per-node keys", async () => {
    const { buildTimeSeries } = await import("@/app/cluster/page");
    const metrics = [
      { timestamp_ms: 1000, node_id: 0, ticks_ingested: 100, ticks_stored: 90, ticks_dropped: 0, queries_executed: 10, total_rows_scanned: 500, partitions_created: 1, last_ingest_latency_ns: 200 },
      { timestamp_ms: 1000, node_id: 1, ticks_ingested: 80, ticks_stored: 75, ticks_dropped: 0, queries_executed: 8, total_rows_scanned: 400, partitions_created: 1, last_ingest_latency_ns: 180 },
      { timestamp_ms: 2000, node_id: 0, ticks_ingested: 200, ticks_stored: 190, ticks_dropped: 0, queries_executed: 20, total_rows_scanned: 1000, partitions_created: 2, last_ingest_latency_ns: 150 },
    ];

    const ts = buildTimeSeries(metrics, [0, 1]);
    expect(ts).toHaveLength(2);
    // First timestamp has both nodes
    expect(ts[0].ingested_0).toBe(100);
    expect(ts[0].ingested_1).toBe(80);
    expect(ts[0].queries_0).toBe(10);
    // Second timestamp has only node 0
    expect(ts[1].ingested_0).toBe(200);
    expect(ts[1].ingested_1).toBeUndefined();
  });

  it("returns sorted by timestamp", async () => {
    const { buildTimeSeries } = await import("@/app/cluster/page");
    const metrics = [
      { timestamp_ms: 3000, node_id: 0, ticks_ingested: 300, ticks_stored: 0, ticks_dropped: 0, queries_executed: 0, total_rows_scanned: 0, partitions_created: 0, last_ingest_latency_ns: 0 },
      { timestamp_ms: 1000, node_id: 0, ticks_ingested: 100, ticks_stored: 0, ticks_dropped: 0, queries_executed: 0, total_rows_scanned: 0, partitions_created: 0, last_ingest_latency_ns: 0 },
    ];
    const ts = buildTimeSeries(metrics, [0]);
    expect(ts[0].timestamp_ms).toBe(1000);
    expect(ts[1].timestamp_ms).toBe(3000);
  });

  it("handles empty metrics", async () => {
    const { buildTimeSeries } = await import("@/app/cluster/page");
    expect(buildTimeSeries([], [])).toEqual([]);
  });
});

// ── sidebar visibility ──────────────────────────────────────────────────────

describe("sidebar cluster menu visibility", () => {
  it("admin sees Cluster menu", async () => {
    const { getVisibleNav } = await import("@/components/Sidebar");
    expect(getVisibleNav("admin").map((n) => n.label)).toContain("Cluster");
  });

  it("writer does NOT see Cluster menu", async () => {
    const { getVisibleNav } = await import("@/components/Sidebar");
    expect(getVisibleNav("writer").map((n) => n.label)).not.toContain("Cluster");
  });
});

// ── data aggregation ────────────────────────────────────────────────────────

describe("cluster page data aggregation", () => {
  it("computes active count and totals from nodes", () => {
    const nodes = [
      { id: 0, state: "ACTIVE", ticks_ingested: 5000, queries_executed: 100 },
      { id: 1, state: "ACTIVE", ticks_ingested: 4800, queries_executed: 90 },
      { id: 2, state: "DEAD", ticks_ingested: 3000, queries_executed: 50 },
    ];
    expect(nodes.filter((n) => n.state === "ACTIVE").length).toBe(2);
    expect(nodes.reduce((s, n) => s + n.ticks_ingested, 0)).toBe(12800);
    expect(nodes.reduce((s, n) => s + n.queries_executed, 0)).toBe(240);
  });
});
