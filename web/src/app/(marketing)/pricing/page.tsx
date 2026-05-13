"use client";
import { Box, Typography, Card, Button, Stack } from "@mui/material";

export default function Pricing() {
  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 4, alignItems: "center", pt: 2, pb: 6 }}>
      <Box sx={{ textAlign: "center" }}>
        <Typography variant="h3" component="h1" color="primary" sx={{ mb: 2 }}>
          Pricing
        </Typography>
        <Typography variant="h6" component="h2" color="text.secondary" sx={{ maxWidth: 720, mx: "auto", fontWeight: 400 }}>
          Open core, enterprise-ready when you need it.
        </Typography>
      </Box>

      <Box sx={{ display: "flex", gap: 3, flexWrap: "wrap", justifyContent: "center" }}>
        <Card
          sx={{
            width: { xs: "100%", sm: 340 },
            p: 4,
            display: "flex",
            flexDirection: "column",
            justifyContent: "space-between",
          }}
        >
          <Box>
            <Typography variant="overline" color="text.secondary">Community</Typography>
            <Typography variant="h5" component="h3" sx={{ mt: 0.5 }}>Open Source</Typography>
            <Typography variant="h3" color="secondary" sx={{ my: 2 }}>Free</Typography>
            <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>
              Full engine, standard SQL, Python DSL, Kafka / MQTT / OPC-UA connectors. Self-hosted. Apache-2.0-compatible license.
            </Typography>
            <Stack spacing={1}>
              {[
                "ClickHouse-compatible HTTP API on port 8123",
                "ASOF JOIN, Window JOIN, xbar, EMA, VWAP in SQL",
                "Kafka, MQTT, OPC-UA, FIX, NASDAQ ITCH, Binance feeds",
                "Docker images, Helm chart, single binary",
              ].map((b) => (
                <Typography key={b} variant="body2" color="text.primary">
                  · {b}
                </Typography>
              ))}
            </Stack>
          </Box>
          <Button
            component="a"
            href="https://github.com/ZeptoDB/ZeptoDB#-quick-start"
            target="_blank"
            rel="noopener noreferrer"
            variant="outlined"
            size="large"
            sx={{ mt: 4 }}
          >
            Get Started
          </Button>
        </Card>

        <Card
          sx={{
            width: { xs: "100%", sm: 340 },
            p: 4,
            border: "2px solid",
            borderColor: "primary.main",
            display: "flex",
            flexDirection: "column",
            justifyContent: "space-between",
          }}
        >
          <Box>
            <Typography variant="overline" color="primary">Recommended for production</Typography>
            <Typography variant="h5" component="h3" sx={{ mt: 0.5 }}>Enterprise</Typography>
            <Typography variant="h3" color="secondary" sx={{ my: 2 }}>Contact Us</Typography>
            <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>
              Advanced RBAC + multi-tenancy, SOC 2 / MiFID II audit log, cluster HA, priority support, licensed connectors. For production deployments across finance, factory floors, game backends, and Physical AI platforms.
            </Typography>
            <Stack spacing={1}>
              {[
                "Advanced RBAC, per-tenant namespaces, Vault-backed keys",
                "SOC 2 / EMIR / MiFID II audit log retention",
                "Multi-node HA, rolling upgrade, live rebalancing",
                "Priority support and SLAs for licensed connectors",
              ].map((b) => (
                <Typography key={b} variant="body2" color="text.primary">
                  · {b}
                </Typography>
              ))}
            </Stack>
          </Box>
          <Button
            component="a"
            href="mailto:sales@zeptodb.com?subject=ZeptoDB%20Enterprise%20Demo"
            variant="contained"
            size="large"
            sx={{ mt: 4 }}
          >
            Book a Demo
          </Button>
        </Card>
      </Box>

      <Typography variant="body2" color="text.secondary" sx={{ textAlign: "center", mt: 1 }}>
        Cloud-hosted tier coming soon.
      </Typography>
    </Box>
  );
}
