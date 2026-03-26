"use client";
import { Box, Typography, Button } from "@mui/material";
import Link from "next/link";
import StorageIcon from "@mui/icons-material/Storage";

export default function Home() {
  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 4, alignItems: "center", justifyContent: "center", minHeight: "80vh", textAlign: "center" }}>
      <StorageIcon color="secondary" sx={{ fontSize: 80 }} />
      <Typography variant="h2" color="primary">Welcome to ZeptoDB</Typography>
      <Typography variant="h5" color="text.secondary" maxWidth={600}>
        The next-generation, ultra-low latency in-memory database tailored for the financial and HFT markets.
      </Typography>
      <Button component={Link} href="/login" variant="contained" size="large" sx={{ mt: 2 }}>Go to Console</Button>
    </Box>
  );
}
