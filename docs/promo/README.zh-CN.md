<!-- neograph-i18n: source=docs/promo/README.md locale=zh-CN source_sha256=eda23a0fd62ee1ba5a3eb5f26292eac3aa3ac273903f5fd8c5714eaa49627b54 -->
# NeoGraph 宣传片：Remotion 源码

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

仓库 README 顶部展示的 15 秒宣传片的源码
（`docs/videos/neograph-promo-v3.mp4` 和 `docs/images/neograph-promo-v3.gif`）。

源码已提交到仓库，这样可以修改场景并重新渲染。
生成的视频只是用来说明 Program 的流水线，不是 API 参考、基准测试或资格认证记录。
媒体文件名里的 `v3` 只是素材的标签，不是 SDK 的接口修订版。
NeoGraph `0.13.1` 使用 alpha 版 SDK `0.1.1`，接口/共享库代数为 4；
现有的渲染结果既没有演示该运行时，也不代表其待完成的验证。

## 场景（`src/scenes/`）

1. `Intro`：字标（wordmark）和金色分隔线的绘制
2. 网格显现（常驻的 `GridBackground` 在一个空白节拍中淡入）
3. `ReactGraph`：proposal → compile → 语义闸门 → 准入的流水线。
   连接线锚定在方框的边缘（右侧中点 → 左侧中点），
   ReAct 循环的回环弧线落在 `llm_call` 的上边缘。布局通过修改
   `ReactGraph.tsx` 中的 `NODES` 来调整：宽度和间距都是显式给出且居中的，
   所以不会重叠。
4. `CodeEditor`：一段 QuickJS 的 `define()` 加生成器 `main()`，逐字打出
5. `FeatureOutro`：当前的 Program、Hook、运行时上下文和协议功能网格 → 结尾面板

时间轴定义在 `src/theme.ts` 中（`SCENES`、`VIDEO`）。

## 重新构建

先安装 Node.js/npm 和 FFmpeg。`package-lock.json` 固定了 Remotion/React
的依赖树；`npm ci` 会安装其中记录的依赖。

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

`node render.mjs stills 30,175,290,445` 会把单帧验证画面渲染到 `out/`，
无需完整编码，就能快速做视觉对比。

无头或沙箱环境的注意事项：渲染器会绑定一个本地 HTTP
服务器；默认的端口扫描可能被拦截，所以 `render.mjs` 把它固定在
`REMOTION_PORT`（默认 45678）上。Remotion 在尚未安装 Chrome Headless Shell
时会自行下载，下载的大小不固定。

`node_modules/` 和 `out/` 已列入 .gitignore。
