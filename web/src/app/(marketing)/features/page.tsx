"use client";
import { Box, Typography, Card } from "@mui/material";

export default function Features() {
  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 4, alignItems: "center", p: 4, textAlign: "center" }}>
      <Typography variant="h3" color="primary">Platform Features</Typography>
      <Typography variant="h6" color="text.secondary">Engineered to scale limits</Typography>
      
      <Box sx={{ display: "flex", gap: 3, flexWrap: "wrap", justifyContent: "center", mt: 4 }}>
        <Card sx={{ p: 4, width: 300 }}>
          <Typography variant="h5" color="secondary" mb={2}>Ultra-Low Latency</Typography>
          <Typography color="text.secondary">Bespoke C++ engine optimized for minimal cache-misses and CPU zero-copy buffers.</Typography>
        </Card>
        <Card sx={{ p: 4, width: 300 }}>
          <Typography variant="h5" color="secondary" mb={2}>Lock-free Execution</Typography>
          <Typography color="text.secondary">Seamlessly handle millions of rows concurrently via our revolutionary pipeline structures.</Typography>
        </Card>
        <Card sx={{ p: 4, width: 300 }}>
          <Typography variant="h5" color="secondary" mb={2}>Enterprise RBAC</Typography>
          <Typography color="text.secondary">Deeply integrated authentication scopes and multi-tenancy limits for heavy workload isolation.</Typography>
        </Card>
      </Box>
    </Box>
  );
}
