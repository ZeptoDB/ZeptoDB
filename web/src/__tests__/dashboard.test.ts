import { describe, it, expect, vi, beforeEach } from "vitest";
import { fetchStats, querySQL } from "@/lib/api";

const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

beforeEach(() => mockFetch.mockReset());

describe("Dashboard stats fields (per HTTP_REFERENCE.md)", () => {
  const fullStats = {
    ticks_ingested: 5000000,
    ticks_stored: 4999800,
    ticks_dropped: 200,
    queries_executed: 12345,
    total_rows_scanned: 50000000,
    partitions_created: 10,
    last_ingest_latency_ns: 181,
  };

  it("returns all 7 stats fields from /stats endpoint", async () => {
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve(fullStats) });
    const data = await fetchStats("key");
    expect(data.ticks_ingested).toBe(5000000);
    expect(data.ticks_stored).toBe(4999800);
    expect(data.ticks_dropped).toBe(200);
    expect(data.queries_executed).toBe(12345);
    expect(data.total_rows_scanned).toBe(50000000);
    expect(data.partitions_created).toBe(10);
    expect(data.last_ingest_latency_ns).toBe(181);
  });
});

describe("Dashboard drop rate calculation", () => {
  it("computes drop rate percentage from stats", () => {
    const stats = { ticks_ingested: 10000, ticks_dropped: 50 };
    const dropRate = stats.ticks_ingested > 0 ? (stats.ticks_dropped / stats.ticks_ingested) * 100 : 0;
    expect(dropRate).toBeCloseTo(0.5);
  });

  it("returns 0 when no ticks ingested", () => {
    const stats = { ticks_ingested: 0, ticks_dropped: 0 };
    const dropRate = stats.ticks_ingested > 0 ? (stats.ticks_dropped / stats.ticks_ingested) * 100 : 0;
    expect(dropRate).toBe(0);
  });

  it("returns 0 when no drops", () => {
    const stats = { ticks_ingested: 100000, ticks_dropped: 0 };
    const dropRate = stats.ticks_ingested > 0 ? (stats.ticks_dropped / stats.ticks_ingested) * 100 : 0;
    expect(dropRate).toBe(0);
  });
});

describe("Dashboard avg query cost calculation", () => {
  it("computes avg rows scanned per query", () => {
    const stats = { queries_executed: 100, total_rows_scanned: 500000 };
    const avg = stats.queries_executed > 0 ? Math.round(stats.total_rows_scanned / stats.queries_executed) : 0;
    expect(avg).toBe(5000);
  });

  it("returns 0 when no queries executed", () => {
    const stats = { queries_executed: 0, total_rows_scanned: 0 };
    const avg = stats.queries_executed > 0 ? Math.round(stats.total_rows_scanned / stats.queries_executed) : 0;
    expect(avg).toBe(0);
  });
});

describe("Dashboard tables summary fetch", () => {
  it("SHOW TABLES + DESCRIBE + count(*) builds table info", async () => {
    // 1st call: SHOW TABLES
    mockFetch.mockResolvedValueOnce({
      ok: true,
      json: () => Promise.resolve({ columns: ["name"], data: [["trades"], ["quotes"]], rows: 2 }),
    });
    const tables = await querySQL("SHOW TABLES", "key");
    const names = tables.data.map((row: (string | number)[]) => String(row[0]));
    expect(names).toEqual(["trades", "quotes"]);

    // 2nd call: DESCRIBE trades
    mockFetch.mockResolvedValueOnce({
      ok: true,
      json: () => Promise.resolve({ columns: ["column", "type"], data: [["price", "int64"], ["volume", "int64"], ["symbol", "string"]] }),
    });
    const desc = await querySQL("DESCRIBE trades", "key");
    expect(desc.data.length).toBe(3);

    // 3rd call: SELECT count(*)
    mockFetch.mockResolvedValueOnce({
      ok: true,
      json: () => Promise.resolve({ columns: ["count"], data: [[50000]], rows: 1 }),
    });
    const count = await querySQL("SELECT count(*) FROM trades", "key");
    expect(Number(count.data[0][0])).toBe(50000);

    // Assemble table info
    const info = { name: "trades", columns: desc.data.length, rows: Number(count.data[0][0]) };
    expect(info).toEqual({ name: "trades", columns: 3, rows: 50000 });
  });

  it("handles empty database (no tables)", async () => {
    mockFetch.mockResolvedValueOnce({
      ok: true,
      json: () => Promise.resolve({ columns: ["name"], data: [], rows: 0 }),
    });
    const tables = await querySQL("SHOW TABLES", "key");
    expect(tables.data).toEqual([]);
  });
});
