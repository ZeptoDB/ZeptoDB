import { defineConfig } from "vitest/config";
import path from "path";

export default defineConfig({
  test: {
    environment: "jsdom",
    globals: true,
    setupFiles: ["./src/__tests__/setup.ts"],
    // Playwright specs live under ./e2e and must be run via `pnpm exec playwright test`,
    // not by Vitest — exclude them so the Vitest run is clean.
    exclude: ["node_modules/**", "dist/**", "e2e/**", ".next/**", "out/**"],
  },
  resolve: {
    alias: { "@": path.resolve(__dirname, "src") },
  },
});
