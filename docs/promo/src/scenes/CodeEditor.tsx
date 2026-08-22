import React from "react";
import { AbsoluteFill, interpolate, useCurrentFrame } from "remotion";
import { COLORS, FONT_MONO } from "../theme";
import { Kicker, Caption } from "../parts";

type Tok = { t: string; c: string };
const G = "#7E8AA0";
const KW = COLORS.gold;
const FN = "#E0A92B";
const ST = "#E8B45C";
const TX = "#E7ECF5";

const LINES: Tok[][] = [
  [{ t: "// Topology is code. Authority stays in the host.", c: G }],
  [{ t: "export function ", c: KW }, { t: "define", c: FN }, { t: "() {", c: TX }],
  [{ t: "  const graph = ng.", c: TX }, { t: "graph", c: FN }, { t: '("review");', c: ST }],
  [{ t: "  graph.", c: TX }, { t: "node", c: FN }, { t: '("writer", {type: "agent"});', c: ST }],
  [{ t: "  graph.", c: TX }, { t: "node", c: FN }, { t: '("verifier", {type: "agent"});', c: ST }],
  [{ t: "  graph.", c: TX }, { t: "entry", c: FN }, { t: '("writer"); graph.', c: ST }, { t: "edge", c: FN }, { t: '("writer", "verifier");', c: ST }],
  [{ t: "  graph.", c: TX }, { t: "exit", c: FN }, { t: '("verifier"); return graph;', c: ST }],
  [{ t: "}", c: TX }],
  [{ t: "", c: TX }],
  [{ t: "export function* ", c: KW }, { t: "main", c: FN }, { t: "(input) {", c: TX }],
  [{ t: "  return yield ng.", c: TX }, { t: "callCore", c: FN }, { t: '("review", input);', c: ST }],
  [{ t: "}", c: TX }],
];

const TOTAL = LINES.map((line) => line.map((segment) => segment.t).join("") + "\n")
  .join("").length;

export const CodeEditor: React.FC<{ durationInFrames: number }> = ({ durationInFrames }) => {
  const frame = useCurrentFrame();
  const sceneOpacity = interpolate(
    frame,
    [0, 10, durationInFrames - 12, durationInFrames],
    [0, 1, 1, 0],
    { extrapolateLeft: "clamp", extrapolateRight: "clamp" }
  );
  const winIn = interpolate(frame, [4, 20], [0, 1], {
    extrapolateLeft: "clamp",
    extrapolateRight: "clamp",
  });
  const shown = Math.floor(
    interpolate(frame, [18, durationInFrames - 24], [0, TOTAL], {
      extrapolateLeft: "clamp",
      extrapolateRight: "clamp",
    })
  );

  let budget = shown;
  const rendered = LINES.map((line) => {
    const output: React.ReactNode[] = [];
    line.forEach((segment, index) => {
      if (budget <= 0) return;
      const take = Math.min(segment.t.length, budget);
      output.push(<span key={index} style={{ color: segment.c }}>{segment.t.slice(0, take)}</span>);
      budget -= take;
    });
    if (budget > 0) budget -= 1;
    return output;
  });

  return (
    <AbsoluteFill style={{ opacity: sceneOpacity }}>
      <Kicker centered>QUICKJS PROGRAM · RUNTIME TOPOLOGY</Kicker>
      <div style={{
        position: "absolute", left: 300, right: 300, top: 170, opacity: winIn,
        transform: `translateY(${(1 - winIn) * 24}px)`, backgroundColor: COLORS.navy,
        borderRadius: 16, boxShadow: "0 24px 60px rgba(6,28,62,0.28)", overflow: "hidden",
      }}>
        <div style={{
          height: 56, display: "flex", alignItems: "center", padding: "0 22px",
          gap: 12, backgroundColor: "#0A2347",
        }}>
          {["#FF5F56", "#FFBD2E", "#27C93F"].map((color) => (
            <div key={color} style={{ width: 16, height: 16, borderRadius: 8, backgroundColor: color }} />
          ))}
          <span style={{ marginLeft: 18, fontFamily: FONT_MONO, fontSize: 22, color: "#9FB0CC" }}>
            review-topology.js
          </span>
        </div>
        <div style={{
          padding: "28px 38px", fontFamily: FONT_MONO, fontSize: 25,
          lineHeight: "38px", minHeight: 600, whiteSpace: "pre",
        }}>
          {rendered.map((line, index) => <div key={index}>{line.length ? line : " "}</div>)}
        </div>
      </div>
      <Caption bottom={90}>PROPOSAL → COMPILE → SEMANTIC VALIDATE → ADMIT → RUN</Caption>
    </AbsoluteFill>
  );
};
