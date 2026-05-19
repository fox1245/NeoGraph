# NeoGraph promo — Remotion source

Source for the 15-second promo shown in the top of the repo README
(`docs/videos/neograph-promo-v2.mp4` + `docs/images/neograph-promo-v2.gif`).

**This is committed on purpose.** The original promo was rendered once
and the source was never checked in, so a broken connector in the
ReAct-graph scene could not be fixed without rebuilding from scratch.
Keep the source here.

## Scenes (`src/scenes/`)

1. `Intro` — wordmark + gold rule draw
2. grid reveal (the persistent `GridBackground` fades in over an empty beat)
3. `ReactGraph` — the START → llm_call → tool_dispatch → END pipeline.
   Connectors are anchored to the box edges (right-mid → left-mid) and
   the ReAct loop-back arc lands on `llm_call`'s top edge. Lay it out by
   editing `NODES` in `ReactGraph.tsx` — widths/gaps are explicit and
   centred so nothing overlaps.
4. `CodeEditor` — `react_agent.py` types itself in
5. `FeatureOutro` — feature chip grid → outro panel (one continuous scene)

Timing lives in `src/theme.ts` (`SCENES`, `VIDEO`).

## Rebuild

```bash
cd docs/promo
npm install
node render.mjs media          # → out/promo.mp4 (1920x1080, 15s)

# compress + GIF (what ships in docs/):
ffmpeg -i out/promo.mp4 -c:v libx264 -crf 27 -preset slow \
  -pix_fmt yuv420p -movflags +faststart -an ../videos/neograph-promo-v2.mp4 -y
ffmpeg -i out/promo.mp4 -vf "fps=14,scale=960:540:flags=lanczos,palettegen=max_colors=128:stats_mode=diff" -y /tmp/pal.png
ffmpeg -i out/promo.mp4 -i /tmp/pal.png \
  -lavfi "fps=14,scale=960:540:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=5" \
  -y ../images/neograph-promo-v2.gif
```

`node render.mjs stills 30,175,290,445` renders single verification
frames to `out/` for quick visual diffing without a full encode.

Notes for headless/sandboxed boxes: the renderer binds a local HTTP
server — `render.mjs` pins it to `REMOTION_PORT` (default 45678)
because the default port scan can be blocked. Remotion downloads its
own Chrome Headless Shell on first run (~108 MB).

`node_modules/` and `out/` are gitignored.
