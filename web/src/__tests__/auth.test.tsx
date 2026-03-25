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

import { render, screen, act } from "@testing-library/react";
import { AuthProvider, useAuth } from "@/lib/auth";

function TestConsumer() {
  const { auth, login, logout } = useAuth();
  return (
    <div>
      <span data-testid="role">{auth?.role ?? "none"}</span>
      <span data-testid="key">{auth?.apiKey ?? "none"}</span>
      <span data-testid="subject">{auth?.subject ?? "none"}</span>
      <button onClick={() => login("zepto_test_key").catch(() => {})}>login</button>
      <button onClick={() => login("  zepto_padded  ").catch(() => {})}>login-padded</button>
      <button onClick={logout}>logout</button>
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

beforeEach(() => {
  mockFetch.mockReset();
  Object.keys(store).forEach((k) => delete store[k]);
});

function mockWhoami(role: string, subject: string) {
  mockFetch.mockResolvedValueOnce({
    ok: true,
    json: () => Promise.resolve({ role, subject }),
  });
}

function mockWhoamiFail(error: string) {
  mockFetch.mockResolvedValueOnce({
    ok: false,
    json: () => Promise.resolve({ error }),
  });
}

describe("AuthProvider", () => {
  it("starts with no auth", () => {
    render(<AuthProvider><TestConsumer /></AuthProvider>);
    expect(screen.getByTestId("role").textContent).toBe("none");
  });

  it("login calls /api/whoami with cache:no-store", async () => {
    mockWhoami("admin", "ak_1234");
    render(<AuthProvider><TestConsumer /></AuthProvider>);
    await act(async () => { screen.getByText("login").click(); });

    expect(mockFetch).toHaveBeenCalledWith("/api/whoami", {
      headers: { Authorization: "Bearer zepto_test_key" },
      cache: "no-store",
    });
    expect(screen.getByTestId("role").textContent).toBe("admin");
    expect(screen.getByTestId("subject").textContent).toBe("ak_1234");
  });

  it("login trims whitespace from API key", async () => {
    mockWhoami("reader", "ak_5678");
    render(<AuthProvider><TestConsumer /></AuthProvider>);
    await act(async () => { screen.getByText("login-padded").click(); });

    expect(mockFetch).toHaveBeenCalledWith("/api/whoami", {
      headers: { Authorization: "Bearer zepto_padded" },
      cache: "no-store",
    });
    expect(screen.getByTestId("key").textContent).toBe("zepto_padded");
  });

  it("login throws on invalid key (401)", async () => {
    mockWhoamiFail("Invalid API key");
    render(<AuthProvider><TestConsumer /></AuthProvider>);
    await act(async () => { screen.getByText("login").click(); });
    expect(screen.getByTestId("role").textContent).toBe("none");
  });

  it("login propagates backend error message", async () => {
    mockWhoamiFail("Rate limit exceeded");
    render(<AuthProvider><ErrorConsumer /></AuthProvider>);
    await act(async () => { screen.getByText("login-bad").click(); });
    expect(screen.getByTestId("err").textContent).toBe("Rate limit exceeded");
  });

  it("login falls back to generic message when body is not JSON", async () => {
    mockFetch.mockResolvedValueOnce({
      ok: false,
      json: () => Promise.reject(new Error("not json")),
    });
    render(<AuthProvider><ErrorConsumer /></AuthProvider>);
    await act(async () => { screen.getByText("login-bad").click(); });
    expect(screen.getByTestId("err").textContent).toBe("Invalid API key");
  });

  it("logout clears auth state", async () => {
    mockWhoami("admin", "ak_1234");
    render(<AuthProvider><TestConsumer /></AuthProvider>);
    await act(async () => { screen.getByText("login").click(); });
    expect(screen.getByTestId("role").textContent).toBe("admin");

    await act(async () => { screen.getByText("logout").click(); });
    expect(screen.getByTestId("role").textContent).toBe("none");
  });

  it("re-login works after logout", async () => {
    mockWhoami("admin", "ak_1234");
    render(<AuthProvider><TestConsumer /></AuthProvider>);

    // Login
    await act(async () => { screen.getByText("login").click(); });
    expect(screen.getByTestId("role").textContent).toBe("admin");

    // Logout
    await act(async () => { screen.getByText("logout").click(); });
    expect(screen.getByTestId("role").textContent).toBe("none");

    // Re-login
    mockWhoami("writer", "ak_5678");
    await act(async () => { screen.getByText("login").click(); });
    expect(screen.getByTestId("role").textContent).toBe("writer");
    expect(screen.getByTestId("subject").textContent).toBe("ak_5678");
  });

  it("sessionStorage is cleared on logout and set on login", async () => {
    mockWhoami("reader", "ak_r");
    render(<AuthProvider><TestConsumer /></AuthProvider>);

    await act(async () => { screen.getByText("login").click(); });
    expect(mockSessionStorage.setItem).toHaveBeenCalledWith(
      "zepto_auth",
      expect.stringContaining("reader"),
    );

    await act(async () => { screen.getByText("logout").click(); });
    expect(mockSessionStorage.removeItem).toHaveBeenCalledWith("zepto_auth");
  });
});
