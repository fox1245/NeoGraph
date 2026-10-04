<!-- neograph-i18n: source=docs/ABI_POLICY.md locale=ja source_sha256=e17b6ed782cd2ef13aa7091cb02ea0f5fa2459df3e1f670ef014fd3c9bc9b07d -->
# binary 互換方針

**Languages:** [English](ABI_POLICY.md) | [한국어](ABI_POLICY.ko.md) | [日本語](ABI_POLICY.ja.md) | [简体中文](ABI_POLICY.zh-CN.md)

## version と loader 契約

CMake は `pyproject.toml` から NeoGraph version を読み、公開 compiled library の `VERSION` に全 version、`SOVERSION` に major 成分を設定する。

| release 系列 | loader 世代 | 契約 |
|---|---:|---|
| `0.x` | `0` | pre-v1。公表した rebuild 境界で binary 互換が破れる場合がある。 |
| `1.x` | `1` | 将来の stable v1 方針であり、v1 発売の主張ではない。 |
| `N.x`, `N >= 2` | `N` | major ABI 境界。consumer を rebuild する。 |

`SOVERSION 0` は pre-v1 package の loader 名であり、layout の交換可能性ではない。loader はすべての非互換 `0.x` 置換を拒否できない。対象 release note を読み、header/library を一括交換し、公表した境界ごとに rebuild する。稼働 process に個別 pre-v1 library を hot-swap しない。

## 必須 rebuild 境界

- pre-`0.9.0` から `0.9.0+`: GraphNode は旧実行 virtual 八つを削除した。custom node は `run(NodeInput)` を実装し、SyncGraphNode は追加 adapter である。
- 以後の pre-v1 bounded-resource、UsageAccumulator 予約、issue #216 変更: 公開 layout に accounting、admission、cache、runtime-interposition 状態が加わった。対応 header で rebuild する。
- typed-provider 移行: 対応する NeoGraph/SchemaProvider SDK header/library で全 C++ consumer と custom provider を rebuild する。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、旧 descriptor interpreter、Responses WebSocket は alias なしで削除された。
- event-driven provider dispatch: CancelToken は `std::stop_source` を使い `stop_token()` を公開する。`cancel`、`fork`、Asio-slot signature が同じでも旧 inline cancel code は互換ではない。
- 将来の `1.0.0` 境界は loader 世代を `1` にし rebuild を要求する。generation-1 freeze はその release の方針で、現在の検証結果ではない。

## 公開 interface

Provider の subclass hook は `get_name`、`family`、`prepare(ProviderRequest)` の三つ。共通 `invoke(_async)`、`dispatch(_async)` は owned typed request を消費し、不変 `sp::runtime::Result` を返す。virtual completion override の対ではない。

CheckpointStore は明示的 adapter 移行に legacy layout を保つ。sync 既定実装は async override に渡さず失敗し、async 既定実装は sync override を offload する。新しい async-only backend は AsyncCheckpointStore と `adapt_async_checkpoint_store` を、sync capability backend は CheckpointStoreCore と `adapt_checkpoint_store` を使う。公表した pre-v1 境界で rebuild する。adapter 設計は library だけの置換を許可しない。

## SDK と Python 境界

LLM node を無効にしても Core は外部 `SchemaProvider::runtime` を要求する。選択した SDK release は `0.1.0` alpha、interface revision `4`、shared-library ABI revision `4` で、out-of-line capability check を持つ。stable interface の主張ではない。対応 component を一緒に install する。`libsp_*.so.4` 世代は NeoGraph loader 世代と Python `abi3` wheel tag の双方と別である。

Interface 4 は family 別 request control を追加し、公開 request layout を変更する。SDK consumer、NeoGraph、Python extension を一緒に rebuild する。interface-3 header/library と interface 4 は混在できない。Native archive v3 / `spna3` と portable JSON v2 は独立した形式で、変更されない。過去の interface-3 測定は interface 4 の資格検証ではない。

Python は prepared handle と不変 outcome を含む typed provider 契約を公開し、legacy completion shim はない。extension と対応 library を一つの wheel として install する。bundled NeoGraph/SDK library を個別交換しない。graph の ChatMessage 便宜値は native ProviderMessage custody の代替ではない。[Python binding](python-binding.md)を参照。

wheel は対応する SDK runtime shared library 六つを含み、SDK C++ header/CMake package は含まない。C++ consumer は SDK を別途 install する。source resolution は明示的 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、installed package、公開 revision-pinned archive fallback の順。installed SDK を使う offline build は `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` とし、`CMAKE_PREFIX_PATH` で prefix を渡す。

## installed 名と platform 制限

Linux shared library は versioned file、major-generation SONAME link、unversioned linker 名を持つ。NeoGraph shared library は sibling 依存に `$ORIGIN` を使う。macOS `.dylib`/`@loader_path` と Windows の unsuffixed `.dll` は packaging 規則で、新 SDK runtime の検証ではない。static archive は SONAME を持たず、transitive link 要件も除かない。

記録された interface-3 SDK runtime/archive 検証は Linux/POSIX の範囲で、interface 4 の資格検証ではない。既存 macOS/Windows metadata と依存検証は別で、WASM runtime 検証は確立していない。wheel tag は Python/ABI/platform 互換を記すが、全 runtime 経路を実行した証明ではない。[PyPA tag 仕様](https://packaging.python.org/en/latest/specifications/platform-compatibility-tags/)を参照。

## 検証の根拠

`scripts/test_find_package.sh` は installed-consumer 検査の定義で、pass 結果ではない。日付付きの移行前測定は[歴史的根拠](VALGRIND.md)として残す。現在の NeoGraph/SDK/Python 結果は統合 release report で build、platform、実行経路を明示する。この方針は新しい pass 主張を作らない。
