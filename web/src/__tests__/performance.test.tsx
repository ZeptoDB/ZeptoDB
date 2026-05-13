import { describe, it, expect } from "vitest";
import { render, screen, within } from "@testing-library/react";
import Performance from "@/app/(marketing)/performance/page";

describe("Performance Page", () => {
  it("renders the page heading", () => {
    render(<Performance />);
    expect(screen.getByRole("heading", { level: 1, name: /Performance/i })).toBeInTheDocument();
  });

  it("renders a comparison table with the expected 5 column headers", () => {
    render(<Performance />);
    const expected = [
      "Engine",
      "Ingestion",
      "1M-row filter p50",
      "ASOF JOIN",
      "License cost",
      "Deployment",
    ];
    for (const label of expected) {
      expect(screen.getByRole("columnheader", { name: label })).toBeInTheDocument();
    }
  });

  it("renders one row per engine (ZeptoDB, kdb+, ClickHouse, InfluxDB) — 4 total", () => {
    render(<Performance />);
    // The comparison table has an engine cell per row. We locate them by well-known engine names.
    expect(screen.getByRole("cell", { name: "ZeptoDB" })).toBeInTheDocument();
    expect(screen.getByRole("cell", { name: "kdb+" })).toBeInTheDocument();
    expect(screen.getByRole("cell", { name: "ClickHouse" })).toBeInTheDocument();
    expect(screen.getByRole("cell", { name: "InfluxDB" })).toBeInTheDocument();
  });

  it("shows the ZeptoDB reference numbers for ingestion and filter p50", () => {
    render(<Performance />);
    // ZeptoDB ingestion appears in both the comparison table and the detail card,
    // so match with getAllByText rather than asserting uniqueness.
    const ingestion = screen.getAllByText(/5\.52M events\/sec/i);
    expect(ingestion.length).toBeGreaterThanOrEqual(1);
    const filter = screen.getAllByText(/272 µs/);
    expect(filter.length).toBeGreaterThanOrEqual(1);
  });

  it("renders the Test environment footer mentioning clang-19 / devlogs 097 & 098", () => {
    render(<Performance />);
    const footerHeading = screen.getByRole("heading", { level: 2, name: /Test environment/i });
    expect(footerHeading).toBeInTheDocument();
    const footerCard = footerHeading.closest("div");
    expect(footerCard).not.toBeNull();
    const scope = within(footerCard as HTMLElement);
    expect(scope.getByText(/clang-19/i)).toBeInTheDocument();
    expect(scope.getByText(/devlogs 097 and 098/i)).toBeInTheDocument();
  });
});
