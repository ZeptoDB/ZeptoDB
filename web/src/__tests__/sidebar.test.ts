import { describe, it, expect } from "vitest";
import { getVisibleNav } from "@/components/Sidebar";

describe("getVisibleNav (role-based menu filtering)", () => {
  it("admin sees all menus including Cluster, Admin, and Settings", () => {
    const labels = getVisibleNav("admin").map((n) => n.label);
    expect(labels).toEqual(["Dashboard", "Query", "Tables", "Cluster", "Tenants", "Admin", "Settings"]);
  });

  it("writer sees Dashboard, Query, Tables", () => {
    const labels = getVisibleNav("writer").map((n) => n.label);
    expect(labels).toEqual(["Dashboard", "Query", "Tables"]);
  });

  it("reader sees Dashboard, Query, Tables", () => {
    const labels = getVisibleNav("reader").map((n) => n.label);
    expect(labels).toEqual(["Dashboard", "Query", "Tables"]);
  });

  it("analyst sees Dashboard, Query, Tables", () => {
    const labels = getVisibleNav("analyst").map((n) => n.label);
    expect(labels).toEqual(["Dashboard", "Query", "Tables"]);
  });

  it("metrics sees Dashboard", () => {
    const labels = getVisibleNav("metrics").map((n) => n.label);
    expect(labels).toEqual(["Dashboard"]);
  });

  it("unknown role sees nothing", () => {
    expect(getVisibleNav("unknown")).toEqual([]);
  });
});
