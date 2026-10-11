<!-- neograph-i18n: source=tests/fixtures/identity_corpus/README.md locale=zh-CN source_sha256=83accd0c1a18f7f267e987bd5317425e2b0b24cc05a9249464d08243fe4f72e9 -->
# Golden identity 语料库

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`manifest.json` 列出 Program 源码及其编译时所依据的 registry。`golden.json` 将每个源码的编译结果固定为
pin：封存（sealed）的 Core definition、其 `definition_hash`、编译后 plan 的 identity、封存的 diagnostics
以及 bundle id。`tests/test_identity_golden.cpp` 和 `bindings/python/tests/test_identity_golden.py`
检查每一个 pin。

不使用新 Core 键的 program 必须保持其全部 pin 不变。此类 program 的 pin 一旦发生变化，即属于兼容性破坏。

## 平台规则

JavaScript 源码 envelope 会封存 `ProgramSource::JAVASCRIPT_QUICKJS_BUILD_OPTIONS`，而该值在 MSVC 移植版上不同。
因此以下 pin 保存在 `posix_quickjs` 下，并且只针对 POSIX QuickJS 构建 identity 进行检查：

- JavaScript 的 `source_hash` 和 `bundle_id`；
- program 含有 `main()` 时的 `canonical_program_hash`（它将源码封存为自身的 control source）。

`all_platforms` 下的所有内容都会在每个平台上检查，包括 C++ builder program 的每一个 pin。

## 重新生成

仅在有意变更 identity 之后执行，绝不在 CI 中执行：

```sh
NEOGRAPH_REGENERATE_IDENTITY_CORPUS=1 \
    <build>/tests/neograph_identity_golden_tests --gtest_filter='IdentityGolden.*'
```

需要启用 `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON` 的 POSIX 构建。设置了 `CI` 或 `GITHUB_ACTIONS` 时，它会拒绝运行。
输出是确定性的，因此如果手工编辑过该文件，`IdentityGolden.GoldenFileIsTheDeterministicRegeneration` 会失败。
提交前请审查 diff。
