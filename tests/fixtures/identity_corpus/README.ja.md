<!-- neograph-i18n: source=tests/fixtures/identity_corpus/README.md locale=ja source_sha256=83accd0c1a18f7f267e987bd5317425e2b0b24cc05a9249464d08243fe4f72e9 -->
# ゴールデンIdentityコーパス

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`manifest.json` には、Programのソースと、それをコンパイルするときに使うレジストリが並んでいます。`golden.json` は、各ソースのコンパイル結果を固定値（pin）として記録しています。記録するのは、封印されたCore定義、その `definition_hash`、コンパイル済みプランのidentity、封印された診断、バンドルIDです。`tests/test_identity_golden.cpp` と `bindings/python/tests/test_identity_golden.py` が、すべての固定値を検査します。

新しいCoreのキーを使わないプログラムは、すべての固定値を維持しなければなりません。そのようなプログラムの固定値が変わった場合は、互換性が壊れています。

## プラットフォームごとの規則

JavaScriptのソースエンベロープは `ProgramSource::JAVASCRIPT_QUICKJS_BUILD_OPTIONS` を封印しますが、この値はMSVC向けの移植版では異なります。そのため、次の固定値は `posix_quickjs` の下に置き、POSIXのQuickJSビルドのidentityに対してだけ検査します。

- JavaScriptの `source_hash` と `bundle_id`
- `main()` を持つプログラムの `canonical_program_hash`（ソースを制御用のソースとして封印するため）

`all_platforms` の下にあるものは、C++ビルダーのプログラムの固定値をすべて含め、すべてのプラットフォームで検査します。

## 再生成

意図してidentityを変更したあとにだけ実行します。CIでは実行しないでください。

```sh
NEOGRAPH_REGENERATE_IDENTITY_CORPUS=1 \
    <build>/tests/neograph_identity_golden_tests --gtest_filter='IdentityGolden.*'
```

`NEOGRAPH_BUILD_QUICKJS_CONTROL=ON` を指定したPOSIXのビルドが必要です。`CI` または `GITHUB_ACTIONS` が設定されていると、実行を拒否します。出力は決定的なので、ファイルを手で編集すると、`IdentityGolden.GoldenFileIsTheDeterministicRegeneration` が失敗します。コミットする前に、差分を確認してください。
