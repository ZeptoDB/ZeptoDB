import { describe, it, expect, vi, beforeEach } from "vitest";

// Mock localStorage
const store: Record<string, string> = {};
Object.defineProperty(globalThis, "localStorage", {
  value: {
    getItem: vi.fn((key: string) => store[key] ?? null),
    setItem: vi.fn((key: string, val: string) => { store[key] = val; }),
    removeItem: vi.fn((key: string) => { delete store[key]; }),
  },
});

import { loadHistory } from "@/app/query/page";

beforeEach(() => {
  Object.keys(store).forEach((k) => delete store[k]);
});

describe("loadHistory", () => {
  it("returns empty array when no history", () => {
    expect(loadHistory()).toEqual([]);
  });

  it("returns saved history", () => {
    store["zepto_query_history"] = JSON.stringify(["SELECT 1", "SELECT 2"]);
    expect(loadHistory()).toEqual(["SELECT 1", "SELECT 2"]);
  });

  it("returns empty array on corrupted JSON", () => {
    store["zepto_query_history"] = "not-json";
    expect(loadHistory()).toEqual([]);
  });
});
