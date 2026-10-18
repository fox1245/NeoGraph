<!-- neograph-i18n: source=tests/live/README.md locale=ja source_sha256=4834040858097bba00645bd6041377aa2c8b1cb59124098518aea7961d848922 -->
# ライブ・エンドツーエンド・スイート

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

単体テストと統合テストは、記録したプロバイダー通信やスクリプトで作った通信を再生します。そのため、NeoGraph が送るものをプロバイダーがいまも受け付けるか、プロバイダーの今日の応答を NeoGraph が受け付けるかは示せません。だからすべてのリリースは実際のキーでこのスイートを通過し、そのコミットに合格したレポートがなければ CI はリリースの公開を拒否します。

## 対象

- **Python**（`bindings/python/tests/live/` と `bindings/python/tests/test_live_provider.py`）：
  - 8 つのファミリープリセット（OpenAI Chat と Responses、Anthropic Messages、Gemini Generate と Interactions、OpenRouter Chat、Responses、Messages）を、それぞれバッファとストリームで呼び出し、ストリームのデルタをつないだものが最終テキストと一致することを確かめます。
  - Claude 5.5 モデル。
  - 画像入力。
  - すべてのプリセットでの強制ツール呼び出しと、ツールの往復全体。
  - OpenRouter Chat と Gemini Generate が生成した画像の保持。
  - Anthropic Messages の制御：thinking モードと effort、tool choice、temperature の規則、プロンプトキャッシュ、そして OpenRouter 経由の同じ制御。
  - 型付きの失敗：無効なキー、キャンセルしたストリーム、期限切れの deadline。
- **C++**（`tests/live_e2e_test.cpp`、ターゲット `neograph_live_e2e_tests`）：
  - Claude 5.5 のサーバーツール、生成画像、Interactions を使うプロバイダーアダプター。
  - 同梱カタログのすべてのモデルに対する型付きクライアント `DecisionsClient`。
  - 同梱の設定と `gpt-image-2` を使う `ImagesClient`。
  - 同梱の設定を使う `VeoClient`。
- **既知のギャップ**は、そのリリースラインで必ず失敗しなければならないケースです（strict xfail）。`tests/live/manifest.json` が、それを直す issue とともに一覧にしています。修正が入るとケースが通り始めるので、マニフェストも合わせて変える必要があります。

呼び出しごとに安いモデルと小さな上限を使います。全体の実行費用は数米セントで、そのほとんどは Veo の動画です。

## 実行

確認したいコミットの Python 拡張と C++ テストをビルドしてから、次を実行します：

```bash
python scripts/live_e2e.py run --env-file path/to/keys.env \
    --python path/to/python --pythonpath path/to/build \
    --cxx path/to/build/tests/neograph_live_e2e_tests
```

- **キー。** env ファイルには `OPENAI_API_KEY`、`GEMINI_API_KEY`、`ANTHROPIC_API_KEY`、`OPENROUTER_API_KEY` を入れます。スクリプトはこれらをテストプロセスにだけ渡します。表示も保存もせず、失敗メッセージにキーの値が現れれば別の文字列に置き換えます。
- **クリーンなツリー。** レポートはコミットを指すので、`run` はコミットしていない変更がある作業ツリーを拒否します。
- **バージョン。** `run` はツリーのバージョンと異なる `neograph_engine` を拒否します。
- **出力。** `tests/live/report.json` を書きます。コミット、ツリーのハッシュ、NeoGraph のバージョン、SchemaProvider の固定コミット、そしてすべてのケースとその結果が入ります。
- **部分実行。** `--select` と `--cxx-filter` でスイートの一部だけを実行できます。部分的なレポートでリリースを承認することはできません。

## リリースゲート

1. リリースのプルリクエスト（`release/*` ブランチ）は、最終コミットで実行して得た `tests/live/report.json` をコミットします。
2. `Live e2e report` ワークフローが、キーなしで `python scripts/live_e2e.py verify` を実行します。次がすべて成り立つときだけ通ります：
   - レポートがこのツリー（レポート以外のすべての追跡ファイル）を指している。
   - レポートがこのバージョンのもので、クリーンなツリーから作られている。
   - マニフェストのすべてのケースが期待どおりの結果になっている。通ったか、一覧にある既知のギャップとして失敗した。
   - ほかに失敗したケースがない。
3. Wheels ワークフローは、PyPI に公開する前にリリースタグで同じ検査をします。

ケースを追加または削除したら、`python scripts/live_e2e.py manifest --python … --pythonpath … --cxx …` が、スイートが収集したケースからマニフェストを書き直し、既知のギャップはそのまま残します。コミットする前に差分を確認してください。
