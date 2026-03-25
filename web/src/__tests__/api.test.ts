import { describe, it, expect, vi, beforeEach } from "vitest";
import { querySQL, fetchStats, fetchHealth, fetchCluster } from "@/lib/api";

// Mock global fetch
const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

beforeEach(() => mockFetch.mockReset());

describe("headers", () => {
  it("sends Authorization header when apiKey is provided", async () => {
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve({ status: "healthy" }) });
    await fetchHealth("zepto_abc123");
    expect(mockFetch).toHaveBeenCalledWith("/api/health", {
      headers: { Authorization: "Bearer zepto_abc123" },
    });
  });

  it("sends no Authorization header when apiKey is empty string", async () => {
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve({ status: "healthy" }) });
    await fetchHealth("");
    expect(mockFetch).toHaveBeenCalledWith("/api/health", { headers: {} });
  });

  it("sends no Authorization header when apiKey is undefined", async () => {
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve({ status: "healthy" }) });
    await fetchHealth();
    expect(mockFetch).toHaveBeenCalledWith("/api/health", { headers: {} });
  });
});

describe("querySQL", () => {
  it("sends POST with SQL body and auth header", async () => {
    const result = { columns: ["price"], data: [[15000]], rows: 1, execution_time_us: 30 };
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve(result) });

    const r = await querySQL("SELECT price FROM trades WHERE symbol = 1 LIMIT 1", "zepto_key");
    expect(mockFetch).toHaveBeenCalledWith("/api", {
      method: "POST",
      body: "SELECT price FROM trades WHERE symbol = 1 LIMIT 1",
      headers: { Authorization: "Bearer zepto_key" },
    });
    expect(r).toEqual(result);
  });

  it("throws on server error with error message", async () => {
    mockFetch.mockResolvedValue({
      ok: false,
      json: () => Promise.resolve({ error: "No Authorization header" }),
    });
    await expect(querySQL("SELECT 1")).rejects.toThrow("No Authorization header");
  });

  it("throws with HTTP status when body is not JSON", async () => {
    mockFetch.mockResolvedValue({
      ok: false,
      json: () => Promise.reject(new Error("not json")),
    });
    await expect(querySQL("SELECT 1")).rejects.toThrow("HTTP");
  });
});

describe("fetchStats", () => {
  it("returns stats on success", async () => {
    const stats = { ticks_ingested: 10000, ticks_stored: 10000, ticks_dropped: 0 };
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve(stats) });
    expect(await fetchStats("key")).toEqual(stats);
  });

  it("throws on auth failure", async () => {
    mockFetch.mockResolvedValue({ ok: false, status: 401 });
    await expect(fetchStats("bad_key")).rejects.toThrow("HTTP 401");
  });
});

describe("fetchHealth", () => {
  it("returns healthy status", async () => {
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve({ status: "healthy" }) });
    expect(await fetchHealth()).toEqual({ status: "healthy" });
  });

  it("returns unhealthy on failure (no throw)", async () => {
    mockFetch.mockResolvedValue({ ok: false });
    expect(await fetchHealth()).toEqual({ status: "unhealthy" });
  });
});

describe("fetchCluster", () => {
  it("returns cluster data on success", async () => {
    const data = { mode: "standalone", ticks_stored: 10000 };
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve(data) });
    expect(await fetchCluster("key")).toEqual(data);
  });

  it("returns null on auth failure (no throw)", async () => {
    mockFetch.mockResolvedValue({ ok: false });
    expect(await fetchCluster("bad")).toBeNull();
  });
});
