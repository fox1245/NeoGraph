<!-- neograph-i18n: source=docs/promo/README.md locale=ja source_sha256=9321b772f9d90caa27addc8f166b403e9874ce911798bfd16ed8b703811197a4 -->
# NeoGraph プロモ — Remotion ソース

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

リポジトリのREADME上部に表示される15秒プロモーションのソースです（`docs/videos/neograph-promo-v3.mp4` + `docs/images/neograph-promo-v3.gif`）。

**これは意図的に含まれています。** 元のプロモーションは一度レンダリングされただけで、ソースはチェックインされていなかったため、ReActグラフシーン内の壊れたコネクタをゼロから再構築せずに修正することはできませんでした。ソースはここに保持してください。

## シーン（`src/scenes/`）

1. `Intro` — ワードマーク＋ゴールドルールの描画
2. グリッドの表示（永続的な`GridBackground`が空のビートの上にフェードイン）
3. `ReactGraph` — 提案 → コンパイル → 意味検証 → admissionパイプライン。コネクタはボックスエッジ（右中 → 左中）に固定され、ReActループバックアークは`llm_call`の上端に着地します。`NODES`を`ReactGraph.tsx`で編集してレイアウトしてください — 幅/ギャップは明示的で中央揃えなので、何も重なりません。
4. `CodeEditor` — QuickJSの`define()`＋ジェネレーター`main()`が自動で入力される様子
5. `FeatureOutro` — 現在のProgram、Hook、ランタイムコンテキスト、およびプロトコル機能グリッド → アウトロパネル

タイミングは`src/theme.ts`（`SCENES`、`VIDEO`）にあります。

## 再構築

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

`node render.mjs stills 30,175,290,445`は単一の検証フレームを`out/`にレンダリングし、完全なエンコードなしで迅速な視覚的差分を可能にします。

ヘッドレス/サンドボックス環境向けの注意事項：レンダラーはローカルHTTPサーバーをバインドします — `render.mjs`はデフォルトのポートスキャンがブロックされる可能性があるため、これを`REMOTION_PORT`（デフォルト45678）に固定します。Remotionは初回実行時に独自のChrome Headless Shellをダウンロードします（約108 MB）。

`node_modules/`と`out/`はgitignore対象です。
