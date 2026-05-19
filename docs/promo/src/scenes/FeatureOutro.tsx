import React from "react";
import {
  AbsoluteFill,
  interpolate,
  useCurrentFrame,
  Easing,
} from "remotion";
import { COLORS, FONT_MONO } from "../theme";
import { Kicker } from "../parts";

// Scenes 5 + 6 merged so the chip grid → outro panel move is continuous
// (matches the original, where the chips slide up as the panel rises).
const CHIPS = [
  "FAN-OUT",
  "CHECKPOINTS",
  "HITL",
  "TIME-TRAVEL",
  "STREAMING",
  "SUBGRAPHS",
  "MCP",
  "A2A",
  "MULTI-LLM",
];

const COLW = 420;
const ROWH = 96;
const GAP = 36;
const GRIDW = COLW * 3 + GAP * 2;
const GRIDX = (1920 - GRIDW) / 2;

export const FeatureOutro: React.FC<{ durationInFrames: number }> = ({
  durationInFrames,
}) => {
  const frame = useCurrentFrame();
  const sceneOpacity = interpolate(frame, [0, 10], [0, 1], {
    extrapolateLeft: "clamp",
    extrapolateRight: "clamp",
  });

  // phase B (outro) progress
  const out = interpolate(frame, [58, 82], [0, 1], {
    extrapolateLeft: "clamp",
    extrapolateRight: "clamp",
    easing: Easing.inOut(Easing.cubic),
  });

  const gridY = 330 - out * 150;
  const gridScale = 1 - out * 0.16;
  const kickerOp = interpolate(frame, [0, 10, 50, 62], [0, 1, 1, 0], {
    extrapolateLeft: "clamp",
    extrapolateRight: "clamp",
  });

  const panelH = 430;
  const panelY = 1080 - panelH * out;

  return (
    <AbsoluteFill style={{ opacity: sceneOpacity }}>
      <div style={{ opacity: kickerOp }}>
        <Kicker centered top={120}>
          ALL BUILT IN · NO ADAPTERS · NO PLUGIN BOILERPLATE
        </Kicker>
      </div>

      <div
        style={{
          position: "absolute",
          left: GRIDX,
          top: gridY,
          width: GRIDW,
          transform: `scale(${gridScale})`,
          transformOrigin: "center top",
          display: "grid",
          gridTemplateColumns: `repeat(3, ${COLW}px)`,
          gap: GAP,
        }}
      >
        {CHIPS.map((label, i) => {
          const pop = interpolate(
            frame,
            [10 + i * 4, 24 + i * 4],
            [0, 1],
            { extrapolateLeft: "clamp", extrapolateRight: "clamp" }
          );
          return (
            <div
              key={label}
              style={{
                height: ROWH,
                opacity: pop,
                transform: `scale(${0.9 + 0.1 * pop})`,
                backgroundColor: COLORS.white,
                border: `3px solid ${COLORS.navy}`,
                borderRadius: 10,
                boxShadow: "0 8px 0 rgba(6,28,62,0.16)",
                display: "flex",
                alignItems: "center",
                overflow: "hidden",
              }}
            >
              <div
                style={{
                  width: 12,
                  height: "100%",
                  backgroundColor: COLORS.gold,
                }}
              />
              <span
                style={{
                  paddingLeft: 28,
                  fontFamily: FONT_MONO,
                  fontSize: 32,
                  fontWeight: 800,
                  letterSpacing: 2,
                  color: COLORS.navy,
                }}
              >
                {label}
              </span>
            </div>
          );
        })}
      </div>

      <div
        style={{
          position: "absolute",
          left: 0,
          right: 0,
          top: panelY,
          height: panelH,
          backgroundColor: COLORS.navy,
          display: "flex",
          flexDirection: "column",
          justifyContent: "center",
          paddingLeft: 160,
        }}
      >
        <div
          style={{
            fontFamily: FONT_MONO,
            fontSize: 56,
            fontWeight: 800,
            color: COLORS.gold,
          }}
        >
          pip install neograph-engine
        </div>
        <div
          style={{
            marginTop: 18,
            fontFamily: FONT_MONO,
            fontSize: 44,
            fontWeight: 700,
            color: COLORS.white,
          }}
        >
          github.com/fox1245/NeoGraph
        </div>
        <div
          style={{
            marginTop: 26,
            fontFamily: FONT_MONO,
            fontSize: 22,
            letterSpacing: 4,
            color: "#7E8AA0",
          }}
        >
          MIT · PYTHON 3.9 → 3.13 · LINUX / MACOS / WINDOWS · 397 TESTS GREEN
        </div>
      </div>
    </AbsoluteFill>
  );
};
