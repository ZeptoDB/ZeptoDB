import { describe, it, expect } from "vitest";
import { getVisibleNav } from "@/components/Sidebar";

describe("getVisibleNav (role-based menu filtering)", () => {
  it("admin sees all menus including Cluster and Admin", () => {
    const labels = getVisibleNav("admin").map((n) => n.label);
    expect(labels).toEqual(["Query", "Dashboard", "Tables", "Cluster", "Admin"]);
  });

  it("writer sees Query, Dashboard, Tables", () => {
    const labels = getVisibleNav("writer").map((n) => n.label);
    expect(labels).toEqual(["Query", "Dashboard", "Tables"]);
  });

  it("reader sees Query, Dashboard, Tables", () => {
    const labels = getVisibleNav("reader").map((n) => n.label);
    expect(labels).toEqual(["Query", "Dashboard", "Tables"]);
  });

  it("analyst sees Query, Tables (no Dashboard, no Cluster)", () => {
    const labels = getVisibleNav("analyst").map((n) => n.label);
    expect(labels).toEqual(["Query", "Tables"]);
  });

  it("metrics sees Dashboard and Cluster", () => {
    const labels = getVisibleNav("metrics").map((n) => n.label);
    expect(labels).toEqual(["Dashboard", "Cluster"]);
  });

  it("unknown role sees nothing", () => {
    expect(getVisibleNav("unknown")).toEqual([]);
  });
});
