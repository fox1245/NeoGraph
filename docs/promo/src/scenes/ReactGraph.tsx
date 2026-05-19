import React from "react";
import {
  AbsoluteFill,
  interpolate,
  useCurrentFrame,
  Easing,
} from "remotion";
import { COLORS, FONT_MONO } from "../theme";
import { Kicker, SubHeader } from "../parts";

// Scene 3 — the ReAct pipeline. Rebuilt so every connector is anchored
// to the actual box edges (right-mid → left-mid), the row is evenly
// spaced and centered with no overlap, and the loop-back arc starts
// and ends exactly on the box top edges.

const CY = 565; // vertical centre of the node band
const BH = 110; // box height

type NodeDef = { id: string; label: string; x: number; w: number };

// Explicit widths sized to the mono labels; even 150px gaps; centred.
const NODES: NodeDef[] = [
  { id: "start", label: "START", x: 175, w: 220 },
  { id: "llm", label: "llm_call", x: 545, w: 300 },
  { id: "tool", label: "tool_dispatch", x: 995, w: 400 },
  { id: "end", label: "END", x: 1545, w: 200 },
];

const right = (n: NodeDef) => n.x + n.w;
const midY = CY;

const EDGES = [
  { from: NODES[0], to: NODES[1], drawFrom: 16, drawTo: 30, lights: "llm" },
  { from: NODES[1], to: NODES[2], drawFrom: 32, drawTo: 46, lights: "tool" },
  { from: NODES[2], to: NODES[3], drawFrom: 48, drawTo: 62, lights: "end" },
] as const;

const ARC_DRAW = [64, 88] as const;

const Box: React.FC<{ n: NodeDef; activeAt: number | null }> = ({
  n,
  activeAt,
}) => {
  const frame = useCurrentFrame();
  const appear = interpolate(
    frame,
    [NODES.indexOf(n) * 3, NODES.indexOf(n) * 3 + 14],
    [0, 1],
    { extrapolateLeft: "clamp", extrapolateRight: "clamp" }
  );
  const isEnd = n.id === "end";
  // gold-fill transition when the token reaches the node
  const lit =
    activeAt === null
      ? 0
      : interpolate(frame, [activeAt, activeAt + 8], [0, 1], {
          extrapolateLeft: "clamp",
          extrapolateRight: "clamp",
        });

  const bg = isEnd
    ? COLORS.navy
    : lit > 0
    ? `rgba(217,154,10,${lit})`
    : COLORS.white;
  const fg = isEnd ? COLORS.white : COLORS.navy;

  return (
    <div
      style={{
        position: "absolute",
        left: n.x,
        top: CY - BH / 2,
        width: n.w,
        height: BH,
        transform: `scale(${0.92 + 0.08 * appear})`,
        opacity: appear,
        backgroundColor: bg,
        border: `3px solid ${COLORS.navy}`,
        borderRadius: 14,
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
        fontFamily: FONT_MONO,
        fontSize: 36,
        fontWeight: 700,
        color: fg,
        boxShadow: "0 6px 0 rgba(6,28,62,0.18)",
      }}
    >
      {n.label}
    </div>
  );
};

