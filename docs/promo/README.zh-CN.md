<!-- neograph-i18n: source=docs/promo/README.md locale=zh-CN source_sha256=eda23a0fd62ee1ba5a3eb5f26292eac3aa3ac273903f5fd8c5714eaa49627b54 -->
# NeoGraph 宣传片 — Remotion 源码

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

这是仓库README顶部展示的15秒宣传片源文件（`docs/videos/neograph-promo-v3.mp4` + `docs/images/neograph-promo-v3.gif`）。

源码已提交，以便重新编辑和渲染场景。
生成 video 展示 Program pipeline，不是 API reference、benchmark 或 qualification 记录。
媒体文件名的 `v3` 是 asset 标签，不是 SDK interface revision。NeoGraph `0.13.1` 使用 alpha SDK `0.1.1`，interface/shared generation 4；现有 render 不证明该 runtime 或尚未完成的验证。

## 场景（`src/scenes/`）

1. `Intro` — 字标 + 金色规则绘制
2. 网格显现（持久化的`GridBackground`在空拍上淡入）
3. `ReactGraph` — 提案 → 编译 → 语义验证 → 准入(admission)流水线。连接器锚定在框边缘（右中 → 左中），ReAct 回环弧落在 `llm_call` 的顶部边缘。编辑 `NODES`（位于 `ReactGraph.tsx`）即可调整布局；宽度和间距均为显式设置并居中排列，因此不会重叠。
4. `CodeEditor` — QuickJS `define()` + 生成器`main()`自行输入并呈现
5. `FeatureOutro` — 当前Program、Hook、运行时上下文和协议功能网格 → 结尾面板

时间线位于`src/theme.ts`中（`SCENES`、`VIDEO`）。

## 重建

安装 Node.js/npm 和 FFmpeg。`package-lock.json` 固定 Remotion/React 的依赖树，
`npm ci` 安装这些已记录的依赖。

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

`node render.mjs stills 30,175,290,445`将单个验证帧渲染到`out/`，以便无需完整编码即可快速进行视觉对比。

在 headless/sandbox 环境中 renderer 绑定本地 HTTP server。默认 port scan 可能受限，因此 `render.mjs` 使用 `REMOTION_PORT`（默认 45678）。Chrome Headless Shell 未安装时 Remotion 会下载它，下载大小会变化。

`node_modules/`和`out/`在gitignore中。
