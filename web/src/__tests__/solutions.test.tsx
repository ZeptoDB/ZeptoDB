import { describe, it, expect } from "vitest";
import { render, screen } from "@testing-library/react";
import Solutions from "@/app/(marketing)/solutions/page";

describe("Solutions Page", () => {
  it("renders an H1 and a brief intro", () => {
    render(<Solutions />);
    expect(screen.getByRole("heading", { level: 1, name: /Solutions/i })).toBeInTheDocument();
    expect(screen.getByText(/HFT desks, semiconductor fabs/i)).toBeInTheDocument();
  });

  it("renders all five industry sections with the correct anchor ids", () => {
    const { container } = render(<Solutions />);
    const expectedIds = ["physical-ai", "finance", "game", "iot", "observability"];
    for (const id of expectedIds) {
      const node = container.querySelector(`#${id}`);
      expect(node, `missing <#${id}> section`).not.toBeNull();
    }
  });

  it("renders each industry heading", () => {
    render(<Solutions />);
    expect(screen.getByRole("heading", { level: 2, name: "Physical AI" })).toBeInTheDocument();
    expect(screen.getByRole("heading", { level: 2, name: "Finance / HFT" })).toBeInTheDocument();
    expect(screen.getByRole("heading", { level: 2, name: "Game" })).toBeInTheDocument();
    expect(screen.getByRole("heading", { level: 2, name: "IoT / Smart Factory" })).toBeInTheDocument();
    expect(screen.getByRole("heading", { level: 2, name: "Real-Time Observability" })).toBeInTheDocument();
  });

  it("renders each killer line as a visible chip", () => {
    render(<Solutions />);
    expect(screen.getByText(/HFT-grade ingestion for your industrial sensors/i)).toBeInTheDocument();
    expect(screen.getByText(/The engine proven on Wall Street/i)).toBeInTheDocument();
    expect(screen.getByText(/Every shot, every frame, every player — queryable in microseconds/i)).toBeInTheDocument();
    expect(
      screen.getByText(/Factory sensor data powered by an ingestion engine proven in HFT/i),
    ).toBeInTheDocument();
    expect(
      screen.getByText(/Logs, metrics, traces — one unified time-series engine/i),
    ).toBeInTheDocument();
  });

  it("mentions the + more verticals (Crypto, Autonomous Vehicles / Robotics, Logistics)", () => {
    render(<Solutions />);
    expect(screen.getByRole("heading", { level: 2, name: /\+ more verticals/i })).toBeInTheDocument();
    const footer = screen.getByText(/Crypto \/ DeFi/i);
    expect(footer).toBeInTheDocument();
    expect(footer.textContent).toMatch(/Autonomous Vehicles \/ Robotics/i);
    expect(footer.textContent).toMatch(/Logistics/i);
  });
});
