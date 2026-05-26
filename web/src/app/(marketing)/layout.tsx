"use client";
import {
  Box,
  Button,
  Stack,
  IconButton,
  Drawer,
  List,
  ListItem,
  ListItemButton,
  ListItemText,
  Typography,
  useMediaQuery,
  useTheme,
} from "@mui/material";
import MenuIcon from "@mui/icons-material/Menu";
import Link from "next/link";
import { useState, type ReactNode } from "react";

const MARKETING_LINKS: ReadonlyArray<{ href: string; label: string }> = [
  { href: "/home", label: "Home" },
  { href: "/solutions", label: "Solutions" },
  { href: "/features", label: "Features" },
  { href: "/performance", label: "Performance" },
  { href: "/pricing", label: "Pricing" },
];

export default function MarketingLayout({ children }: { children: ReactNode }) {
  const theme = useTheme();
  const isMobile = useMediaQuery(theme.breakpoints.down("md"));
  const [drawerOpen, setDrawerOpen] = useState(false);

  return (
    <Box sx={{ maxWidth: 1200, mx: "auto", width: "100%" }}>
      {isMobile ? (
        <Stack
          direction="row"
          alignItems="center"
          justifyContent="space-between"
          sx={{ mb: 3, px: 1 }}
        >
          <Typography
            component={Link}
            href="/home"
            variant="h6"
            sx={{
              display: "flex",
              alignItems: "center",
              gap: 0.5,
              textDecoration: "none",
              userSelect: "none",
            }}
            aria-label="ZeptoDB home"
          >
            <Box component="span" sx={{ color: "primary.main", fontWeight: 900, letterSpacing: "-0.05em" }}>
              Zepto
            </Box>
            <Box component="span" sx={{ color: "text.primary", fontWeight: 300, letterSpacing: "-0.02em" }}>
              DB
            </Box>
          </Typography>
          <IconButton
            aria-label="Open navigation menu"
            onClick={() => setDrawerOpen(true)}
            sx={{ color: "text.primary" }}
          >
            <MenuIcon />
          </IconButton>
          <Drawer
            anchor="right"
            open={drawerOpen}
            onClose={() => setDrawerOpen(false)}
            ModalProps={{ keepMounted: true }}
          >
            <Box sx={{ width: 240 }} role="presentation">
              <List>
                {MARKETING_LINKS.map((l) => (
                  <ListItem key={l.href} disablePadding>
                    <ListItemButton
                      component={Link}
                      href={l.href}
                      onClick={() => setDrawerOpen(false)}
                    >
                      <ListItemText primary={l.label} />
                    </ListItemButton>
                  </ListItem>
                ))}
              </List>
            </Box>
          </Drawer>
        </Stack>
      ) : (
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
      )}
      {children}
    </Box>
  );
}
