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
      <Box sx={{ display: "flex", gap: 2, mt: 2 }}>
        <Button component={Link} href="/login" variant="contained" size="large">Go to Console</Button>
        <Button component="a" href="https://discord.gg/zeptodb" target="_blank" rel="noopener noreferrer" variant="outlined" size="large" sx={{ borderColor: "#5865F2", color: "#5865F2", "&:hover": { borderColor: "#4752C4", bgcolor: "#5865F210" } }}>Join Discord</Button>
      </Box>
    </Box>
  );
}
