<!-- neograph-i18n: source=docs/promo/README.md locale=zh-CN source_sha256=49b4d504ffeb1f20069738e75640caada9b3aeb0ed51c7290f3204b560543051 -->
# NeoGraph 宣传片 — Remotion 源码

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)



仓库 README 顶部显示的 15 秒宣传片的源码（`docs/videos/neograph-promo-v2.mp4` + `docs/images/neograph-promo-v2.gif`）。

**这是故意提交的。** 原始宣传片已渲染一次，并且从未签入源，因此 ReAct 图场景中的连接器坏了，但不从头重建就无法修复。将源码保留在这里。

## 场景（`src/scenes/`）

1. `Intro` — 字标 + 金色分隔线绘制
2. 网格显示（持久的`GridBackground`在空的节拍上淡入）
3. `ReactGraph` — START → llm_call → tool_dispatch → END 管线。
连接器固定在盒子边（右中→左中），并且 ReAct 环回弧落在`llm_call`的上边。通过编辑 `ReactGraph.tsx` 中的 `NODES` 来调整布局 — 宽度/间隙是明确的且居中的，因此没有重叠。
4. `CodeEditor` — `react_agent.py`自动输入
5. `FeatureOutro` — 功能 chip 网格 → 结尾面板（一个连续场景）

时间参数位于`src/theme.ts`（`SCENES`, `VIDEO`）。

## 重建
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

`node render.mjs stills 30,175,290,445`会把单个验证帧渲染到`out/`用于快速视觉比较，无需完整编码。

无头/沙盒机器注意事项：渲染器绑定本地 HTTP 服务器 -`render.mjs`将其固定到 `REMOTION_PORT`（默认 45678）因为默认端口扫描可能被阻止。 Remotion 在首次运行时下载自己的 Chrome Headless Shell (~108 MB)。

`node_modules/`和`out/`已加入 gitignore。
