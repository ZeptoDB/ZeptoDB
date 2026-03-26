import { describe, it, expect } from "vitest";
import { render, screen } from "@testing-library/react";
import Pricing from "@/app/(marketing)/pricing/page";

describe("Pricing Page", () => {
  it("renders the pricing header and subheader", () => {
    render(<Pricing />);
    expect(screen.getByText("Pricing")).toBeInTheDocument();
    expect(screen.getByText("Choose your grade")).toBeInTheDocument();
  });

  it("renders the Open Source and Enterprise tier cards", () => {
    render(<Pricing />);
    
    // Check Open Source Card
    expect(screen.getByText("Open Source")).toBeInTheDocument();
    expect(screen.getByText("Free")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Get Started" })).toBeInTheDocument();

    // Check Enterprise Card
    expect(screen.getByText("Enterprise")).toBeInTheDocument();
    expect(screen.getByText("Contact Us")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Book Meeting" })).toBeInTheDocument();
  });
});
