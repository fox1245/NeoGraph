import React from "react";
import { COLORS, FONT_SANS } from "./theme";

// Small gold top-left section label, e.g. "ENGINE IN MOTION".
export const Kicker: React.FC<{
  children: React.ReactNode;
  centered?: boolean;
  color?: string;
  top?: number;
}> = ({ children, centered, color = COLORS.gold, top = 90 }) => (
  <div
    style={{
      position: "absolute",
      top,
      left: 0,
      right: 0,
      textAlign: centered ? "center" : "left",
      paddingLeft: centered ? 0 : 160,
      fontFamily: FONT_SANS,
      fontSize: 30,
      fontWeight: 700,
      letterSpacing: 12,
      color,
    }}
  >
    {children}
  </div>
);

// Centered navy strong subheader, e.g. "REACT LOOP · SEQUENTIAL · ~5US / NODE".
export const SubHeader: React.FC<{ children: React.ReactNode; top: number }> = ({
  children,
  top,
}) => (
  <div
    style={{
      position: "absolute",
      top,
      left: 0,
      right: 0,
      textAlign: "center",
      fontFamily: FONT_SANS,
      fontSize: 34,
      fontWeight: 800,
      letterSpacing: 8,
      color: COLORS.navy,
    }}
  >
    {children}
  </div>
);

// Muted grey small-caps caption (scene footers).
export const Caption: React.FC<{ children: React.ReactNode; bottom: number }> = ({
  children,
  bottom,
}) => (
  <div
    style={{
      position: "absolute",
      bottom,
      left: 0,
      right: 0,
      textAlign: "center",
      fontFamily: FONT_SANS,
      fontSize: 22,
      fontWeight: 600,
      letterSpacing: 7,
      color: COLORS.caption,
    }}
  >
    {children}
  </div>
);
