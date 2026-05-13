import { describe, it, expect } from "vitest";
import { render, screen } from "@testing-library/react";
import Home from "@/app/(marketing)/home/page";

describe("Home Page", () => {
  it("renders the new multi-industry H1", () => {
    render(<Home />);
    const h1 = screen.getByRole("heading", { level: 1 });
    expect(h1).toHaveTextContent(
      /The Time-Series Database for Physical AI, IoT, and Real-Time Analytics/i,
    );
  });

  it("renders the proof-metrics strip with all four stats", () => {
    render(<Home />);
    expect(screen.getByText("5.52M")).toBeInTheDocument();
    expect(screen.getByText("272µs")).toBeInTheDocument();
    expect(screen.getByText("6 native feeds")).toBeInTheDocument();
    expect(screen.getByText("kdb+-class")).toBeInTheDocument();
  });

  it("renders the four featured industry cards plus a '+ more verticals' entry", () => {
    render(<Home />);
    expect(screen.getByText("Physical AI")).toBeInTheDocument();
    expect(screen.getByText("Finance / HFT")).toBeInTheDocument();
    expect(screen.getByText("Game")).toBeInTheDocument();
    expect(screen.getByText("IoT / Smart Factory")).toBeInTheDocument();
    expect(screen.getByText("+ more verticals")).toBeInTheDocument();
  });

  it("links each featured industry card to the right /solutions anchor", () => {
    render(<Home />);
    const links = screen.getAllByRole("link", { name: /Learn more/i });
    const hrefs = links.map((l) => l.getAttribute("href"));
    expect(hrefs).toEqual(
      expect.arrayContaining([
        "/solutions#physical-ai",
        "/solutions#finance",
        "/solutions#game",
        "/solutions#iot",
        "/solutions",
      ]),
    );
  });

  it("renders the Why-ZeptoDB 3-column block", () => {
    render(<Home />);
    expect(screen.getByText(/µs latency — not ms, not seconds/i)).toBeInTheDocument();
    expect(
      screen.getByText(/Research → Production → Compliance/i),
    ).toBeInTheDocument();
    expect(
      screen.getByText(/Open source, standard SQL, Python zero-copy/i),
    ).toBeInTheDocument();
  });

  it("renders primary CTAs pointing at docs, solutions, Discord, and GitHub", () => {
    render(<Home />);
    const solutionsLink = screen.getAllByRole("link", { name: /View Solutions/i });
    expect(solutionsLink.length).toBeGreaterThan(0);
    expect(screen.getAllByRole("link", { name: /Get Started/i }).length).toBeGreaterThan(0);
    expect(screen.getByRole("link", { name: /Join Discord/i })).toBeInTheDocument();
    expect(screen.getByRole("link", { name: /GitHub — Star/i })).toBeInTheDocument();
  });
});
