import { describe, it, expect, vi, beforeEach } from "vitest";
import { querySQL } from "@/lib/api";

const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

beforeEach(() => mockFetch.mockReset());

describe("TablesPage SQL usage", () => {
  it("SHOW TABLES sends correct SQL", async () => {
    const result = { columns: ["name", "rows"], data: [["trades", 10000]], rows: 1, execution_time_us: 5 };
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve(result) });

    const r = await querySQL("SHOW TABLES", "zepto_key");
    expect(mockFetch).toHaveBeenCalledWith("/api", {
      method: "POST",
      body: "SHOW TABLES",
      headers: { Authorization: "Bearer zepto_key" },
      credentials: "include",
      signal: undefined,
    });
    expect(r.data[0][0]).toBe("trades");
  });

  it("DESCRIBE sends correct SQL", async () => {
    const result = { columns: ["column", "type"], data: [["price", "int64"], ["volume", "int64"]], rows: 2, execution_time_us: 3 };
    mockFetch.mockResolvedValue({ ok: true, json: () => Promise.resolve(result) });

    const r = await querySQL("DESCRIBE trades", "zepto_key");
    expect(mockFetch).toHaveBeenCalledWith("/api", {
      method: "POST",
      body: "DESCRIBE trades",
      headers: { Authorization: "Bearer zepto_key" },
      credentials: "include",
      signal: undefined,
    });
    expect(r.data).toHaveLength(2);
    expect(r.data[0][0]).toBe("price");
  });
});
