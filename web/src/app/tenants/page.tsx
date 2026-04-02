"use client";
import { Box, Paper, Typography, Table, TableHead, TableRow, TableCell, TableBody, TableContainer, Chip } from "@mui/material";
import { useQuery } from "@tanstack/react-query";
import { fetchTenants } from "@/lib/api";
import { useAuth } from "@/lib/auth";
import { useRouter } from "next/navigation";

const MONO = { fontFamily: "'JetBrains Mono', monospace", fontSize: 13 };

export default function TenantsPage() {
  const { auth } = useAuth();
  const router = useRouter();
  const { data: tenants } = useQuery({
    queryKey: ["tenants"], queryFn: () => fetchTenants(auth?.apiKey), refetchInterval: 5000,
  });

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 3 }}>
      <Typography variant="h6">Tenants</Typography>
      <Paper sx={{ border: "1px solid rgba(255, 255, 255, 0.08)" }}>
        <TableContainer>
          <Table size="small">
            <TableHead>
              <TableRow>
                <TableCell sx={{ fontWeight: 600 }}>Tenant ID</TableCell>
                <TableCell sx={{ fontWeight: 600 }}>Name</TableCell>
                <TableCell sx={{ fontWeight: 600 }}>Namespace</TableCell>
                <TableCell sx={{ fontWeight: 600 }} align="right">Concurrent Limit</TableCell>
                <TableCell sx={{ fontWeight: 600 }} align="right">Active Queries</TableCell>
                <TableCell sx={{ fontWeight: 600 }} align="right">Total Queries</TableCell>
              </TableRow>
            </TableHead>
            <TableBody>
              {(tenants ?? []).map((t: any) => (
                <TableRow key={t.tenant_id} hover sx={{ cursor: "pointer" }} onClick={() => router.push(`/tenants/${t.tenant_id}`)}>
                  <TableCell sx={MONO}>{t.tenant_id}</TableCell>
                  <TableCell>{t.name}</TableCell>
                  <TableCell sx={{ ...MONO, color: "text.secondary" }}>{t.table_namespace || "—"}</TableCell>
                  <TableCell align="right" sx={MONO}>{t.max_concurrent_queries || "∞"}</TableCell>
                  <TableCell align="right" sx={MONO}>{t.usage?.active_queries ?? "—"}</TableCell>
                  <TableCell align="right" sx={MONO}>{t.usage?.total_queries?.toLocaleString() ?? "—"}</TableCell>
                </TableRow>
              ))}
              {(!tenants || tenants.length === 0) && (
                <TableRow><TableCell colSpan={6} sx={{ textAlign: "center", color: "text.secondary", py: 4 }}>
                  {tenants === null ? "Admin access required" : "No tenants"}
                </TableCell></TableRow>
              )}
            </TableBody>
          </Table>
        </TableContainer>
      </Paper>
    </Box>
  );
}
