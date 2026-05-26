"use client";
import { AppBar, Toolbar, Typography, Chip, Box, IconButton, useTheme, useMediaQuery } from "@mui/material";
import DarkModeIcon from "@mui/icons-material/DarkMode";
import LightModeIcon from "@mui/icons-material/LightMode";
import MenuIcon from "@mui/icons-material/Menu";
import { useContext } from "react";
import { ThemeModeContext } from "./Providers";
import { useQuery } from "@tanstack/react-query";
import { fetchHealth } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import { SIDEBAR_WIDTH } from "./Sidebar";
import { usePathname } from "next/navigation";

interface TopBarProps {
  /** When provided, shows a hamburger button on mobile that calls this handler. */
  onMobileMenuToggle?: () => void;
}

export default function TopBar({ onMobileMenuToggle }: TopBarProps = {}) {
  const { auth } = useAuth();
  const pathname = usePathname();
  const { data } = useQuery({ queryKey: ["health"], queryFn: () => fetchHealth(auth?.apiKey), refetchInterval: 5000 });
  const healthy = data?.status === "healthy";
  const theme = useTheme();
  const colorMode = useContext(ThemeModeContext);
  const isMobile = useMediaQuery(theme.breakpoints.down("md"));

  return (
    <AppBar
      position="fixed"
      elevation={0}
      sx={{
        width: { xs: "100%", md: `calc(100% - ${SIDEBAR_WIDTH}px)` },
        ml: { xs: 0, md: `${SIDEBAR_WIDTH}px` },
      }}
    >
      <Toolbar variant="dense" sx={{ minHeight: "60px", px: { xs: 1.5, md: 3 } }}>
        {isMobile && (
          <IconButton
            aria-label="Open navigation menu"
            edge="start"
            onClick={onMobileMenuToggle}
            sx={{ mr: 1, color: "text.primary" }}
          >
            <MenuIcon />
          </IconButton>
        )}
        <Typography variant="body1" sx={{ flexGrow: 1, color: "text.primary", fontWeight: 600, letterSpacing: "-0.01em" }}>
          {pathname === "/dashboard" ? "Console" : `Console / ${pathname.split("/").pop()}`}
        </Typography>
        <Box sx={{ display: "flex", alignItems: "center", gap: { xs: 1, md: 2 } }}>
          <Chip
            label={auth?.role ?? "Guest"}
            size="small"
            variant="outlined"
            sx={{
              textTransform: "capitalize",
              borderColor: "divider",
              color: "text.secondary",
              fontWeight: 500,
              display: { xs: "none", sm: "flex" },
            }}
          />
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
