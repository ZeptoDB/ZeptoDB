"use client";
import { Box, Paper, Typography, CircularProgress, Alert, Stack, Divider, Chip } from "@mui/material";
import { useQuery } from "@tanstack/react-query";
import { fetchSettings } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import SettingsIcon from "@mui/icons-material/Settings";
import LockIcon from "@mui/icons-material/LockOutlined";
import HubIcon from "@mui/icons-material/HubOutlined";
import AccessTimeIcon from "@mui/icons-material/AccessTime";
import PeopleAltIcon from "@mui/icons-material/PeopleAlt";

const MONO = { fontFamily: "'JetBrains Mono', monospace", fontSize: 13 };

export default function SettingsPage() {
  const { auth } = useAuth();
  
  const { data: settings, isLoading, error } = useQuery({
    queryKey: ["admin-settings"],
    queryFn: () => fetchSettings(auth?.apiKey),
    refetchInterval: 10000,
  });

  if (isLoading) {
    return (
      <Box sx={{ display: "flex", justifyContent: "center", py: 8 }}>
        <CircularProgress color="primary" />
      </Box>
    );
  }

  if (error || !settings) {
    return (
      <Alert severity="error">
        Failed to load settings. Ensure you have admin access and the server is running.
      </Alert>
    );
  }

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 3, maxWidth: 800, mx: "auto", mt: 4 }}>
      <Typography variant="h5" fontWeight={600} color="text.primary" sx={{ display: "flex", alignItems: "center", gap: 1 }}>
        <SettingsIcon color="primary" /> Server Configuration
      </Typography>

      <Paper sx={{ p: 4 }}>
        <Typography variant="subtitle2" color="text.secondary" sx={{ textTransform: "uppercase", fontWeight: 700, mb: 3 }}>
          Runtime Properties (Read-only)
        </Typography>

        <Stack spacing={3} divider={<Divider />}>
          <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between" }}>
            <Box sx={{ display: "flex", alignItems: "center", gap: 2 }}>
              <HubIcon color="action" />
              <Box>
                <Typography variant="body1" fontWeight={600}>Listen Port</Typography>
                <Typography variant="body2" color="text.secondary">The main HTTP/HTTPS port the server is bound to</Typography>
              </Box>
            </Box>
            <Typography sx={MONO}>{settings.port}</Typography>
          </Box>

          <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between" }}>
            <Box sx={{ display: "flex", alignItems: "center", gap: 2 }}>
              <LockIcon color="action" />
              <Box>
                <Typography variant="body1" fontWeight={600}>TLS (HTTPS) Encryption</Typography>
                <Typography variant="body2" color="text.secondary">Secure encrypted communication bounds</Typography>
              </Box>
            </Box>
            <Chip label={settings.tls_enabled ? "Enabled" : "Disabled"} color={settings.tls_enabled ? "success" : "default"} variant="outlined" size="small" />
          </Box>

          <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between" }}>
            <Box sx={{ display: "flex", alignItems: "center", gap: 2 }}>
              <LockIcon color="action" />
              <Box>
                <Typography variant="body1" fontWeight={600}>Authentication</Typography>
                <Typography variant="body2" color="text.secondary">Is ZeptoDB enforcing API Keys / Tokens?</Typography>
              </Box>
            </Box>
            <Chip label={settings.auth_enabled ? "Enforced" : "Open"} color={settings.auth_enabled ? "primary" : "error"} variant="outlined" size="small" />
          </Box>

          <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between" }}>
            <Box sx={{ display: "flex", alignItems: "center", gap: 2 }}>
              <AccessTimeIcon color="action" />
              <Box>
                <Typography variant="body1" fontWeight={600}>Query Timeout</Typography>
                <Typography variant="body2" color="text.secondary">Maximum duration a query is allowed to execute</Typography>
              </Box>
            </Box>
            <Typography sx={MONO}>{settings.query_timeout_ms === 0 ? "Unlimited" : `${settings.query_timeout_ms} ms`}</Typography>
          </Box>

          <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between" }}>
            <Box sx={{ display: "flex", alignItems: "center", gap: 2 }}>
              <PeopleAltIcon color="action" />
              <Box>
                <Typography variant="body1" fontWeight={600}>Multi-Tenancy Mode</Typography>
                <Typography variant="body2" color="text.secondary">Resource isolation and quota enforcement per tenant</Typography>
              </Box>
            </Box>
            <Chip label={settings.multi_tenancy_enabled ? "Active" : "Inactive"} color={settings.multi_tenancy_enabled ? "primary" : "default"} variant="outlined" size="small" />
          </Box>

          <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between" }}>
            <Box sx={{ display: "flex", alignItems: "center", gap: 2 }}>
              <HubIcon color="action" />
              <Box>
                <Typography variant="body1" fontWeight={600}>Cluster Mode</Typography>
                <Typography variant="body2" color="text.secondary">Distributed query execution over multiple nodes</Typography>
              </Box>
            </Box>
            <Chip label={settings.cluster_mode ? "Coordinating" : "Standalone"} color={settings.cluster_mode ? "primary" : "default"} variant="outlined" size="small" />
          </Box>
        </Stack>
      </Paper>
    </Box>
  );
}
