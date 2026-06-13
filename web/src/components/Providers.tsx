"use client";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { ThemeProvider, CssBaseline } from "@mui/material";
import { createContext, useState, useMemo, type ReactNode } from "react";
import { getTheme } from "@/theme/theme";

const globalQueryClient = new QueryClient();

export function getQueryClient() {
  return globalQueryClient;
}

export const ThemeModeContext = createContext({ toggleColorMode: () => {} });

export default function Providers({ children }: { children: ReactNode }) {
  const [mode, setMode] = useState<"light" | "dark">("dark");
  const colorMode = useMemo(() => ({
    toggleColorMode: () => {
      setMode((prevMode) => (prevMode === "light" ? "dark" : "light"));
    },
  }), []);

  const theme = useMemo(() => getTheme(mode), [mode]);

  return (
    <QueryClientProvider client={globalQueryClient}>
      <ThemeModeContext.Provider value={colorMode}>
        <ThemeProvider theme={theme}>
          <CssBaseline />
          {children}
        </ThemeProvider>
      </ThemeModeContext.Provider>
    </QueryClientProvider>
  );
}
