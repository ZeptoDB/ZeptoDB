"use client";
import { usePathname } from "next/navigation";
import { Box, useMediaQuery, useTheme } from "@mui/material";
import Providers from "./Providers";
import { AuthProvider } from "@/lib/auth";
import AuthGuard from "./AuthGuard";
import Sidebar, { SIDEBAR_WIDTH } from "./Sidebar";
import TopBar from "./TopBar";
import { useState, type ReactNode } from "react";

function ConsoleShell({ children }: { children: ReactNode }) {
  const theme = useTheme();
  const isMobile = useMediaQuery(theme.breakpoints.down("md"));
  const [mobileOpen, setMobileOpen] = useState(false);

  return (
    <Box sx={{ display: "flex" }}>
      {isMobile ? (
        <Sidebar variant="temporary" open={mobileOpen} onClose={() => setMobileOpen(false)} />
      ) : (
        <Sidebar />
      )}
      <TopBar onMobileMenuToggle={() => setMobileOpen((o) => !o)} />
      <Box
        component="main"
        sx={{
          flexGrow: 1,
          ml: { xs: 0, md: `${SIDEBAR_WIDTH}px` },
          mt: "60px",
          p: { xs: 2, md: 4 },
          minHeight: "calc(100vh - 60px)",
          width: { xs: "100%", md: `calc(100% - ${SIDEBAR_WIDTH}px)` },
        }}
      >
        {children}
      </Box>
    </Box>
  );
}

export default function ClientShell({ children }: { children: ReactNode }) {
  const pathname = usePathname();
  const isLogin = pathname === "/login";

  return (
    <Providers>
      <AuthProvider>
        {isLogin ? children : (
          <AuthGuard>
            <ConsoleShell>{children}</ConsoleShell>
          </AuthGuard>
        )}
      </AuthProvider>
    </Providers>
  );
}
