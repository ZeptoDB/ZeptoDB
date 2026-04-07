import { describe, it, expect } from "vitest";
import { getTheme } from "@/theme/theme";

describe("Quantum Precision Theme", () => {
  it("provides dark mode palette with custom colors", () => {
    const theme = getTheme("dark");
    expect(theme.palette.mode).toBe("dark");
    expect(theme.palette.primary.main).toBe("#4D7CFF");
    expect(theme.palette.secondary.main).toBe("#00F5D4");
    expect(theme.palette.background.default).toBe("#0A0C10");
  });

  it("provides light mode palette with custom colors for toggle support", () => {
    const theme = getTheme("light");
    expect(theme.palette.mode).toBe("light");
    expect(theme.palette.primary.main).toBe("#2563EB");
    expect(theme.palette.background.default).toBe("#F8FAFC");
  });

  it("configures backdrop filter (Glassmorphism) on Paper and Card components", () => {
    const theme = getTheme("dark");
    const paperOverrides = theme.components?.MuiPaper?.styleOverrides?.root;
    // @ts-ignore
    expect(paperOverrides?.backdropFilter).toBe("blur(16px)");
    
    const cardOverrides = theme.components?.MuiCard?.styleOverrides?.root;
    // @ts-ignore
    expect(cardOverrides?.backdropFilter).toBe("blur(16px)");
  });
});
