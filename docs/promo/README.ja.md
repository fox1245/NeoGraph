<!-- neograph-i18n: source=docs/promo/README.md locale=ja source_sha256=eda23a0fd62ee1ba5a3eb5f26292eac3aa3ac273903f5fd8c5714eaa49627b54 -->
# NeoGraphのプロモーション動画 — Remotionのソース

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

リポジトリのREADMEの先頭に載せている、15秒のプロモーション動画のソースです（`docs/videos/neograph-promo-v3.mp4` と `docs/images/neograph-promo-v3.gif`）。

ソースをコミットしてあるので、シーンを編集して、もう一度レンダリングできます。生成される動画は、Programのパイプラインを図解したものです。APIリファレンスでも、ベンチマークでも、適格性確認の記録でもありません。メディアのファイル名にある `v3` は、アセットのラベルであり、SDKのインターフェースのリビジョンではありません。NeoGraph `0.13.1` はalpha SDK `0.1.1`（インターフェースと共有世代は4）を使います。既存のレンダリング結果は、そのランタイムも、保留中の検証も示すものではありません。

## シーン（`src/scenes/`）

1. `Intro` — ワードマークと、ゴールドの罫線の描画
2. グリッドの表示（常に表示される `GridBackground` が、何も映らない間にフェードインする）
3. `ReactGraph` — 提案 → コンパイル → 意味検証のゲート → admissionというパイプライン。コネクターは、ボックスの端に固定されています（右の中央 → 左の中央）。ReActのループバックの弧は、`llm_call` の上端に着地します。レイアウトを変えるには、`ReactGraph.tsx` の `NODES` を編集します。幅と間隔は明示的に指定されていて、中央に揃えられているので、要素が重なることはありません。
4. `CodeEditor` — QuickJSの `define()` とジェネレーターの `main()` が、タイプされるように表示される
5. `FeatureOutro` — 現在のProgram、Hook、ランタイムコンテキスト、プロトコルの機能のグリッド → エンディングのパネル

タイミングは、`src/theme.ts`（`SCENES`、`VIDEO`）で指定します。

## 再ビルド

Node.js／npmとFFmpegをインストールします。`package-lock.json` が、Remotionの依存ツリーとReactの依存ツリーを固定しています。`npm ci` を実行すると、記録された依存関係がインストールされます。

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

`node render.mjs stills 30,175,290,445` は、フルエンコードをせずに、確認用のフレームを1枚ずつ `out/` にレンダリングします。見た目の差分を手早く確かめるのに便利です。

ヘッドレス環境やサンドボックス環境での注意：レンダラーはローカルのHTTPサーバーにバインドします。デフォルトのポートスキャンがブロックされることがあるので、`render.mjs` は `REMOTION_PORT`（デフォルトは45678）で固定しています。Remotionは、Chrome Headless Shellがインストールされていない場合にダウンロードします。ダウンロードサイズは一定ではありません。

`node_modules/` と `out/` は、gitignoreされています。
