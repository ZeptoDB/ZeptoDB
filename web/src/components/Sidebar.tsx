"use client";
import { usePathname, useRouter } from "next/navigation";
import {
  Drawer, List, ListItemButton, ListItemIcon, ListItemText,
  Toolbar, Typography, Box, Divider,
} from "@mui/material";
import TerminalIcon from "@mui/icons-material/Terminal";
import DashboardIcon from "@mui/icons-material/Dashboard";
import StorageIcon from "@mui/icons-material/Storage";
import HubIcon from "@mui/icons-material/Hub";
import LogoutIcon from "@mui/icons-material/Logout";
import { useAuth } from "@/lib/auth";

const WIDTH = 220;

interface NavItem { label: string; href: string; icon: React.ReactNode; roles: string[] }

const nav: NavItem[] = [
  { label: "Query", href: "/query", icon: <TerminalIcon />, roles: ["admin", "writer", "reader", "analyst"] },
  { label: "Dashboard", href: "/dashboard", icon: <DashboardIcon />, roles: ["admin", "writer", "reader", "metrics"] },
  { label: "Tables", href: "/tables", icon: <StorageIcon />, roles: ["admin", "writer", "reader", "analyst"] },
  { label: "Cluster", href: "/cluster", icon: <HubIcon />, roles: ["admin", "metrics"] },
];

export function getVisibleNav(role: string): NavItem[] {
  return nav.filter((n) => n.roles.includes(role));
}

export default function Sidebar() {
  const pathname = usePathname();
  const router = useRouter();
  const { auth, logout } = useAuth();

  const handleLogout = () => { logout(); router.push("/login"); };
  const visible = getVisibleNav(auth?.role ?? "reader");

  return (
    <Drawer variant="permanent" sx={{ width: WIDTH, flexShrink: 0, "& .MuiDrawer-paper": { width: WIDTH, boxSizing: "border-box" } }}>
      <Toolbar>
        <Typography variant="h6" sx={{ fontWeight: 700, background: "linear-gradient(135deg,#5C6BC0,#FFC107)", WebkitBackgroundClip: "text", WebkitTextFillColor: "transparent" }}>
          ZeptoDB
        </Typography>
      </Toolbar>
      <Divider />
      <List sx={{ flex: 1 }}>
        {visible.map((n) => (
          <ListItemButton key={n.href} selected={pathname === n.href} onClick={() => router.push(n.href)} sx={{ borderRadius: 2, mx: 1, mb: 0.5 }}>
            <ListItemIcon sx={{ minWidth: 36, color: pathname === n.href ? "primary.main" : "text.secondary" }}>{n.icon}</ListItemIcon>
            <ListItemText primary={n.label} primaryTypographyProps={{ fontSize: 14 }} />
          </ListItemButton>
        ))}
      </List>
      <Divider />
      <List>
        <ListItemButton onClick={handleLogout} sx={{ borderRadius: 2, mx: 1, mb: 1 }}>
          <ListItemIcon sx={{ minWidth: 36, color: "text.secondary" }}><LogoutIcon /></ListItemIcon>
          <ListItemText primary={auth?.role ?? "user"} primaryTypographyProps={{ fontSize: 12, color: "text.secondary" }} secondary="Sign out" secondaryTypographyProps={{ fontSize: 11 }} />
        </ListItemButton>
      </List>
    </Drawer>
  );
}

export { WIDTH as SIDEBAR_WIDTH };
