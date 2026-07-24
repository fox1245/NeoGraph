<!-- neograph-i18n: source=docs/promo/README.md locale=ja source_sha256=49b4d504ffeb1f20069738e75640caada9b3aeb0ed51c7290f3204b560543051 -->
# NeoGraph プロモーション — リモート ソース

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

リポジトリの README の上部にある 15 秒のプロモーションのソース
(`docs/videos/neograph-promo-v2.mp4` + `docs/images/neograph-promo-v2.gif`)。

**これは意図的にコミットされています。** 元のプロモーションは一度レンダリングされました
ソースがチェックインされていなかったため、ソースのコネクタが壊れていました。
ReAct-graph シーンは最初から再構築しないと修正できませんでした。
ソースはここに置いておきます。

## シーン (`src/scenes/`)

1. `Intro` — ワードマーク + ゴールド ルール ドロー
2. グリッドの表示 (持続的な `GridBackground` が空のビートにフェードインします)
3. `ReactGraph` — START → llm_call → tools_dispatch → END パイプライン。
   コネクタはボックスの端 (右中央→左中央) に固定されており、
   ReAct ループバック アークは `llm_call` の上端に到達します。レイアウトしてください
   `ReactGraph.tsx` で `NODES` を編集 — 幅/ギャップは明示的であり、
   何も重ならないように中央に配置されます。
4. `CodeEditor` — `react_agent.py` は自分自身を入力します
5. `FeatureOutro` — フィーチャー チップ グリッド → アウトロ パネル (1 つの連続したシーン)

タイミングは `src/theme.ts` (`SCENES`、`VIDEO`) にあります。

## 再構築

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

`node render.mjs stills 30,175,290,445` は単一の検証をレンダリングします
フレームを `out/` に変換すると、完全なエンコードを行わずに簡単に視覚的な差分を得ることができます。

ヘッドレス/サンドボックス ボックスに関する注意: レンダラーはローカル HTTP をバインドします。
サーバー — `render.mjs` は `REMOTION_PORT` に固定します (デフォルトは 45678)
デフォルトのポートスキャンがブロックされる可能性があるためです。リモートはそのファイルをダウンロードします
最初の実行時に独自の Chrome ヘッドレス シェル (約 108 MB)。

`node_modules/` と `out/` は gitignore されます。
