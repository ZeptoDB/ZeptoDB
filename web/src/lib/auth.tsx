"use client";
import { createContext, useContext, useState, useCallback, useEffect, type ReactNode } from "react";
import { getQueryClient } from "@/components/Providers";

interface AuthState {
  apiKey: string;
  role: string;
  subject: string;
  source: string; // "api_key" | "jwt" | "session"
}

interface AuthContextType {
  auth: AuthState | null;
  login: (apiKey: string) => Promise<void>;
  loginSSO: () => void;
  logout: () => Promise<void>;
  refresh: () => Promise<boolean>;
}

const AuthContext = createContext<AuthContextType>({
  auth: null,
  login: async () => {},
  loginSSO: () => {},
  logout: async () => {},
  refresh: async () => false,
});

const STORAGE_KEY = "zepto_auth";

export function AuthProvider({ children }: { children: ReactNode }) {
  const [auth, setAuth] = useState<AuthState | null>(null);
  const [loaded, setLoaded] = useState(false);

  // On mount: check session cookie first, then fall back to sessionStorage
  useEffect(() => {
    (async () => {
      // Try session cookie (server-side session)
      try {
        const res = await fetch("/api/auth/me", { credentials: "include" });
        if (res.ok) {
          const { role, subject, source } = await res.json();
          setAuth({ apiKey: "", role, subject, source: source ?? "session" });
          setLoaded(true);
          return;
        }
      } catch { /* no session — fall through */ }

      // Fall back to saved Bearer token
      const saved = sessionStorage.getItem(STORAGE_KEY);
      if (saved) setAuth(JSON.parse(saved));
      setLoaded(true);
    })();
  }, []);

  const login = useCallback(async (apiKey: string) => {
    const trimmed = apiKey.trim();
    const res = await fetch("/api/whoami", {
      headers: { Authorization: `Bearer ${trimmed}` },
      cache: "no-store",
    });
    if (!res.ok) {
      const body = await res.json().catch(() => null);
      throw new Error(body?.error ?? "Invalid API key");
    }

    const { role: detectedRole, subject: detectedSubject } = await res.json();

    // Try to create a server-side session
    try {
      const sessionRes = await fetch("/api/auth/session", {
        method: "POST",
        headers: { Authorization: `Bearer ${trimmed}` },
        credentials: "include",
      });
      if (sessionRes.ok) {
        const state = { apiKey: "", role: detectedRole ?? "reader", subject: detectedSubject ?? "user", source: "session" };
        setAuth(state);
        // Don't store token in sessionStorage when using server sessions
        sessionStorage.removeItem(STORAGE_KEY);
        return;
      }
    } catch { /* session creation failed — fall back to client-side storage */ }

    const state = { apiKey: trimmed, role: detectedRole ?? "reader", subject: detectedSubject ?? "user", source: "api_key" };
    setAuth(state);
    sessionStorage.setItem(STORAGE_KEY, JSON.stringify(state));
  }, []);

  const loginSSO = useCallback(() => {
    // Redirect to server's OIDC login endpoint
    window.location.href = "/api/auth/login";
  }, []);

  const logout = useCallback(async () => {
    // Try server-side logout
    try {
      await fetch("/api/auth/logout", { method: "POST", credentials: "include" });
    } catch { /* ignore */ }

    setAuth(null);
    sessionStorage.removeItem(STORAGE_KEY);
    const qc = getQueryClient();
    qc?.cancelQueries();
    qc?.clear();
  }, []);

  const refresh = useCallback(async (): Promise<boolean> => {
    try {
      const res = await fetch("/api/auth/refresh", {
        method: "POST",
        credentials: "include",
      });
      return res.ok;
    } catch {
      return false;
    }
  }, []);

  if (!loaded) return null;
  return <AuthContext.Provider value={{ auth, login, loginSSO, logout, refresh }}>{children}</AuthContext.Provider>;
}

export function useAuth() { return useContext(AuthContext); }