export const ReactGraph: React.FC<{ durationInFrames: number }> = ({
  durationInFrames,
}) => {
  const frame = useCurrentFrame();

  const sceneOpacity = interpolate(
    frame,
    [0, 10, durationInFrames - 12, durationInFrames],
    [0, 1, 1, 0],
    { extrapolateLeft: "clamp", extrapolateRight: "clamp" }
  );

  // when each node lights up (START lit from the very start)
  const litAt: Record<string, number | null> = {
    start: 4,
    llm: EDGES[0].drawTo,
    tool: EDGES[1].drawTo,
    end: null,
  };

  // current token position along the forward edges, then the loop arc
  const tokenPos = (): { x: number; y: number; show: boolean } => {
    for (const e of EDGES) {
      if (frame >= e.drawFrom && frame <= e.drawTo + 2) {
        const t = interpolate(frame, [e.drawFrom, e.drawTo], [0, 1], {
          extrapolateLeft: "clamp",
          extrapolateRight: "clamp",
        });
        return { x: right(e.from) + (e.to.x - right(e.from)) * t, y: midY, show: true };
      }
    }
    if (frame >= ARC_DRAW[0] && frame <= ARC_DRAW[1] + 2) {
      const t = interpolate(frame, [ARC_DRAW[0], ARC_DRAW[1]], [0, 1], {
        extrapolateLeft: "clamp",
        extrapolateRight: "clamp",
      });
      // mirror of the arc path below (cubic from tool-top to llm-top)
      const x0 = NODES[2].x + NODES[2].w / 2;
      const x1 = NODES[1].x + NODES[1].w / 2;
      const yTop = CY - BH / 2;
      const cYy = CY - BH / 2 - 150;
      const b = (a: number, bb: number, c: number, d: number, tt: number) => {
        const u = 1 - tt;
        return (
          u * u * u * a + 3 * u * u * tt * bb + 3 * u * tt * tt * c + tt * tt * tt * d
        );
      };
      return {
        x: b(x0, x0, x1, x1, t),
        y: b(yTop, cYy, cYy, yTop, t),
        show: true,
      };
    }
    return { x: 0, y: 0, show: false };
  };

  const tk = tokenPos();

  const dash = (e: (typeof EDGES)[number]) => {
    const len = e.to.x - right(e.from);
    const p = interpolate(frame, [e.drawFrom, e.drawTo], [0, 1], {
      extrapolateLeft: "clamp",
      extrapolateRight: "clamp",
      easing: Easing.inOut(Easing.cubic),
    });
    return { len, off: len * (1 - p) };
  };

  // loop-back arc anchored to box top-centres
  const arcX0 = NODES[2].x + NODES[2].w / 2;
  const arcX1 = NODES[1].x + NODES[1].w / 2;
  const arcYTop = CY - BH / 2;
  const arcCtrlY = CY - BH / 2 - 150;
  const arcPath = `M ${arcX0} ${arcYTop} C ${arcX0} ${arcCtrlY} ${arcX1} ${arcCtrlY} ${arcX1} ${arcYTop}`;
  const arcLen = 1100;
  const arcP = interpolate(frame, [ARC_DRAW[0], ARC_DRAW[1]], [0, 1], {
    extrapolateLeft: "clamp",
    extrapolateRight: "clamp",
    easing: Easing.inOut(Easing.cubic),
  });

  return (
    <AbsoluteFill style={{ opacity: sceneOpacity }}>
      <Kicker>ENGINE IN MOTION</Kicker>
      <SubHeader top={210}>REACT LOOP · SEQUENTIAL · ~5US / NODE</SubHeader>

      <svg
        width={1920}
        height={1080}
        style={{ position: "absolute", inset: 0 }}
      >
        {EDGES.map((e, i) => {
          const d = dash(e);
          return (
            <line
              key={i}
              x1={right(e.from)}
              y1={midY}
              x2={e.to.x}
              y2={midY}
              stroke={COLORS.navy}
              strokeWidth={5}
              strokeLinecap="round"
              strokeDasharray={d.len}
              strokeDashoffset={d.off}
            />
          );
        })}

        {/* ReAct loop-back: tool_dispatch top → llm_call top */}
        <path
          d={arcPath}
          fill="none"
          stroke={COLORS.gold}
          strokeWidth={5}
          strokeLinecap="round"
          strokeDasharray={arcLen}
          strokeDashoffset={arcLen * (1 - arcP)}
        />
        {arcP > 0.98 && (
          <polygon
            points={`${arcX1},${arcYTop} ${arcX1 - 13},${arcYTop - 22} ${
              arcX1 + 13
            },${arcYTop - 22}`}
            fill={COLORS.gold}
          />
        )}

        {tk.show && (
          <circle cx={tk.x} cy={tk.y} r={11} fill={COLORS.gold} stroke={COLORS.navy} strokeWidth={3} />
        )}
      </svg>

      {NODES.map((n) => (
        <Box key={n.id} n={n} activeAt={litAt[n.id]} />
      ))}
    </AbsoluteFill>
  );
};
