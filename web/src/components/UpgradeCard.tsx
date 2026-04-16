"use client";
import { Typography, Button, Paper } from "@mui/material";
import LockIcon from "@mui/icons-material/Lock";

interface Props {
  feature: string;
  description: string;
}

export default function UpgradeCard({ feature, description }: Props) {
  return (
    <Paper sx={{ p: 4, textAlign: "center", maxWidth: 480, mx: "auto", mt: 8 }}>
      <LockIcon sx={{ fontSize: 48, color: "text.secondary", mb: 2 }} />
      <Typography variant="h5" gutterBottom>{feature}</Typography>
      <Typography color="text.secondary" sx={{ mb: 3 }}>{description}</Typography>
      <Button variant="contained" href="https://zeptodb.com/pricing" target="_blank" rel="noopener noreferrer">
        View Plans
      </Button>
    </Paper>
  );
}
