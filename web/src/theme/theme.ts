"use client";
import { createTheme, alpha } from "@mui/material/styles";
import type { PaletteMode } from "@mui/material";

const buildTheme = (mode: PaletteMode) => {
  const isDark = mode === "dark";
  
  // Electric Indigo Brand Theme
  const backgroundDefault = isDark ? "#0A0C10" : "#F8FAFC";
  const backgroundPaper = isDark ? "#11161D" : "rgba(255, 255, 255, 0.8)";
  const surfaceElevation = "#1B2129";
  const primaryMain = isDark ? "#4D7CFF" : "#2563EB";
  const secondaryMain = isDark ? "#00F5D4" : "#7C3AED";
  const textPrimary = isDark ? "#F8FAFC" : "#0F172A";
  const textSecondary = isDark ? "#94A3B8" : "#64748B";
  const borderColor = isDark ? "rgba(255, 255, 255, 0.08)" : "rgba(0, 0, 0, 0.08)";

  return createTheme({
    palette: {
      mode,
      primary: { main: primaryMain },
      secondary: { main: secondaryMain },
      success: { main: "#00E676" },
      error: { main: "#FF1744" },
      warning: { main: "#FFB300" },
      info: { main: "#2979FF" },
      background: { default: backgroundDefault, paper: backgroundPaper },
      text: { primary: textPrimary, secondary: textSecondary },
      divider: borderColor,
    },
    typography: {
      fontFamily: "'Inter', sans-serif",
      h1: { fontWeight: 800, letterSpacing: "-0.02em" },
      h2: { fontWeight: 800, letterSpacing: "-0.02em" },
      h3: { fontWeight: 700, letterSpacing: "-0.02em" },
      h4: { fontWeight: 700, letterSpacing: "-0.02em" },
      h5: { fontWeight: 700, letterSpacing: "-0.02em" },
      h6: { fontWeight: 700, letterSpacing: "-0.02em" },
      button: { fontWeight: 600, textTransform: "none" },
      body1: { fontSize: "0.95rem" },
      body2: { fontSize: "0.875rem" },
      subtitle1: { fontWeight: 600 },
      subtitle2: { fontWeight: 600 },
    },
    shape: { borderRadius: 8 },
    components: {
      MuiCssBaseline: {
        styleOverrides: {
          body: {
            backgroundColor: backgroundDefault,
            transition: "background-color 0.3s ease",
            "&::-webkit-scrollbar": { width: 8, height: 8 },
            "&::-webkit-scrollbar-track": { background: "transparent" },
            "&::-webkit-scrollbar-thumb": { 
              background: isDark ? "rgba(255,255,255,0.1)" : "rgba(0,0,0,0.1)", 
              borderRadius: 4 
            },
            "&::-webkit-scrollbar-thumb:hover": { 
              background: isDark ? "rgba(255,255,255,0.2)" : "rgba(0,0,0,0.2)" 
            },
          },
        },
      },
      MuiPaper: {
        styleOverrides: {
          root: {
            backgroundImage: "none",
            backgroundColor: backgroundPaper,
            backdropFilter: "blur(16px)",
            WebkitBackdropFilter: "blur(16px)",
            border: `1px solid ${borderColor}`,
            boxShadow: isDark 
              ? "0 4px 24px rgba(0, 0, 0, 0.4)" 
              : "0 4px 24px rgba(0, 0, 0, 0.05)",
          },
        },
      },
      MuiButton: {
        styleOverrides: {
          root: {
            borderRadius: 6,
            transition: "all 0.2s cubic-bezier(0.25, 0.46, 0.45, 0.94)",
            "&:active": { transform: "scale(0.97)" },
          },
          containedPrimary: {
            boxShadow: `0 0 8px rgba(77, 124, 255, 0.3)`,
            "&:hover": {
              boxShadow: `0 0 16px rgba(77, 124, 255, 0.5)`,
              transform: "translateY(-1px)",
            },
          },
        },
      },
      MuiCard: {
        styleOverrides: {
          root: {
            backgroundColor: backgroundPaper,
            backdropFilter: "blur(16px)",
            border: `1px solid ${borderColor}`,
            borderRadius: 12,
            transition: "all 0.3s ease",
            "&:hover": {
              border: `1px solid ${alpha(primaryMain, 0.3)}`,
              boxShadow: isDark 
                ? `0 8px 32px ${alpha(primaryMain, 0.1)}`
                : `0 8px 32px ${alpha(primaryMain, 0.05)}`,
            },
          },
        },
      },
      MuiDrawer: {
        styleOverrides: {
          paper: {
            backgroundColor: isDark ? "#0A0C10" : "#FFFFFF",
            borderRight: `1px solid ${borderColor}`,
            backgroundImage: "none",
          },
        },
      },
      MuiAppBar: {
        styleOverrides: {
          root: {
            backgroundColor: isDark ? "rgba(10, 12, 16, 0.7)" : "rgba(255, 255, 255, 0.7)",
            backdropFilter: "blur(20px)",
            WebkitBackdropFilter: "blur(20px)",
            borderBottom: `1px solid ${borderColor}`,
            backgroundImage: "none",
            boxShadow: "none",
          },
        },
      },
      MuiListItemButton: {
        styleOverrides: {
          root: {
            borderRadius: 6,
            marginBottom: 2,
            "&.Mui-selected": {
              backgroundColor: alpha(primaryMain, 0.1),
              color: primaryMain,
              "&:hover": { backgroundColor: alpha(primaryMain, 0.15) },
              "& .MuiListItemIcon-root": { color: primaryMain },
              "& .MuiListItemText-primary": { color: primaryMain, fontWeight: 600 },
            },
            "&:hover": { backgroundColor: alpha(primaryMain, 0.05) },
          },
        },
      },
      MuiTableCell: {
        styleOverrides: {
          root: {
            borderBottom: `1px solid ${borderColor}`,
          },
          head: {
            fontWeight: 600,
            color: textSecondary,
            backgroundColor: isDark ? alpha("#000", 0.2) : alpha("#fff", 0.5),
          },
        },
      },
    },
  });
};

export const getTheme = (mode: PaletteMode) => buildTheme(mode);
export default buildTheme("dark");
