"use client";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { ThemeProvider, CssBaseline } from "@mui/material";
import { useRef, type ReactNode } from "react";
import theme from "@/theme/theme";

let globalQueryClient: QueryClient | null = null;

export function getQueryClient() {
  return globalQueryClient;
}

export default function Providers({ children }: { children: ReactNode }) {
  const qcRef = useRef<QueryClient | null>(null);
  if (!qcRef.current) {
    qcRef.current = new QueryClient();
    globalQueryClient = qcRef.current;
  }
  return (
    <QueryClientProvider client={qcRef.current}>
      <ThemeProvider theme={theme}>
        <CssBaseline />
        {children}
      </ThemeProvider>
    </QueryClientProvider>
  );
}
