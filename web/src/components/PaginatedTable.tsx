"use client";
import { useState, useEffect, useMemo } from "react";
import {
  Table, TableHead, TableRow, TableCell, TableBody, TableContainer,
  Box, IconButton, Typography, Select, MenuItem, type SelectChangeEvent,
} from "@mui/material";
import FirstPageIcon from "@mui/icons-material/FirstPage";
import LastPageIcon from "@mui/icons-material/LastPage";
import ChevronLeftIcon from "@mui/icons-material/ChevronLeft";
import ChevronRightIcon from "@mui/icons-material/ChevronRight";
import ArrowUpwardIcon from "@mui/icons-material/ArrowUpward";
import ArrowDownwardIcon from "@mui/icons-material/ArrowDownward";
import FilterListIcon from "@mui/icons-material/FilterList";

const MONO = { fontFamily: "'JetBrains Mono', monospace", fontSize: 12 };
const PAGE_OPTIONS = [25, 50, 100, 250];

interface Props {
  columns: string[];
  data: (string | number)[][];
  stickyHeader?: boolean;
  maxHeight?: number | string;
}

type SortDir = "asc" | "desc" | null;

export default function PaginatedTable({ columns, data, stickyHeader = true, maxHeight }: Props) {
  const [page, setPage] = useState(0);
  const [rowsPerPage, setRowsPerPage] = useState(50);
  const [sortCol, setSortCol] = useState<number | null>(null);
  const [sortDir, setSortDir] = useState<SortDir>(null);
  const [showFilters, setShowFilters] = useState(false);
  const [filters, setFilters] = useState<Record<number, string>>({});

  // Reset page, sort & filters when data changes
  useEffect(() => { setPage(0); setSortCol(null); setSortDir(null); setFilters({}); }, [data]);

  const handleSort = (colIdx: number) => {
    if (sortCol === colIdx) {
      setSortDir((d) => (d === "asc" ? "desc" : d === "desc" ? null : "asc"));
      if (sortDir === "desc") setSortCol(null);
    } else {
      setSortCol(colIdx);
      setSortDir("asc");
    }
  };

  const filteredData = useMemo(() => {
    const activeFilters = Object.entries(filters).filter(([, v]) => v.trim());
    if (activeFilters.length === 0) return data;
    return data.filter((row) =>
      activeFilters.every(([ci, term]) => String(row[Number(ci)]).toLowerCase().includes(term.toLowerCase()))
    );
  }, [data, filters]);

  const sortedData = useMemo(() => {
    if (sortCol === null || sortDir === null) return filteredData;
    const ci = sortCol;
    const dir = sortDir === "asc" ? 1 : -1;
    return [...filteredData].sort((a, b) => {
      const av = a[ci], bv = b[ci];
      if (typeof av === "number" && typeof bv === "number") return (av - bv) * dir;
      return String(av).localeCompare(String(bv)) * dir;
    });
  }, [filteredData, sortCol, sortDir]);

  const totalPages = Math.max(1, Math.ceil(sortedData.length / rowsPerPage));
  const safePage = Math.min(page, totalPages - 1);
  const start = safePage * rowsPerPage;
  const pageData = sortedData.slice(start, start + rowsPerPage);
  const showPagination = sortedData.length > PAGE_OPTIONS[0];
  const hasActiveFilters = Object.values(filters).some((v) => v.trim());

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
              {columns.map((c, i) => (
                <TableCell
                  key={c}
                  sx={{ fontWeight: 600, ...MONO, bgcolor: "background.paper", cursor: "pointer", userSelect: "none", "&:hover": { color: "primary.main" } }}
                  onClick={() => handleSort(i)}
                >
                  <Box sx={{ display: "flex", alignItems: "center", gap: 0.5 }}>
                    {c}
                    {sortCol === i && sortDir === "asc" && <ArrowUpwardIcon sx={{ fontSize: 14 }} />}
                    {sortCol === i && sortDir === "desc" && <ArrowDownwardIcon sx={{ fontSize: 14 }} />}
                  </Box>
                </TableCell>
              ))}
            </TableRow>
            {showFilters && (
              <TableRow>
                {columns.map((_, i) => (
                  <TableCell key={i} sx={{ py: 0.5, px: 1, bgcolor: "background.paper" }}>
                    <input
                      placeholder="Filter…"
                      value={filters[i] ?? ""}
                      onChange={(e) => { setFilters((prev) => ({ ...prev, [i]: e.target.value })); setPage(0); }}
                      onClick={(e) => e.stopPropagation()}
                      style={{ background: "transparent", border: "1px solid rgba(128,128,128,0.3)", borderRadius: 4, color: "inherit", fontSize: 11, width: "100%", padding: "2px 6px", outline: "none", fontFamily: "'JetBrains Mono', monospace" }}
                    />
                  </TableCell>
                ))}
              </TableRow>
            )}
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

      <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between", px: 2, py: 0.5, borderTop: "1px solid", borderColor: "divider", flexShrink: 0 }}>
        <IconButton size="small" onClick={() => setShowFilters((v) => !v)} sx={{ color: hasActiveFilters ? "primary.main" : "text.secondary" }}>
          <FilterListIcon fontSize="small" />
        </IconButton>
        {filteredData.length < data.length && (
          <Typography variant="caption" color="text.secondary">{filteredData.length} of {data.length} rows matched</Typography>
        )}
        {showPagination && (
          <Box sx={{ display: "flex", alignItems: "center", gap: 1 }}>
            <Typography variant="caption" color="text.secondary">Rows per page:</Typography>
            <Select size="small" value={rowsPerPage} onChange={handleRpp}
              sx={{ fontSize: 12, height: 28, "& .MuiSelect-select": { py: 0.25 } }}>
              {PAGE_OPTIONS.map((n) => <MenuItem key={n} value={n}>{n}</MenuItem>)}
            </Select>
            <Typography variant="caption" color="text.secondary" sx={{ mx: 1 }}>
              {start + 1}–{Math.min(start + rowsPerPage, sortedData.length)} of {sortedData.length}
            </Typography>
            <IconButton size="small" disabled={safePage === 0} onClick={() => setPage(0)}><FirstPageIcon fontSize="small" /></IconButton>
            <IconButton size="small" disabled={safePage === 0} onClick={() => setPage(safePage - 1)}><ChevronLeftIcon fontSize="small" /></IconButton>
            <IconButton size="small" disabled={safePage >= totalPages - 1} onClick={() => setPage(safePage + 1)}><ChevronRightIcon fontSize="small" /></IconButton>
            <IconButton size="small" disabled={safePage >= totalPages - 1} onClick={() => setPage(totalPages - 1)}><LastPageIcon fontSize="small" /></IconButton>
          </Box>
        )}
      </Box>
    </Box>
  );
}
