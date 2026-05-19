// Palette sampled directly from the original neograph-promo-v2.mp4 frames.
export const COLORS = {
  bg: "#F0F0F0",
  navy: "#061C3E", // text, borders, dark panels, END node, code editor
  navySoft: "#0E2748",
  gold: "#D99A0A", // accent, active node fill, headers, rules
  goldDeep: "#C98A06",
  grid: "rgba(6, 28, 62, 0.06)", // faint cool grid lines on the light bg
  white: "#FFFFFF",
  caption: "#8A8F99", // muted grey small-caps captions
} as const;

// DejaVu ships with Chrome on Linux, so renders deterministically headless.
export const FONT_MONO = "'DejaVu Sans Mono', 'Menlo', 'Consolas', monospace";
export const FONT_SANS = "'DejaVu Sans', 'Arial', sans-serif";

export const VIDEO = { width: 1920, height: 1080, fps: 30, durationInFrames: 450 };

// Scene boundaries in frames (30fps). Grid background persists from S2 on.
export const SCENES = {
  intro: { from: 0, durationInFrames: 63 },
  gridReveal: { from: 63, durationInFrames: 45 },
  reactGraph: { from: 108, durationInFrames: 117 },
  codeEditor: { from: 225, durationInFrames: 120 },
  featureGrid: { from: 345, durationInFrames: 75 },
  outro: { from: 420, durationInFrames: 30 },
} as const;
