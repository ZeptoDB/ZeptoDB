"use client";
import { Box, Paper, Typography, CircularProgress, Alert, Stack, Divider, Chip } from "@mui/material";
import { useQuery } from "@tanstack/react-query";
import { fetchSettings, fetchVersion, fetchHealth } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import SettingsIcon from "@mui/icons-material/Settings";
import LockIcon from "@mui/icons-material/LockOutlined";
import HubIcon from "@mui/icons-material/HubOutlined";
import AccessTimeIcon from "@mui/icons-material/AccessTime";
import PeopleAltIcon from "@mui/icons-material/PeopleAlt";
import InfoIcon from "@mui/icons-material/InfoOutlined";
import FavoriteIcon from "@mui/icons-material/FavoriteBorder";

const MONO = { fontFamily: "'JetBrains Mono', monospace", fontSize: 13 };

function Row({ icon, label, description, children }: { icon: React.ReactNode; label: string; description: string; children: React.ReactNode }) {
  return (
    <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between" }}>
      <Box sx={{ display: "flex", alignItems: "center", gap: 2 }}>
        {icon}
        <Box>
          <Typography variant="body1" fontWeight={600}>{label}</Typography>
          <Typography variant="body2" color="text.secondary">{description}</Typography>
        </Box>
      </Box>
      {children}
    </Box>
  );
}

export default function SettingsPage() {
  const { auth } = useAuth();

  const { data: settings, isLoading, error } = useQuery({
    queryKey: ["admin-settings"],
    queryFn: () => fetchSettings(auth?.apiKey),
    refetchInterval: 10000,
  });
  const { data: version } = useQuery({
    queryKey: ["version"],
    queryFn: () => fetchVersion(auth?.apiKey),
  });
  const { data: health } = useQuery({
    queryKey: ["health-settings"],
    queryFn: () => fetchHealth(auth?.apiKey),
    refetchInterval: 5000,
  });

  if (isLoading) {
    return <Box sx={{ display: "flex", justifyContent: "center", py: 8 }}><CircularProgress color="primary" /></Box>;
  }

  if (error || !settings) {
    return <Alert severity="error">Failed to load settings. Ensure you have admin access and the server is running.</Alert>;
  }

  const healthy = health?.status === "healthy";

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 3, maxWidth: 800, mx: "auto", mt: 4 }}>
      <Typography variant="h5" fontWeight={600} color="text.primary" sx={{ display: "flex", alignItems: "center", gap: 1 }}>
        <SettingsIcon color="primary" /> Server Configuration
      </Typography>

      {/* Server info */}
      {version && (
        <Paper sx={{ p: 3 }}>
          <Typography variant="subtitle2" color="text.secondary" sx={{ textTransform: "uppercase", fontWeight: 700, mb: 2 }}>
            Server Info
          </Typography>
          <Stack spacing={2} divider={<Divider />}>
            <Row icon={<InfoIcon color="action" />} label="Engine" description="Database engine name and version">
              <Typography sx={MONO}>{version.engine} v{version.version}</Typography>
            </Row>
            <Row icon={<InfoIcon color="action" />} label="Build Date" description="Server binary compilation date">
              <Typography sx={MONO}>{version.build}</Typography>
            </Row>
            <Row icon={<FavoriteIcon color="action" />} label="Health" description="Current server health status">
              <Chip label={healthy ? "Healthy" : "Unhealthy"} color={healthy ? "success" : "error"} variant="outlined" size="small" />
            </Row>
          </Stack>
        </Paper>
      )}

      {/* Runtime config */}
      <Paper sx={{ p: 3 }}>
        <Typography variant="subtitle2" color="text.secondary" sx={{ textTransform: "uppercase", fontWeight: 700, mb: 2 }}>
          Runtime Properties (Read-only)
        </Typography>
        <Stack spacing={2} divider={<Divider />}>
          <Row icon={<HubIcon color="action" />} label="Listen Port" description="The main HTTP/HTTPS port the server is bound to">
            <Typography sx={MONO}>{settings.port}</Typography>
          </Row>
          <Row icon={<LockIcon color="action" />} label="TLS (HTTPS) Encryption" description="Secure encrypted communication">
            <Chip label={settings.tls_enabled ? "Enabled" : "Disabled"} color={settings.tls_enabled ? "success" : "default"} variant="outlined" size="small" />
          </Row>
          <Row icon={<LockIcon color="action" />} label="Authentication" description="Is ZeptoDB enforcing API Keys / Tokens?">
            <Chip label={settings.auth_enabled ? "Enforced" : "Open"} color={settings.auth_enabled ? "primary" : "error"} variant="outlined" size="small" />
          </Row>
          <Row icon={<AccessTimeIcon color="action" />} label="Query Timeout" description="Maximum duration a query is allowed to execute">
            <Typography sx={MONO}>{settings.query_timeout_ms === 0 ? "Unlimited" : `${settings.query_timeout_ms} ms`}</Typography>
          </Row>
          <Row icon={<PeopleAltIcon color="action" />} label="Multi-Tenancy Mode" description="Resource isolation and quota enforcement per tenant">
            <Chip label={settings.multi_tenancy_enabled ? "Active" : "Inactive"} color={settings.multi_tenancy_enabled ? "primary" : "default"} variant="outlined" size="small" />
          </Row>
          <Row icon={<HubIcon color="action" />} label="Cluster Mode" description="Distributed query execution over multiple nodes">
            <Chip label={settings.cluster_mode ? "Coordinating" : "Standalone"} color={settings.cluster_mode ? "primary" : "default"} variant="outlined" size="small" />
          </Row>
        </Stack>
      </Paper>
    </Box>
  );
}
