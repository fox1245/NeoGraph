import React from "react";
import { AbsoluteFill } from "remotion";
import { COLORS } from "./theme";

// The faint engineering-graph-paper grid that sits behind every scene
// from the grid reveal onward. Two cell scales like the original.
export const GridBackground: React.FC<{ opacity: number }> = ({ opacity }) => {
  const minor = 40;
  const major = 200;
  return (
    <AbsoluteFill
      style={{
        opacity,
        backgroundColor: COLORS.bg,
        backgroundImage: `
          linear-gradient(${COLORS.grid} 1px, transparent 1px),
          linear-gradient(90deg, ${COLORS.grid} 1px, transparent 1px),
          linear-gradient(rgba(6,28,62,0.10) 1px, transparent 1px),
          linear-gradient(90deg, rgba(6,28,62,0.10) 1px, transparent 1px)
        `,
        backgroundSize: `${minor}px ${minor}px, ${minor}px ${minor}px, ${major}px ${major}px, ${major}px ${major}px`,
      }}
    />
  );
};
