"use client";
import { Box, Button, Stack } from "@mui/material";
import Link from "next/link";
import type { ReactNode } from "react";

const MARKETING_LINKS: ReadonlyArray<{ href: string; label: string }> = [
  { href: "/home", label: "Home" },
  { href: "/solutions", label: "Solutions" },
  { href: "/features", label: "Features" },
  { href: "/performance", label: "Performance" },
  { href: "/pricing", label: "Pricing" },
];

export default function MarketingLayout({ children }: { children: ReactNode }) {
  return (
    <Box sx={{ maxWidth: 1200, mx: "auto", width: "100%" }}>
      <Stack
        direction="row"
        spacing={1}
        sx={{ mb: 3, flexWrap: "wrap", justifyContent: "center", rowGap: 1 }}
      >
        {MARKETING_LINKS.map((l) => (
          <Button
            key={l.href}
            component={Link}
            href={l.href}
            size="small"
            variant="text"
            sx={{ color: "text.secondary", "&:hover": { color: "primary.main" } }}
          >
            {l.label}
          </Button>
        ))}
      </Stack>
      {children}
    </Box>
  );
}
