"use client";
import { useState } from "react";
import { useRouter } from "next/navigation";
import { Box, Paper, Typography, TextField, Button, Alert, InputAdornment, IconButton, Divider, Tab, Tabs } from "@mui/material";
import Visibility from "@mui/icons-material/Visibility";
import VisibilityOff from "@mui/icons-material/VisibilityOff";
import KeyIcon from "@mui/icons-material/Key";
import TokenIcon from "@mui/icons-material/Token";
import { useAuth } from "@/lib/auth";

export default function LoginPage() {
  const [tab, setTab] = useState(0);
  const [key, setKey] = useState("");
  const [jwt, setJwt] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [show, setShow] = useState(false);
  const { login } = useAuth();
  const router = useRouter();

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setLoading(true);
    setError(null);
    const token = tab === 0 ? key : jwt;
    if (!token) { setError("Please enter credentials."); setLoading(false); return; }
    try {
      await login(token);
      router.push("/query");
    } catch (err) {
      setError(err instanceof Error ? err.message : "Authentication failed");
    } finally {
      setLoading(false);
    }
  };

  return (
    <Box sx={{ minHeight: "100vh", display: "flex", alignItems: "center", justifyContent: "center", bgcolor: "#0A0C10" }}>
      <Paper sx={{ p: 4, width: 420, border: "1px solid rgba(255, 255, 255, 0.08)" }}>
        <Box sx={{ textAlign: "center", mb: 3 }}>
          <Typography variant="h5" sx={{ fontWeight: 700, background: "linear-gradient(135deg,#4D7CFF,#00F5D4)", WebkitBackgroundClip: "text", WebkitTextFillColor: "transparent" }}>
            ZeptoDB
          </Typography>
          <Typography variant="body2" color="text.secondary" sx={{ mt: 0.5 }}>Sign in to Console</Typography>
        </Box>

        <Tabs value={tab} onChange={(_, v) => { setTab(v); setError(null); }} variant="fullWidth" sx={{ mb: 2 }}>
          <Tab icon={<KeyIcon sx={{ fontSize: 18 }} />} iconPosition="start" label="API Key" sx={{ textTransform: "none", minHeight: 42 }} />
          <Tab icon={<TokenIcon sx={{ fontSize: 18 }} />} iconPosition="start" label="JWT / SSO" sx={{ textTransform: "none", minHeight: 42 }} />
        </Tabs>

        <form onSubmit={handleSubmit}>
          {tab === 0 ? (
            <TextField
              fullWidth label="API Key" placeholder="zepto_..." value={key} onChange={(e) => setKey(e.target.value)}
              type={show ? "text" : "password"} autoFocus sx={{ mb: 2 }}
              slotProps={{ input: { endAdornment: (
                <InputAdornment position="end">
                  <IconButton onClick={() => setShow(!show)} edge="end" size="small">{show ? <VisibilityOff /> : <Visibility />}</IconButton>
                </InputAdornment>
              ), sx: { fontFamily: "'JetBrains Mono', monospace", fontSize: 14 } } }}
            />
          ) : (
            <TextField
              fullWidth label="JWT Token" placeholder="eyJhbGciOi..." value={jwt} onChange={(e) => setJwt(e.target.value)}
              type={show ? "text" : "password"} autoFocus multiline minRows={3} maxRows={5} sx={{ mb: 2 }}
              slotProps={{ input: { endAdornment: (
                <InputAdornment position="end">
                  <IconButton onClick={() => setShow(!show)} edge="end" size="small">{show ? <VisibilityOff /> : <Visibility />}</IconButton>
                </InputAdornment>
              ), sx: { fontFamily: "'JetBrains Mono', monospace", fontSize: 12 } } }}
            />
          )}

          {error && <Alert severity="error" variant="outlined" sx={{ mb: 2 }}>{error}</Alert>}

          <Button type="submit" variant="contained" fullWidth disabled={loading || (tab === 0 ? !key : !jwt)} sx={{ textTransform: "none", py: 1.2 }}>
            {loading ? "Connecting..." : "Sign In"}
          </Button>

          {tab === 1 && (
            <>
              <Divider sx={{ my: 2 }}>
                <Typography variant="caption" color="text.secondary">or</Typography>
              </Divider>
              <Button variant="outlined" fullWidth sx={{ textTransform: "none" }} disabled>
                Sign in with SSO (Coming Soon)
              </Button>
            </>
          )}
        </form>

        <Typography variant="caption" color="text.secondary" sx={{ display: "block", mt: 2, textAlign: "center" }}>
          API keys are printed to the server console on startup.
        </Typography>
      </Paper>
    </Box>
  );
}
