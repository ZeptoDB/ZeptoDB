"use client";
import { AppBar, Toolbar, Typography, Chip, Box } from "@mui/material";
import { useQuery } from "@tanstack/react-query";
import { fetchHealth } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import { SIDEBAR_WIDTH } from "./Sidebar";

export default function TopBar() {
  const { auth } = useAuth();
  const { data } = useQuery({ queryKey: ["health"], queryFn: () => fetchHealth(auth?.apiKey), refetchInterval: 5000 });
  const healthy = data?.status === "healthy";
  return (
    <AppBar position="fixed" elevation={0} sx={{ width: `calc(100% - ${SIDEBAR_WIDTH}px)`, ml: `${SIDEBAR_WIDTH}px` }}>
      <Toolbar variant="dense">
        <Typography variant="body2" sx={{ flexGrow: 1, color: "text.secondary" }}>Console</Typography>
        <Box sx={{ display: "flex", alignItems: "center", gap: 1 }}>
          <Chip label={auth?.role ?? ""} size="small" variant="outlined" sx={{ textTransform: "capitalize" }} />
          <Chip label={healthy ? "Connected" : "Disconnected"} size="small" color={healthy ? "success" : "error"} variant="outlined" />
        </Box>
      </Toolbar>
    </AppBar>
  );
}
