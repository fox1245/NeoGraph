import React from "react";
import {
  AbsoluteFill,
  Sequence,
  interpolate,
  useCurrentFrame,
} from "remotion";
import { COLORS, SCENES } from "./theme";
import { GridBackground } from "./GridBackground";
import { Intro } from "./scenes/Intro";
import { ReactGraph } from "./scenes/ReactGraph";
import { CodeEditor } from "./scenes/CodeEditor";
import { FeatureOutro } from "./scenes/FeatureOutro";

export const Promo: React.FC = () => {
  const frame = useCurrentFrame();
  // grid is absent during the intro, reveals over the empty beat (S2),
  // then persists behind every following scene.
  const gridOpacity = interpolate(frame, [60, 95], [0, 1], {
    extrapolateLeft: "clamp",
    extrapolateRight: "clamp",
  });

  return (
    <AbsoluteFill style={{ backgroundColor: COLORS.bg }}>
      <GridBackground opacity={gridOpacity} />

      <Sequence
        from={SCENES.intro.from}
        durationInFrames={SCENES.intro.durationInFrames}
      >
        <Intro durationInFrames={SCENES.intro.durationInFrames} />
      </Sequence>

      <Sequence
        from={SCENES.reactGraph.from}
        durationInFrames={SCENES.reactGraph.durationInFrames}
      >
        <ReactGraph durationInFrames={SCENES.reactGraph.durationInFrames} />
      </Sequence>

      <Sequence
        from={SCENES.codeEditor.from}
        durationInFrames={SCENES.codeEditor.durationInFrames}
      >
        <CodeEditor durationInFrames={SCENES.codeEditor.durationInFrames} />
      </Sequence>

      <Sequence from={SCENES.featureGrid.from} durationInFrames={105}>
        <FeatureOutro durationInFrames={105} />
      </Sequence>
    </AbsoluteFill>
  );
};
