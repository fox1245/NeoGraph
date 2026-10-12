<!-- neograph-i18n: source=tests/fixtures/identity_corpus/README.md locale=zh-CN source_sha256=83accd0c1a18f7f267e987bd5317425e2b0b24cc05a9249464d08243fe4f72e9 -->
# 黄金身份语料库

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`manifest.json` 列出了各个 Program 源码，以及它们编译时所对照的注册表。
`golden.json` 固定了每个 Program 编译后的结果：封存的 Core 定义、它的
`definition_hash`、编译计划的标识、封存的诊断和 bundle id。
`tests/test_identity_golden.cpp` 和
`bindings/python/tests/test_identity_golden.py` 会检查每一项固定值。

没有使用新 Core 键的 Program，所有固定值都必须保持不变。这类 Program 的任何
固定值发生变化，都属于兼容性破坏。

## 平台规则

JavaScript 源码封装会封存 `ProgramSource::JAVASCRIPT_QUICKJS_BUILD_OPTIONS`，
这个值在 MSVC 移植版上是不同的。因此下面这些固定值保存在 `posix_quickjs` 之下，
只针对 POSIX 的 QuickJS 构建身份进行检查：

- JavaScript 的 `source_hash` 和 `bundle_id`；
- Program 带有 `main()` 时的 `canonical_program_hash`（它会把源码作为控制源码封存）。

`all_platforms` 之下的所有内容，在每个平台上都会检查，其中包括 C++ 构建器 Program
的每一项固定值。

## 重新生成

只在有意改变了身份之后才这样做，并且绝不要在 CI 中进行：

```sh
NEOGRAPH_REGENERATE_IDENTITY_CORPUS=1 \
    <build>/tests/neograph_identity_golden_tests --gtest_filter='IdentityGolden.*'
```

这需要一个设置了 `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON` 的 POSIX 构建。设置了 `CI` 或
`GITHUB_ACTIONS` 时，脚本会拒绝运行。输出是确定性的，所以如果文件被手工改过，
`IdentityGolden.GoldenFileIsTheDeterministicRegeneration` 就会失败。提交之前请先审阅 diff。
