import { bundle } from "@remotion/bundler";
import { selectComposition, renderStill, renderMedia } from "@remotion/renderer";
import path from "node:path";
import { fileURLToPath } from "node:url";

const DIR = path.dirname(fileURLToPath(import.meta.url));
const abs = (p) => path.join(DIR, p);

const mode = process.argv[2] || "still";
const frame = Number(process.argv[3] || 170);

const PORT = Number(process.env.REMOTION_PORT || 45678);
const serveUrl = await bundle({ entryPoint: abs("src/index.ts") });
const composition = await selectComposition({ serveUrl, id: "PromoV2", port: PORT });

const chromiumOptions = { gl: "swiftshader", headless: true };

if (mode === "still") {
  await renderStill({
    composition,
    serveUrl,
    output: abs(`out/n${frame}.png`),
    frame,
    chromiumOptions,
    port: PORT,
  });
  console.log("STILL_OK", frame);
} else if (mode === "stills") {
  const frames = String(process.argv[3]).split(",").map(Number);
  for (const f of frames) {
    await renderStill({
      composition,
      serveUrl,
      output: abs(`out/n${f}.png`),
      frame: f,
      chromiumOptions,
      port: PORT,
    });
    console.log("STILL_OK", f);
  }
} else {
  await renderMedia({
    composition,
    serveUrl,
    codec: "h264",
    outputLocation: abs("out/promo.mp4"),
    chromiumOptions,
    port: PORT,
    concurrency: 3,
    onProgress: ({ progress }) =>
      process.stdout.write(`\rRENDER ${Math.round(progress * 100)}%`),
  });
  console.log("\nMEDIA_OK");
}
