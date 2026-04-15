"use client";
import { usePathname, useRouter } from "next/navigation";
import {
  Drawer, List, ListItemButton, ListItemIcon, ListItemText,
  Toolbar, Typography, Box, Divider,
} from "@mui/material";
import TerminalIcon from "@mui/icons-material/Terminal";
import DashboardIcon from "@mui/icons-material/Dashboard";
import StorageIcon from "@mui/icons-material/Storage";
import HubIcon from "@mui/icons-material/HubOutlined";
import AdminPanelSettingsIcon from "@mui/icons-material/AdminPanelSettings";
import SettingsIcon from "@mui/icons-material/Settings";
import ExitToAppIcon from "@mui/icons-material/ExitToApp";
import ForumIcon from "@mui/icons-material/Forum";
import GroupsIcon from "@mui/icons-material/Groups";
import { useAuth } from "@/lib/auth";

const WIDTH = 220;

interface NavItem { label: string; href: string; icon: React.ReactNode; roles: string[] }

const nav: NavItem[] = [
  { label: "Dashboard", href: "/dashboard", icon: <DashboardIcon />, roles: ["admin", "writer", "reader", "analyst", "metrics"] },
  { label: "Query", href: "/query", icon: <TerminalIcon />, roles: ["admin", "writer", "reader", "analyst"] },
  { label: "Tables", href: "/tables", icon: <StorageIcon />, roles: ["admin", "writer", "reader", "analyst"] },
  { label: "Cluster", href: "/cluster", icon: <HubIcon />, roles: ["admin"] },
  { label: "Tenants", href: "/tenants", icon: <GroupsIcon />, roles: ["admin"] },
  { label: "Admin", href: "/admin", icon: <AdminPanelSettingsIcon />, roles: ["admin"] },
  { label: "Settings", href: "/settings", icon: <SettingsIcon />, roles: ["admin"] },
];

export function getVisibleNav(role: string): NavItem[] {
  return nav.filter((n) => n.roles.includes(role));
}

export default function Sidebar() {
  const pathname = usePathname();
  const router = useRouter();
  const { auth, logout } = useAuth();

  const handleLogout = () => { logout().then(() => router.push("/login")); };
  const visible = getVisibleNav(auth?.role ?? "reader");

  return (
    <Drawer variant="permanent" sx={{ width: WIDTH, flexShrink: 0, "& .MuiDrawer-paper": { width: WIDTH, boxSizing: "border-box" } }}>
      <Toolbar sx={{ px: 3, pt: 2, pb: 1, justifyContent: "center" }}>
        <Typography variant="h5" sx={{ display: "flex", alignItems: "center", gap: 0.5, userSelect: "none" }}>
          <Box component="span" sx={{ 
            color: "primary.main", 
            fontWeight: 900, 
            letterSpacing: "-0.05em",
            textShadow: (theme) => `0 0 16px ${theme.palette.primary.main}80`
          }}>Zepto</Box>
          <Box component="span" sx={{ color: "text.primary", fontWeight: 300, letterSpacing: "-0.02em" }}>DB</Box>
        </Typography>
      </Toolbar>
      <Divider sx={{ mx: 2, mb: 2, opacity: 0.5 }} />
      <List sx={{ flex: 1, px: 1 }}>
        {visible.map((n) => (
          <ListItemButton key={n.href} selected={pathname === n.href} onClick={() => router.push(n.href)}>
            <ListItemIcon sx={{ minWidth: 40, color: pathname === n.href ? "primary.main" : "text.secondary" }}>{n.icon}</ListItemIcon>
            <ListItemText primary={n.label} primaryTypographyProps={{ fontSize: "0.9rem", fontWeight: pathname === n.href ? 600 : 500 }} />
          </ListItemButton>
        ))}
      </List>
      <Divider sx={{ mx: 2, my: 1, opacity: 0.5 }} />
      <List sx={{ px: 1, pb: 2 }}>
        <ListItemButton component="a" href="https://discord.gg/zeptodb" target="_blank" rel="noopener noreferrer">
          <ListItemIcon sx={{ minWidth: 40, color: "text.secondary" }}><ForumIcon fontSize="small" /></ListItemIcon>
          <ListItemText primary="Discord" primaryTypographyProps={{ fontSize: "0.9rem", fontWeight: 500, color: "text.secondary" }} />
        </ListItemButton>
        <ListItemButton onClick={handleLogout}>
          <ListItemIcon sx={{ minWidth: 40, color: "text.secondary" }}><ExitToAppIcon fontSize="small" /></ListItemIcon>
          <ListItemText primary="Logout" primaryTypographyProps={{ fontSize: "0.9rem", fontWeight: 500, color: "text.secondary" }} />
        </ListItemButton>
      </List>
    </Drawer>
  );
}

export { WIDTH as SIDEBAR_WIDTH };
