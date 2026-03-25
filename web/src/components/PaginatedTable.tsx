"use client";
import { useState, useEffect } from "react";
import {
  Table, TableHead, TableRow, TableCell, TableBody, TableContainer,
  Box, IconButton, Typography, Select, MenuItem, type SelectChangeEvent,
} from "@mui/material";
import FirstPageIcon from "@mui/icons-material/FirstPage";
import LastPageIcon from "@mui/icons-material/LastPage";
import ChevronLeftIcon from "@mui/icons-material/ChevronLeft";
import ChevronRightIcon from "@mui/icons-material/ChevronRight";

const MONO = { fontFamily: "'JetBrains Mono', monospace", fontSize: 12 };
const PAGE_OPTIONS = [25, 50, 100, 250];

interface Props {
  columns: string[];
  data: (string | number)[][];
  stickyHeader?: boolean;
  maxHeight?: number | string;
}

export default function PaginatedTable({ columns, data, stickyHeader = true, maxHeight }: Props) {
  const [page, setPage] = useState(0);
  const [rowsPerPage, setRowsPerPage] = useState(50);

  // Reset page when data changes
  useEffect(() => { setPage(0); }, [data]);

  const totalPages = Math.max(1, Math.ceil(data.length / rowsPerPage));
  const safePage = Math.min(page, totalPages - 1);
  const start = safePage * rowsPerPage;
  const pageData = data.slice(start, start + rowsPerPage);
  const showPagination = data.length > PAGE_OPTIONS[0];

  const handleRpp = (e: SelectChangeEvent<number>) => {
    setRowsPerPage(Number(e.target.value));
    setPage(0);
  };

  return (
    <Box sx={{ display: "flex", flexDirection: "column", flex: 1, minHeight: 0, overflow: "hidden" }}>
      <TableContainer sx={{ flex: 1, overflow: "auto", ...(maxHeight != null && { maxHeight }) }}>
        <Table size="small" stickyHeader={stickyHeader}>
          <TableHead>
            <TableRow>
              {columns.map((c) => (
                <TableCell key={c} sx={{ fontWeight: 600, ...MONO, bgcolor: "#0D1220" }}>{c}</TableCell>
              ))}
            </TableRow>
          </TableHead>
          <TableBody>
            {pageData.map((row, i) => (
              <TableRow key={start + i} hover>
                {row.map((cell, j) => (
                  <TableCell key={j} sx={MONO}>{String(cell)}</TableCell>
                ))}
              </TableRow>
            ))}
            {pageData.length === 0 && (
              <TableRow>
                <TableCell colSpan={columns.length} sx={{ textAlign: "center", color: "text.secondary", py: 3 }}>
                  No data
                </TableCell>
              </TableRow>
            )}
          </TableBody>
        </Table>
      </TableContainer>

      {showPagination && (
        <Box sx={{ display: "flex", alignItems: "center", justifyContent: "flex-end", gap: 1, px: 2, py: 0.5, borderTop: "1px solid #1E293B", flexShrink: 0 }}>
          <Typography variant="caption" color="text.secondary">Rows per page:</Typography>
          <Select size="small" value={rowsPerPage} onChange={handleRpp}
            sx={{ fontSize: 12, height: 28, "& .MuiSelect-select": { py: 0.25 } }}>
            {PAGE_OPTIONS.map((n) => <MenuItem key={n} value={n}>{n}</MenuItem>)}
          </Select>
          <Typography variant="caption" color="text.secondary" sx={{ mx: 1 }}>
            {start + 1}–{Math.min(start + rowsPerPage, data.length)} of {data.length}
          </Typography>
          <IconButton size="small" disabled={safePage === 0} onClick={() => setPage(0)}><FirstPageIcon fontSize="small" /></IconButton>
          <IconButton size="small" disabled={safePage === 0} onClick={() => setPage(safePage - 1)}><ChevronLeftIcon fontSize="small" /></IconButton>
          <IconButton size="small" disabled={safePage >= totalPages - 1} onClick={() => setPage(safePage + 1)}><ChevronRightIcon fontSize="small" /></IconButton>
          <IconButton size="small" disabled={safePage >= totalPages - 1} onClick={() => setPage(totalPages - 1)}><LastPageIcon fontSize="small" /></IconButton>
        </Box>
      )}
    </Box>
  );
}
