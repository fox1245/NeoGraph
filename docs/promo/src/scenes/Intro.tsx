import React from "react";
import { AbsoluteFill, interpolate, useCurrentFrame, Easing } from "remotion";
import { COLORS, FONT_SANS } from "../theme";

// Scene 1 — plain light bg (no grid yet). A gold rule draws left→right,
// the wordmark fades up beneath it.
export const Intro: React.FC<{ durationInFrames: number }> = ({
  durationInFrames,
}) => {
  const frame = useCurrentFrame();

  const ruleW = interpolate(frame, [4, 34], [0, 1180], {
    extrapolateLeft: "clamp",
    extrapolateRight: "clamp",
    easing: Easing.out(Easing.cubic),
  });
  const textOpacity = interpolate(frame, [16, 34], [0, 1], {
    extrapolateLeft: "clamp",
    extrapolateRight: "clamp",
  });
  const out = interpolate(
    frame,
    [durationInFrames - 12, durationInFrames],
    [1, 0],
    { extrapolateLeft: "clamp", extrapolateRight: "clamp" }
  );

  return (
    <AbsoluteFill style={{ backgroundColor: COLORS.bg, opacity: out }}>
      <div style={{ position: "absolute", left: 160, top: 470 }}>
        <div
          style={{
            width: ruleW,
            height: 8,
            backgroundColor: COLORS.gold,
            borderRadius: 2,
          }}
        />
        <div
          style={{
            marginTop: 28,
            opacity: textOpacity,
            fontFamily: FONT_SANS,
            fontSize: 38,
            fontWeight: 700,
            letterSpacing: 18,
            color: COLORS.navy,
          }}
        >
          NEOGRAPH&nbsp;&nbsp;·&nbsp;&nbsp;V0.2.3
        </div>
      </div>
    </AbsoluteFill>
  );
};
