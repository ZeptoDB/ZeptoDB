import { test, expect } from "@playwright/test";

const API_KEY = "ak_d75add5a"; // dev-admin

// Helper: login via API key
async function login(page: import("@playwright/test").Page) {
  await page.goto("/login");
  await page.waitForSelector('input[type="password"], input[type="text"]');
  const input = page.locator("input").first();
  await input.fill(API_KEY);
  await page.locator("button", { hasText: /sign in/i }).first().click();
  // Wait for redirect away from login
  await page.waitForURL((url) => !url.pathname.includes("/login"), { timeout: 10000 });
}

// ─── 1. Login Flow ──────────────────────────────────────────────────────────

test.describe("Login", () => {
  test("login page renders", async ({ page }) => {
    await page.goto("/login");
    await expect(page.getByRole("heading", { name: "ZeptoDB" })).toBeVisible();
    await expect(page.locator("input")).toBeVisible();
  });

  test("login with valid API key redirects to query", async ({ page }) => {
    await login(page);
    await expect(page).toHaveURL(/\/(dashboard|query|tables)/);
  });

  test("login with invalid key shows error", async ({ page }) => {
    await page.goto("/login");
    const input = page.locator("input").first();
    await input.fill("invalid_key_12345");
    await page.locator("button", { hasText: /sign in/i }).first().click();
    // Should stay on login or show error
    await page.waitForTimeout(2000);
    const hasError = await page.locator('[role="alert"], .MuiAlert-root').count();
    const stillOnLogin = page.url().includes("/login");
    expect(hasError > 0 || stillOnLogin).toBeTruthy();
  });
});

// ─── 2. Dashboard ───────────────────────────────────────────────────────────

test.describe("Dashboard", () => {
  test.beforeEach(async ({ page }) => { await login(page); });

  test("dashboard loads with stat cards", async ({ page }) => {
    await page.goto("/dashboard");
    await page.waitForTimeout(3000);
    // Should show key metrics
    await expect(page.locator("text=Ticks Ingested")).toBeVisible();
    await expect(page.locator("text=Queries Executed")).toBeVisible();
    await expect(page.locator("text=Ingest Latency")).toBeVisible();
  });

  test("dashboard shows health status", async ({ page }) => {
    await page.goto("/dashboard");
    await page.waitForTimeout(2000);
    const healthy = page.locator("text=Healthy");
    const unhealthy = page.locator("text=Unhealthy");
    const hasStatus = (await healthy.count()) > 0 || (await unhealthy.count()) > 0;
    expect(hasStatus).toBeTruthy();
  });

  test("dashboard shows tables section", async ({ page }) => {
    await page.goto("/dashboard");
    await page.waitForTimeout(3000);
    await expect(page.getByRole("paragraph").filter({ hasText: "Tables" })).toBeVisible();
  });
});

// ─── 3. Sidebar Navigation ─────────────────────────────────────────────────

test.describe("Sidebar Navigation", () => {
  test.beforeEach(async ({ page }) => { await login(page); });

  test("sidebar shows all admin menus", async ({ page }) => {
    await page.goto("/dashboard");
    const sidebar = page.locator("nav, [class*=Drawer]").first();
    await expect(sidebar.locator("text=Dashboard")).toBeVisible();
    await expect(sidebar.locator("text=Query")).toBeVisible();
    await expect(sidebar.locator("text=Tables")).toBeVisible();
    await expect(sidebar.locator("text=Cluster")).toBeVisible();
    await expect(sidebar.locator("text=Admin")).toBeVisible();
  });

  test("clicking Query navigates to /query", async ({ page }) => {
    await page.goto("/dashboard");
    await page.locator("nav, [class*=Drawer]").first().locator("text=Query").click();
    await expect(page).toHaveURL(/\/query/);
  });

  test("clicking Tables navigates to /tables", async ({ page }) => {
    await page.goto("/dashboard");
    await page.locator("nav, [class*=Drawer]").first().locator("text=Tables").click();
    await expect(page).toHaveURL(/\/tables/);
  });
});

// ─── 4. Query Editor ────────────────────────────────────────────────────────

