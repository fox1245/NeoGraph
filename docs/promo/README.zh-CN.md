<!-- neograph-i18n: source=docs/promo/README.md locale=zh-CN source_sha256=9321b772f9d90caa27addc8f166b403e9874ce911798bfd16ed8b703811197a4 -->
# NeoGraph 宣传片 — Remotion 源码

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

这是仓库README顶部展示的15秒宣传片源文件（`docs/videos/neograph-promo-v3.mp4` + `docs/images/neograph-promo-v3.gif`）。

**这是有意提交的。** 原始宣传片只渲染过一次，源文件从未检入，因此ReAct图场景中损坏的连接器无法在不从头重建的情况下修复。请将源文件保留在此处。

## 场景（`src/scenes/`）

1. `Intro` — 字标 + 金色规则绘制
2. 网格显现（持久化的`GridBackground`在空拍上淡入）
3. `ReactGraph` — 提案 → 编译 → 语义验证 → 准入(admission)流水线。连接器锚定在框边缘（右中 → 左中），ReAct 回环弧落在 `llm_call` 的顶部边缘。编辑 `NODES`（位于 `ReactGraph.tsx`）即可调整布局；宽度和间距均为显式设置并居中排列，因此不会重叠。
4. `CodeEditor` — QuickJS `define()` + 生成器`main()`自行输入并呈现
5. `FeatureOutro` — 当前Program、Hook、运行时上下文和协议功能网格 → 结尾面板

时间线位于`src/theme.ts`中（`SCENES`、`VIDEO`）。

## 重建

```bash
cd docs/promo
npm install
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

无头/沙盒环境注意事项：渲染器绑定本地HTTP服务器 — `render.mjs`将其固定到`REMOTION_PORT`（默认45678），因为默认端口扫描可能被阻止。Remotion在首次运行时下载自己的Chrome Headless Shell（约108 MB）。

`node_modules/`和`out/`在gitignore中。
