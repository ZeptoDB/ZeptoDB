import { describe, it, expect } from "vitest";
import { render, screen } from "@testing-library/react";
import Features from "@/app/(marketing)/features/page";

describe("Features Page", () => {
  it("renders the Platform Features H1 and the subtitle tagline", () => {
    render(<Features />);
    expect(
      screen.getByRole("heading", { level: 1, name: /Platform Features/i }),
    ).toBeInTheDocument();
    expect(
      screen.getByText(/Four capability groups, one engine/i),
    ).toBeInTheDocument();
  });

  it("renders all four capability-group H2 headings", () => {
    render(<Features />);
    expect(screen.getByRole("heading", { level: 2, name: "Ingest" })).toBeInTheDocument();
    expect(screen.getByRole("heading", { level: 2, name: "Query" })).toBeInTheDocument();
    expect(screen.getByRole("heading", { level: 2, name: "Deploy" })).toBeInTheDocument();
    expect(screen.getByRole("heading", { level: 2, name: "Secure" })).toBeInTheDocument();
  });

  it("cites the corrected ITCH 250 ns parser latency (not 350 ns) under Ingest", () => {
    render(<Features />);
    expect(screen.getByText(/NASDAQ ITCH \(250 ns parser\)/i)).toBeInTheDocument();
    expect(screen.getByText(/FIX \(350 ns parser\)/i)).toBeInTheDocument();
  });

  it("renders a representative bullet from each capability group", () => {
    render(<Features />);
    // Ingest
    expect(screen.getByText(/5\.52M ticks\/sec sustained/i)).toBeInTheDocument();
    // Query
    expect(
      screen.getByText(/ClickHouse-compatible HTTP API on port 8123/i),
    ).toBeInTheDocument();
    // Deploy
    expect(
      screen.getByText(/Official Helm chart with rolling upgrades/i),
    ).toBeInTheDocument();
    // Secure
    expect(
      screen.getByText(/RBAC with 5 roles/i),
    ).toBeInTheDocument();
  });
});
