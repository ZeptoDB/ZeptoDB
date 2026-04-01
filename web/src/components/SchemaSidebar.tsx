"use client";
import { useState } from "react";
import {
  Box, Typography, List, ListItemButton, ListItemText, Collapse, IconButton, Tooltip,
} from "@mui/material";
import ExpandMoreIcon from "@mui/icons-material/ExpandMore";
import ChevronRightIcon from "@mui/icons-material/ChevronRight";
import TableChartIcon from "@mui/icons-material/TableChart";
import RefreshIcon from "@mui/icons-material/Refresh";

const MONO = { fontFamily: "'JetBrains Mono', monospace", fontSize: 12 };

interface Props {
  schema: Record<string, string[]>;
  onRefresh: () => void;
  onInsert: (text: string) => void;
}

export default function SchemaSidebar({ schema, onRefresh, onInsert }: Props) {
  const [expanded, setExpanded] = useState<Record<string, boolean>>({});
  const tables = Object.keys(schema).sort();

  const toggle = (t: string) => setExpanded((prev) => ({ ...prev, [t]: !prev[t] }));

  return (
    <Box sx={{ width: 200, minWidth: 200, borderRight: "1px solid", borderColor: "divider", display: "flex", flexDirection: "column", overflow: "hidden" }}>
      <Box sx={{ display: "flex", alignItems: "center", justifyContent: "space-between", px: 1.5, py: 1, borderBottom: "1px solid", borderColor: "divider" }}>
        <Typography variant="caption" color="text.secondary" fontWeight={600}>SCHEMA</Typography>
        <Tooltip title="Refresh schema">
          <IconButton size="small" onClick={onRefresh}><RefreshIcon sx={{ fontSize: 16 }} /></IconButton>
        </Tooltip>
      </Box>
      <List dense disablePadding sx={{ flex: 1, overflow: "auto" }}>
        {tables.length === 0 && (
          <Typography variant="caption" color="text.secondary" sx={{ px: 1.5, py: 1 }}>No tables</Typography>
        )}
        {tables.map((t) => (
          <Box key={t}>
            <ListItemButton sx={{ py: 0.25, gap: 0.5 }} onClick={() => toggle(t)}>
              {expanded[t] ? <ExpandMoreIcon sx={{ fontSize: 16 }} /> : <ChevronRightIcon sx={{ fontSize: 16 }} />}
              <TableChartIcon sx={{ fontSize: 14, mr: 0.5, color: "primary.main" }} />
              <ListItemText
                primary={t}
                primaryTypographyProps={{ ...MONO, noWrap: true }}
                onClick={(e) => { e.stopPropagation(); onInsert(t); }}
                sx={{ cursor: "pointer" }}
              />
            </ListItemButton>
            <Collapse in={expanded[t]}>
              <List dense disablePadding>
                {(schema[t] ?? []).map((col) => (
                  <ListItemButton key={col} sx={{ pl: 5, py: 0 }} onClick={() => onInsert(col)}>
                    <ListItemText primary={col} primaryTypographyProps={{ ...MONO, color: "text.secondary" }} />
                  </ListItemButton>
                ))}
              </List>
            </Collapse>
          </Box>
        ))}
      </List>
    </Box>
  );
}
