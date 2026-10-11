<!-- neograph-i18n: source=tests/fixtures/identity_corpus/README.md locale=ja source_sha256=83accd0c1a18f7f267e987bd5317425e2b0b24cc05a9249464d08243fe4f72e9 -->
# ゴールデン identity コーパス

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`manifest.json` は Program のソースと、各ソースのコンパイル時に基準となる registry を列挙します。
`golden.json` は各ソースのコンパイル結果を pin（固定値）として記録します。対象は封印済み（sealed）の
Core definition、その `definition_hash`、コンパイル済み plan の identity、封印済みの diagnostics、
bundle id です。`tests/test_identity_golden.cpp` と `bindings/python/tests/test_identity_golden.py`
がすべての pin を検査します。

新しい Core キーを使わない program は、すべての pin を維持しなければなりません。そのような program の
pin が変わった場合は、互換性の破壊です。

## プラットフォーム規則

JavaScript のソース envelope は `ProgramSource::JAVASCRIPT_QUICKJS_BUILD_OPTIONS` を封印しますが、
この値は MSVC 移植版では異なります。そのため、次の pin は `posix_quickjs` の下に置き、
POSIX QuickJS ビルドの identity に対してのみ検査します。

- JavaScript の `source_hash` と `bundle_id`
- program に `main()` がある場合の `canonical_program_hash`（ソースを control source として
  封印するため）

`all_platforms` の下にあるものは、C++ builder program のすべての pin を含め、すべてのプラットフォームで検査します。

## 再生成

意図した identity の変更後にのみ実行し、CI では決して実行しません。

```sh
NEOGRAPH_REGENERATE_IDENTITY_CORPUS=1 \
    <build>/tests/neograph_identity_golden_tests --gtest_filter='IdentityGolden.*'
```

`NEOGRAPH_BUILD_QUICKJS_CONTROL=ON` を指定した POSIX ビルドが必要です。`CI` または `GITHUB_ACTIONS`
が設定されている場合は実行を拒否します。出力は決定的（deterministic）なので、ファイルを手で編集すると
`IdentityGolden.GoldenFileIsTheDeterministicRegeneration` が失敗します。コミットする前に diff を確認してください。
