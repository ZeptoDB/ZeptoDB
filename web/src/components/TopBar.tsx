"use client";
import { AppBar, Toolbar, Typography, Chip, Box, IconButton, useTheme } from "@mui/material";
import DarkModeIcon from "@mui/icons-material/DarkMode";
import LightModeIcon from "@mui/icons-material/LightMode";
import { useContext } from "react";
import { ThemeModeContext } from "./Providers";
import { useQuery } from "@tanstack/react-query";
import { fetchHealth } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import { SIDEBAR_WIDTH } from "./Sidebar";
import { usePathname } from "next/navigation";

export default function TopBar() {
  const { auth } = useAuth();
  const pathname = usePathname();
  const { data } = useQuery({ queryKey: ["health"], queryFn: () => fetchHealth(auth?.apiKey), refetchInterval: 5000 });
  const healthy = data?.status === "healthy";
  const theme = useTheme();
  const colorMode = useContext(ThemeModeContext);

  return (
    <AppBar position="fixed" elevation={0} sx={{ width: `calc(100% - ${SIDEBAR_WIDTH}px)`, ml: `${SIDEBAR_WIDTH}px` }}>
      <Toolbar variant="dense" sx={{ minHeight: "60px", px: 3 }}>
        <Typography variant="body1" sx={{ flexGrow: 1, color: "text.primary", fontWeight: 600, letterSpacing: "-0.01em" }}>
          {pathname === "/dashboard" ? "Console" : `Console / ${pathname.split("/").pop()}`}
        </Typography>
        <Box sx={{ display: "flex", alignItems: "center", gap: 2 }}>
          <Chip label={auth?.role ?? "Guest"} size="small" variant="outlined" sx={{ textTransform: "capitalize", borderColor: "divider", color: "text.secondary", fontWeight: 500 }} />
          <Box sx={{ display: "flex", alignItems: "center", gap: 1, px: 1.5, py: 0.5, borderRadius: 1.5, bgcolor: "background.paper", border: "1px solid", borderColor: "divider" }}>
            <Box sx={{ width: 8, height: 8, borderRadius: "50%", bgcolor: healthy ? "success.main" : "error.main", boxShadow: (theme) => `0 0 8px ${healthy ? theme.palette.success.main : theme.palette.error.main}80` }} />
            <Typography variant="caption" sx={{ color: "text.secondary", fontWeight: 600, letterSpacing: "0.02em", textTransform: "uppercase" }}>
              {healthy ? "Connected" : "Disconnected"}
            </Typography>
          </Box>
          <IconButton size="small" onClick={colorMode.toggleColorMode} sx={{ color: "text.secondary", "&:hover": { color: "primary.main", bgcolor: "divider" } }}>
            {theme.palette.mode === "dark" ? <LightModeIcon fontSize="small" /> : <DarkModeIcon fontSize="small" />}
          </IconButton>
        </Box>
      </Toolbar>
    </AppBar>
  );
}