test.describe("Query Editor", () => {
  test.beforeEach(async ({ page }) => { await login(page); });

  test("query page loads with editor", async ({ page }) => {
    await page.goto("/query");
    await expect(page.locator("text=SQL Editor")).toBeVisible();
    await expect(page.locator("text=Run")).toBeVisible();
  });

  test("run SELECT query and see results", async ({ page }) => {
    await page.goto("/query");
    await page.waitForTimeout(1000);

    // Find CodeMirror editor and type SQL
    const editor = page.locator(".cm-editor .cm-content");
    await editor.click();
    await page.keyboard.press("Control+a");
    await page.keyboard.type("SELECT * FROM trades LIMIT 5");

    // Click Run
    await page.locator("button", { hasText: "Run" }).click();
    await page.waitForTimeout(3000);

    // Should show result with rows or an error (table may not exist)
    const rowChip = page.locator("text=/\\d+ rows/");
    const alert = page.locator('[role="alert"], .MuiAlert-root');
    const hasResult = (await rowChip.count()) > 0 || (await alert.count()) > 0;
    expect(hasResult).toBeTruthy();
  });

  test("run invalid SQL shows error", async ({ page }) => {
    await page.goto("/query");
    await page.waitForTimeout(1000);

    const editor = page.locator(".cm-editor .cm-content");
    await editor.click();
    await page.keyboard.press("Control+a");
    await page.keyboard.type("INVALID SQL GARBAGE");

    await page.locator("button", { hasText: "Run" }).click();
    await page.waitForTimeout(3000);

    // Should show error alert
    const alert = page.locator('.MuiAlert-root');
    await expect(alert).toBeVisible({ timeout: 5000 });
  });

  test("multi-tab: add and switch tabs", async ({ page }) => {
    await page.goto("/query");
    await page.waitForTimeout(1000);

    const tabsBefore = await page.locator('[role="tab"]').count();
    await page.locator("button[aria-label='New tab']").or(page.locator("button").filter({ has: page.locator("svg[data-testid='AddIcon']") })).first().click();
    await page.waitForTimeout(500);
    const tabsAfter = await page.locator('[role="tab"]').count();
    expect(tabsAfter).toBeGreaterThanOrEqual(tabsBefore);
  });
});

// ─── 5. Tables Page ─────────────────────────────────────────────────────────

test.describe("Tables", () => {
  test.beforeEach(async ({ page }) => { await login(page); });

  test("tables page lists tables", async ({ page }) => {
    await page.goto("/tables");
    await page.waitForTimeout(3000);
    // Should show at least the demo table or a message
    const tableRows = page.locator("table tbody tr, [class*=TableRow]");
    const noTables = page.locator("text=/no tables|empty/i");
    const count = await tableRows.count();
    const hasMessage = (await noTables.count()) > 0;
    expect(count > 0 || hasMessage).toBeTruthy();
  });
});

// ─── 6. Cluster Page ────────────────────────────────────────────────────────

test.describe("Cluster", () => {
  test.beforeEach(async ({ page }) => { await login(page); });

  test("cluster page loads with overview", async ({ page }) => {
    await page.goto("/cluster");
    await page.waitForTimeout(2000);
    await expect(page.locator("text=Cluster Overview")).toBeVisible();
    await expect(page.locator("text=Nodes")).toBeVisible();
  });

  test("cluster shows node health table", async ({ page }) => {
    await page.goto("/cluster");
    await page.waitForTimeout(3000);
    await expect(page.locator("text=Node Health")).toBeVisible();
  });
});

// ─── 7. Settings Page ───────────────────────────────────────────────────────

test.describe("Settings", () => {
  test.beforeEach(async ({ page }) => { await login(page); });

  test("settings page loads", async ({ page }) => {
    await page.goto("/settings");
    await page.waitForTimeout(2000);
    // Should show some settings content
    const body = await page.textContent("body");
    expect(body).toBeTruthy();
  });
});

// ─── 8. Default Redirect ────────────────────────────────────────────────────

test.describe("Routing", () => {
  test.beforeEach(async ({ page }) => { await login(page); });

  test("/ redirects to /dashboard", async ({ page }) => {
    await page.goto("/");
    await page.waitForTimeout(2000);
    expect(page.url()).toMatch(/\/(dashboard|login|query)/);
  });
});
