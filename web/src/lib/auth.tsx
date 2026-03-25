"use client";
import { createContext, useContext, useState, useCallback, useEffect, type ReactNode } from "react";
import { getQueryClient } from "@/components/Providers";

interface AuthState {
  apiKey: string;
  role: string;
  subject: string;
}

interface AuthContextType {
  auth: AuthState | null;
  login: (apiKey: string) => Promise<void>;
  logout: () => void;
}

const AuthContext = createContext<AuthContextType>({ auth: null, login: async () => {}, logout: () => {} });

const STORAGE_KEY = "zepto_auth";

export function AuthProvider({ children }: { children: ReactNode }) {
  const [auth, setAuth] = useState<AuthState | null>(null);
  const [loaded, setLoaded] = useState(false);

  useEffect(() => {
    const saved = sessionStorage.getItem(STORAGE_KEY);
    if (saved) setAuth(JSON.parse(saved));
    setLoaded(true);
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
    const state = { apiKey: trimmed, role: detectedRole ?? "reader", subject: detectedSubject ?? "user" };
    setAuth(state);
    sessionStorage.setItem(STORAGE_KEY, JSON.stringify(state));
  }, []);

  const logout = useCallback(() => {
    setAuth(null);
    sessionStorage.removeItem(STORAGE_KEY);
    const qc = getQueryClient();
    qc?.cancelQueries();
    qc?.clear();
  }, []);

  if (!loaded) return null;
  return <AuthContext.Provider value={{ auth, login, logout }}>{children}</AuthContext.Provider>;
}

export function useAuth() { return useContext(AuthContext); }
