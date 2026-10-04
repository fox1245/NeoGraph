# NeoGraph promo — Remotion source

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

Source for the 15-second promo shown at the top of the repository README
(`docs/videos/neograph-promo-v3.mp4` + `docs/images/neograph-promo-v3.gif`).

The source is committed so the scenes can be edited and rendered again.
The generated video illustrates the Program pipeline; it is not an API
reference, benchmark, or qualification record.
The `v3` media filenames are asset labels, not SDK interface revisions.
NeoGraph `0.13.0` uses alpha SDK `0.1.0`, interface/shared generation 4;
the existing render does not demonstrate that runtime or its pending validation.

## Scenes (`src/scenes/`)

1. `Intro` — wordmark + gold rule draw
2. grid reveal (the persistent `GridBackground` fades in over an empty beat)
3. `ReactGraph` — the proposal → compile → semantic gate → admit pipeline.
   Connectors are anchored to the box edges (right-mid → left-mid) and
   the ReAct loop-back arc lands on `llm_call`'s top edge. Lay it out by
   editing `NODES` in `ReactGraph.tsx` — widths/gaps are explicit and
   centred so nothing overlaps.
4. `CodeEditor` — a QuickJS `define()` + generator `main()` types itself in
5. `FeatureOutro` — current Program, Hook, runtime-context, and protocol feature grid → outro panel

Timing lives in `src/theme.ts` (`SCENES`, `VIDEO`).

## Rebuild

Install Node.js/npm and FFmpeg. `package-lock.json` pins the Remotion/React
dependency tree; `npm ci` installs those recorded dependencies.

```bash
cd docs/promo
npm ci
node render.mjs media          # → out/promo.mp4 (1920x1080, 15s)

# compress + GIF (what ships in docs/):
ffmpeg -i out/promo.mp4 -c:v libx264 -crf 27 -preset slow \
  -pix_fmt yuv420p -movflags +faststart -an ../videos/neograph-promo-v3.mp4 -y
ffmpeg -i out/promo.mp4 -vf "fps=14,scale=960:540:flags=lanczos,palettegen=max_colors=128:stats_mode=diff" -y /tmp/pal.png
ffmpeg -i out/promo.mp4 -i /tmp/pal.png \
  -lavfi "fps=14,scale=960:540:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=5" \
  -y ../images/neograph-promo-v3.gif
```

`node render.mjs stills 30,175,290,445` renders single verification
frames to `out/` for quick visual diffing without a full encode.

Notes for headless/sandboxed boxes: the renderer binds a local HTTP
server — `render.mjs` pins it to `REMOTION_PORT` (default 45678)
because the default port scan can be blocked. Remotion downloads its
Chrome Headless Shell when it is not already installed; the download size varies.

`node_modules/` and `out/` are gitignored.
