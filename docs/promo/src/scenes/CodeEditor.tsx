import React from "react";
import { AbsoluteFill, interpolate, useCurrentFrame } from "remotion";
import { COLORS, FONT_MONO } from "../theme";
import { Kicker, Caption } from "../parts";

// Scene 4 — a macOS-style window where the Python API types itself in.
type Tok = { t: string; c: string };
const G = "#7E8AA0"; // comment
const KW = COLORS.gold; // keyword
const FN = "#E0A92B"; // call
const CL = "#5BC8C8"; // class / type
const ST = "#E8B45C"; // string
const TX = "#E7ECF5"; // plain

const LINES: Tok[][] = [
  [{ t: "# C++ engine, Python decorator API.", c: G }],
  [
    { t: "import", c: KW },
    { t: " neograph_engine ", c: TX },
    { t: "as", c: KW },
    { t: " ng", c: TX },
  ],
  [
    { t: "from", c: KW },
    { t: " neograph_engine.llm ", c: TX },
    { t: "import", c: KW },
    { t: " OpenAIProvider", c: CL },
  ],
  [{ t: "", c: TX }],
  [
    { t: "engine = ng.", c: TX },
    { t: "GraphEngine", c: CL },
    { t: ".", c: TX },
    { t: "compile", c: FN },
    { t: "(graph_def,", c: TX },
  ],
  [
    { t: "    ng.", c: TX },
    { t: "NodeContext", c: CL },
    { t: "(provider=", c: TX },
    { t: "OpenAIProvider", c: CL },
    { t: "()))", c: TX },
  ],
  [{ t: "", c: TX }],
  [
    { t: "result = engine.", c: TX },
    { t: "run", c: FN },
    { t: "(ng.", c: TX },
    { t: "RunConfig", c: CL },
    { t: "(", c: TX },
  ],
  [
    { t: "    thread_id=", c: TX },
    { t: '"s1"', c: ST },
    { t: ",", c: TX },
  ],
  [
    { t: "    input={", c: TX },
    { t: '"messages"', c: ST },
    { t: ": [{", c: TX },
    { t: '"role"', c: ST },
    { t: ':', c: TX },
    { t: '"user"', c: ST },
    { t: ', ', c: TX },
    { t: '"content"', c: ST },
    { t: ':', c: TX },
    { t: '"Hi!"', c: ST },
    { t: "}]}))", c: TX },
  ],
];

const flat = LINES.flatMap((l) => l.map((s) => s.t).join("") + "\n");
const TOTAL = flat.join("").length;

export const CodeEditor: React.FC<{ durationInFrames: number }> = ({
  durationInFrames,
}) => {
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

  // characters revealed by now
  const shown = Math.floor(
    interpolate(frame, [18, durationInFrames - 24], [0, TOTAL], {
      extrapolateLeft: "clamp",
      extrapolateRight: "clamp",
    })
  );

  let budget = shown;
  const rendered = LINES.map((line) => {
    const out: React.ReactNode[] = [];
    line.forEach((seg, i) => {
      if (budget <= 0) return;
      const take = Math.min(seg.t.length, budget);
      out.push(
        <span key={i} style={{ color: seg.c }}>
          {seg.t.slice(0, take)}
        </span>
      );
      budget -= take;
    });
    if (budget > 0) budget -= 1; // newline
    return out;
  });

  return (
    <AbsoluteFill style={{ opacity: sceneOpacity }}>
      <Kicker centered>PYTHON · PIP INSTALL NEOGRAPH-ENGINE</Kicker>
      <div
        style={{
          position: "absolute",
          left: 360,
          right: 360,
          top: 220,
          opacity: winIn,
          transform: `translateY(${(1 - winIn) * 24}px)`,
          backgroundColor: COLORS.navy,
          borderRadius: 16,
          boxShadow: "0 24px 60px rgba(6,28,62,0.28)",
          overflow: "hidden",
        }}
      >
        <div
          style={{
            height: 56,
            display: "flex",
            alignItems: "center",
            padding: "0 22px",
            gap: 12,
            backgroundColor: "#0A2347",
          }}
        >
          {["#FF5F56", "#FFBD2E", "#27C93F"].map((c) => (
            <div
              key={c}
              style={{
                width: 16,
                height: 16,
                borderRadius: 8,
                backgroundColor: c,
              }}
            />
          ))}
          <span
            style={{
              marginLeft: 18,
              fontFamily: FONT_MONO,
              fontSize: 22,
              color: "#9FB0CC",
            }}
          >
            react_agent.py
          </span>
        </div>
        <div
          style={{
            padding: "34px 44px",
            fontFamily: FONT_MONO,
            fontSize: 28,
            lineHeight: "44px",
            minHeight: 470,
            whiteSpace: "pre",
          }}
        >
          {rendered.map((l, i) => (
            <div key={i}>{l.length ? l : " "}</div>
          ))}
        </div>
      </div>
      <Caption bottom={120}>
        SAME API IN C++ · NATIVE 5 MS OVERHEAD UNDER THE BINDING
      </Caption>
    </AbsoluteFill>
  );
};
