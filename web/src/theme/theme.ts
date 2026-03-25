"use client";
import { createTheme } from "@mui/material/styles";

const theme = createTheme({
  palette: {
    mode: "dark",
    primary: { main: "#5C6BC0" },
    secondary: { main: "#FFC107" },
    background: { default: "#0A0E1A", paper: "#111827" },
  },
  typography: {
    fontFamily: "'Inter', sans-serif",
    h6: { fontWeight: 700 },
  },
  shape: { borderRadius: 10 },
  components: {
    MuiDrawer: {
      styleOverrides: {
        paper: { backgroundColor: "#0D1220", borderRight: "1px solid #1E293B" },
      },
    },
    MuiAppBar: {
      styleOverrides: {
        root: { backgroundColor: "#0D1220", borderBottom: "1px solid #1E293B" },
      },
    },
  },
});

export default theme;
