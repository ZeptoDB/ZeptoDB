"use client";
import { useEffect, useState } from "react";
import { Box, Paper, Typography, Table, TableHead, TableRow, TableCell, TableBody, TableContainer, Chip } from "@mui/material";
import { querySQL } from "@/lib/api";
import { useAuth } from "@/lib/auth";

interface TableInfo { name: string; rows: number }
interface SchemaCol { column: string; type: string }

export default function TablesPage() {
  const { auth } = useAuth();
  const [tables, setTables] = useState<TableInfo[]>([]);
  const [selected, setSelected] = useState<string | null>(null);
  const [schema, setSchema] = useState<SchemaCol[]>([]);
  const [preview, setPreview] = useState<{ columns: string[]; data: (string | number)[][] } | null>(null);

  useEffect(() => {
    querySQL("SHOW TABLES", auth?.apiKey)
      .then((r) => {
        const list: TableInfo[] = (r.data ?? []).map((row: (string | number)[]) => ({
          name: String(row[0]),
          rows: Number(row[1] ?? 0),
        }));
        setTables(list);
      })
      .catch(() => setTables([]));
  }, [auth]);

  useEffect(() => {
    if (!selected) { setSchema([]); setPreview(null); return; }
    querySQL(`DESCRIBE ${selected}`, auth?.apiKey)
      .then((r) => {
        setSchema((r.data ?? []).map((row: (string | number)[]) => ({
          column: String(row[0]),
          type: String(row[1] ?? "unknown"),
        })));
      })
      .catch(() => setSchema([]));
    querySQL(`SELECT * FROM ${selected} LIMIT 20`, auth?.apiKey)
      .then(setPreview)
      .catch(() => setPreview(null));
  }, [selected, auth]);

  return (
    <Box sx={{ display: "flex", flexDirection: "column", gap: 2 }}>
      <Typography variant="h6">Tables</Typography>
      <Paper sx={{ border: "1px solid #1E293B" }}>
        <TableContainer>
          <Table size="small">
            <TableHead>
              <TableRow>
                <TableCell sx={{ fontWeight: 600 }}>Table</TableCell>
                <TableCell sx={{ fontWeight: 600 }}>Rows</TableCell>
              </TableRow>
            </TableHead>
            <TableBody>
              {tables.map((t) => (
                <TableRow key={t.name} hover selected={selected === t.name} onClick={() => setSelected(t.name)} sx={{ cursor: "pointer" }}>
                  <TableCell sx={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 13 }}>{t.name}</TableCell>
                  <TableCell>{t.rows.toLocaleString()}</TableCell>
                </TableRow>
              ))}
              {tables.length === 0 && (
                <TableRow><TableCell colSpan={2} sx={{ textAlign: "center", color: "text.secondary" }}>No tables found</TableCell></TableRow>
              )}
            </TableBody>
          </Table>
        </TableContainer>
      </Paper>

      {schema.length > 0 && (
        <Paper sx={{ border: "1px solid #1E293B" }}>
          <Box sx={{ px: 2, py: 1, borderBottom: "1px solid #1E293B", display: "flex", gap: 1, alignItems: "center" }}>
            <Chip label={selected} size="small" color="primary" variant="outlined" />
            <Typography variant="caption" color="text.secondary">Schema</Typography>
          </Box>
          <TableContainer>
            <Table size="small">
              <TableHead>
                <TableRow>
                  <TableCell sx={{ fontWeight: 600 }}>Column</TableCell>
                  <TableCell sx={{ fontWeight: 600 }}>Type</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                {schema.map((c) => (
                  <TableRow key={c.column}>
                    <TableCell sx={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 13 }}>{c.column}</TableCell>
                    <TableCell sx={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 13 }}>{c.type}</TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </TableContainer>
        </Paper>
      )}

      {preview && (
        <Paper sx={{ border: "1px solid #1E293B" }}>
          <Box sx={{ px: 2, py: 1, borderBottom: "1px solid #1E293B" }}>
            <Typography variant="caption" color="text.secondary">Preview (LIMIT 20)</Typography>
          </Box>
          <TableContainer>
            <Table size="small">
              <TableHead>
                <TableRow>
                  {preview.columns.map((c) => <TableCell key={c} sx={{ fontWeight: 600 }}>{c}</TableCell>)}
                </TableRow>
              </TableHead>
              <TableBody>
                {preview.data.map((row, i) => (
                  <TableRow key={i}>
                    {row.map((cell, j) => <TableCell key={j} sx={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 13 }}>{String(cell)}</TableCell>)}
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </TableContainer>
        </Paper>
      )}
    </Box>
  );
}
