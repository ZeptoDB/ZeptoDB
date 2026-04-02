import { describe, it, expect, vi, beforeEach } from "vitest";

// Mock fetch globally before importing auth module
const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

// Mock sessionStorage
const store: Record<string, string> = {};
const mockSessionStorage = {
  getItem: vi.fn((key: string) => store[key] ?? null),
  setItem: vi.fn((key: string, val: string) => { store[key] = val; }),
  removeItem: vi.fn((key: string) => { delete store[key]; }),
};
Object.defineProperty(globalThis, "sessionStorage", { value: mockSessionStorage });

import { render, screen, act, waitFor } from "@testing-library/react";
import { AuthProvider, useAuth } from "@/lib/auth";

function TestConsumer() {
  const { auth, login, logout } = useAuth();
  return (
    <div>
      <span data-testid="role">{auth?.role ?? "none"}</span>
      <span data-testid="subject">{auth?.subject ?? "none"}</span>
      <span data-testid="source">{auth?.source ?? "none"}</span>
      <button onClick={() => login("zepto_test_key").catch(() => {})}>login</button>
      <button onClick={() => login("  zepto_padded  ").catch(() => {})}>login-padded</button>
      <button onClick={() => { logout(); }}>logout</button>
    </div>
  );
}

function ErrorConsumer() {
  const { login } = useAuth();
  return (
    <div>
      <button onClick={async () => {
        try { await login("bad_key"); } catch (e) {
          document.getElementById("err")!.textContent = (e as Error).message;
        }
      }}>login-bad</button>
      <span id="err" data-testid="err"></span>
    </div>
  );
}

// Default mock: no session, no auth
function setupDefaultMock() {
  mockFetch.mockImplementation((url: string) => {
    if (url === "/api/auth/me") return Promise.resolve({ ok: false, status: 401 });
    if (url === "/api/auth/logout") return Promise.resolve({ ok: true, json: () => Promise.resolve({ ok: true }) });
    if (url === "/api/auth/session") return Promise.resolve({ ok: false, status: 503 });
    return Promise.resolve({ ok: false, status: 404 });
  });
}

beforeEach(() => {
  mockFetch.mockReset();
  Object.keys(store).forEach((k) => delete store[k]);
  setupDefaultMock();
});

async function renderAndWait(ui: React.ReactElement) {
  let result: ReturnType<typeof render>;
  await act(async () => { result = render(ui); });
  return result!;
}

function mockWhoami(role: string, subject: string) {
  mockFetch.mockImplementation((url: string) => {
    if (url === "/api/auth/me") return Promise.resolve({ ok: false, status: 401 });
    if (url === "/api/whoami") return Promise.resolve({ ok: true, json: () => Promise.resolve({ role, subject }) });
    if (url === "/api/auth/session") return Promise.resolve({ ok: false, status: 503 });
    if (url === "/api/auth/logout") return Promise.resolve({ ok: true, json: () => Promise.resolve({ ok: true }) });
    return Promise.resolve({ ok: false, status: 404 });
  });
}

function mockWhoamiFail(error: string) {
  mockFetch.mockImplementation((url: string) => {
    if (url === "/api/auth/me") return Promise.resolve({ ok: false, status: 401 });
    if (url === "/api/whoami") return Promise.resolve({ ok: false, json: () => Promise.resolve({ error }) });
    if (url === "/api/auth/logout") return Promise.resolve({ ok: true, json: () => Promise.resolve({ ok: true }) });
    return Promise.resolve({ ok: false, status: 404 });
  });
}

describe("AuthProvider", () => {
  it("starts with no auth", async () => {
    await renderAndWait(<AuthProvider><TestConsumer /></AuthProvider>);
    expect(screen.getByTestId("role").textContent).toBe("none");
  });

  it("login calls /api/whoami and falls back to client storage", async () => {
    mockWhoami("admin", "ak_1234");
    await renderAndWait(<AuthProvider><TestConsumer /></AuthProvider>);
    await act(async () => { screen.getByText("login").click(); });

    expect(screen.getByTestId("role").textContent).toBe("admin");
    expect(screen.getByTestId("subject").textContent).toBe("ak_1234");
    expect(screen.getByTestId("source").textContent).toBe("api_key");
  });

  it("login trims whitespace from API key", async () => {
    mockWhoami("reader", "ak_5678");
    await renderAndWait(<AuthProvider><TestConsumer /></AuthProvider>);
    await act(async () => { screen.getByText("login-padded").click(); });
    expect(screen.getByTestId("role").textContent).toBe("reader");
  });

  it("login throws on invalid key (401)", async () => {
    mockWhoamiFail("Invalid API key");
    await renderAndWait(<AuthProvider><TestConsumer /></AuthProvider>);
    await act(async () => { screen.getByText("login").click(); });
    expect(screen.getByTestId("role").textContent).toBe("none");
  });

  it("login propagates backend error message", async () => {
    mockWhoamiFail("Rate limit exceeded");
    await renderAndWait(<AuthProvider><ErrorConsumer /></AuthProvider>);
    await act(async () => { screen.getByText("login-bad").click(); });
    expect(screen.getByTestId("err").textContent).toBe("Rate limit exceeded");
  });

  it("login falls back to generic message when body is not JSON", async () => {
    mockFetch.mockImplementation((url: string) => {
      if (url === "/api/auth/me") return Promise.resolve({ ok: false, status: 401 });
      if (url === "/api/whoami") return Promise.resolve({ ok: false, json: () => Promise.reject(new Error("not json")) });
      return Promise.resolve({ ok: false, status: 404 });
    });
    await renderAndWait(<AuthProvider><ErrorConsumer /></AuthProvider>);
    await act(async () => { screen.getByText("login-bad").click(); });
    expect(screen.getByTestId("err").textContent).toBe("Invalid API key");
  });

  it("logout clears auth state", async () => {
    mockWhoami("admin", "ak_1234");
    await renderAndWait(<AuthProvider><TestConsumer /></AuthProvider>);
    await act(async () => { screen.getByText("login").click(); });
    expect(screen.getByTestId("role").textContent).toBe("admin");

    await act(async () => { screen.getByText("logout").click(); });
    expect(screen.getByTestId("role").textContent).toBe("none");
  });

  it("re-login works after logout", async () => {
    mockWhoami("admin", "ak_1234");
    await renderAndWait(<AuthProvider><TestConsumer /></AuthProvider>);

    await act(async () => { screen.getByText("login").click(); });
    expect(screen.getByTestId("role").textContent).toBe("admin");

    await act(async () => { screen.getByText("logout").click(); });
    expect(screen.getByTestId("role").textContent).toBe("none");

    mockWhoami("writer", "ak_5678");
    await act(async () => { screen.getByText("login").click(); });
    expect(screen.getByTestId("role").textContent).toBe("writer");
    expect(screen.getByTestId("subject").textContent).toBe("ak_5678");
  });

  it("sessionStorage is cleared on logout", async () => {
    mockWhoami("reader", "ak_r");
    await renderAndWait(<AuthProvider><TestConsumer /></AuthProvider>);

    await act(async () => { screen.getByText("login").click(); });
    expect(mockSessionStorage.setItem).toHaveBeenCalledWith(
      "zepto_auth",
      expect.stringContaining("reader"),
    );

    await act(async () => { screen.getByText("logout").click(); });
    expect(mockSessionStorage.removeItem).toHaveBeenCalledWith("zepto_auth");
  });
});
