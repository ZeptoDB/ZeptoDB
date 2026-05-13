import { describe, it, expect } from "vitest";
import { render, screen } from "@testing-library/react";
import Pricing from "@/app/(marketing)/pricing/page";

describe("Pricing Page", () => {
  it("renders the pricing header and the open-core subheader", () => {
    render(<Pricing />);
    expect(screen.getByRole("heading", { level: 1, name: /pricing/i })).toBeInTheDocument();
    expect(screen.getByText(/Open core, enterprise-ready/i)).toBeInTheDocument();
  });

  it("renders the Open Source and Enterprise tier cards with neutralized copy", () => {
    render(<Pricing />);

    // Open Source
    expect(screen.getByText("Open Source")).toBeInTheDocument();
    expect(screen.getByText("Free")).toBeInTheDocument();
    expect(screen.getByRole("link", { name: /get started/i })).toBeInTheDocument();

    // Enterprise
    expect(screen.getByText("Enterprise")).toBeInTheDocument();
    expect(screen.getByText("Contact Us")).toBeInTheDocument();
    expect(screen.getByRole("link", { name: /book a demo/i })).toBeInTheDocument();
  });

  it("mentions the non-finance-specific production domains", () => {
    render(<Pricing />);
    const enterpriseBody = screen.getByText(
      /finance, factory floors, game backends, and Physical AI platforms/i,
    );
    expect(enterpriseBody).toBeInTheDocument();
  });

  it("shows the cloud-hosted tier coming-soon note", () => {
    render(<Pricing />);
    expect(screen.getByText(/Cloud-hosted tier coming soon/i)).toBeInTheDocument();
  });
});
