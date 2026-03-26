"use client";
import { Box, Typography, Card, Button } from "@mui/material";

export default function Pricing() {
  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 4, alignItems: "center", p: 4, mt: 4 }}>
      <Typography variant="h3" color="primary">Pricing</Typography>
      <Typography variant="h6" color="text.secondary" mb={4}>Choose your grade</Typography>

      <Box sx={{ display: "flex", gap: 3 }}>
        <Card sx={{ width: 300, textAlign: "center", p: 4, display: "flex", flexDirection: "column", justifyContent: "space-between" }}>
          <Box>
            <Typography variant="h5">Open Source</Typography>
            <Typography variant="h4" color="secondary" sx={{ my: 2 }}>Free</Typography>
            <Typography color="text.secondary">Community support and standard engine limits.</Typography>
          </Box>
          <Button variant="outlined" sx={{ mt: 4 }}>Get Started</Button>
        </Card>

        <Card sx={{ width: 300, textAlign: "center", p: 4, border: "2px solid", borderColor: "primary.main", display: "flex", flexDirection: "column", justifyContent: "space-between" }}>
          <Box>
            <Typography variant="h5">Enterprise</Typography>
            <Typography variant="h4" color="secondary" sx={{ my: 2 }}>Contact Us</Typography>
            <Typography color="text.secondary">Advanced RBAC, 24/7 priority support, limitless multi-tenant clusters.</Typography>
          </Box>
          <Button variant="contained" sx={{ mt: 4 }}>Book Meeting</Button>
        </Card>
      </Box>
    </Box>
  );
}
