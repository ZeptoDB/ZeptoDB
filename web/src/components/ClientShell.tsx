"use client";
import { usePathname } from "next/navigation";
import { Box } from "@mui/material";
import Providers from "./Providers";
import { AuthProvider } from "@/lib/auth";
import AuthGuard from "./AuthGuard";
import Sidebar, { SIDEBAR_WIDTH } from "./Sidebar";
import TopBar from "./TopBar";
import type { ReactNode } from "react";

export default function ClientShell({ children }: { children: ReactNode }) {
  const pathname = usePathname();
  const isLogin = pathname === "/login";

  return (
    <Providers>
      <AuthProvider>
        {isLogin ? children : (
          <AuthGuard>
            <Box sx={{ display: "flex" }}>
              <Sidebar />
              <TopBar />
              <Box component="main" sx={{ flexGrow: 1, ml: `${SIDEBAR_WIDTH}px`, mt: "48px", p: 3, minHeight: "calc(100vh - 48px)" }}>
                {children}
              </Box>
            </Box>
          </AuthGuard>
        )}
      </AuthProvider>
    </Providers>
  );
}
