"use client";
import { useCallback, useRef } from "react";
import { Box } from "@mui/material";

interface Props {
  onResize: (deltaY: number) => void;
}

export default function ResizeDivider({ onResize }: Props) {
  const lastY = useRef(0);

  const onMouseDown = useCallback(
    (e: React.MouseEvent) => {
      e.preventDefault();
      lastY.current = e.clientY;

      const onMove = (ev: MouseEvent) => {
        const dy = ev.clientY - lastY.current;
        lastY.current = ev.clientY;
        onResize(dy);
      };
      const onUp = () => {
        document.removeEventListener("mousemove", onMove);
        document.removeEventListener("mouseup", onUp);
      };
      document.addEventListener("mousemove", onMove);
      document.addEventListener("mouseup", onUp);
    },
    [onResize],
  );

  return (
    <Box
      onMouseDown={onMouseDown}
      sx={{
        height: 6,
        cursor: "row-resize",
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
        "&:hover, &:active": { bgcolor: "rgba(255,255,255,0.08)" },
      }}
    >
      <Box sx={{ width: 32, height: 2, borderRadius: 1, bgcolor: "divider" }} />
    </Box>
  );
}
