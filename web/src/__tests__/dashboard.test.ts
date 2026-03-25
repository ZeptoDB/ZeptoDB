import { describe, it, expect, vi, beforeEach } from "vitest";
import { fetchStats } from "@/lib/api";

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
