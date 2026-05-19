import React from "react";
import { Composition } from "remotion";
import { Promo } from "./Promo";
import { VIDEO } from "./theme";

export const RemotionRoot: React.FC = () => (
  <Composition
    id="PromoV2"
    component={Promo}
    durationInFrames={VIDEO.durationInFrames}
    fps={VIDEO.fps}
    width={VIDEO.width}
    height={VIDEO.height}
  />
);
