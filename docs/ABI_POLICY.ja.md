<!-- neograph-i18n: source=docs/ABI_POLICY.md locale=ja source_sha256=eadbd5019a617097b40042dc1f99f69e4ef46d890f562c028484e003fa10f5ce -->
# バイナリ互換性ポリシー

**Languages:** [English](ABI_POLICY.md) | [한국어](ABI_POLICY.ko.md) | [日本語](ABI_POLICY.ja.md) | [简体中文](ABI_POLICY.zh-CN.md)

このポリシーは、インストール済みの NeoGraph 静的・共有ライブラリを使う
C++ コンシューマーに適用されます。Python wheel は対応する拡張と
ライブラリを一つのパッケージとして配布するため、同梱ライブラリだけを
個別に置き換えてはいけません。
typed provider 移行は必須の再コンパイル境界で、旧恒久互換計画を置き換えます。NeoGraph の pre-v1 loader 名を維持しても旧 provider オブジェクトの互換性はありません。NeoGraph と SchemaProvider SDK の一致したヘッダー/ライブラリを一括設置します。SDK interface revision 3 と `libsp_*.so.3` は out-of-line capability gate を持つ別 shared ABI です。不安定 package `0.0.0` は安定リリースではありません。現 SDK runtime/archive は Linux/POSIX を必要とします。以下の Windows/macOS 名前例は packaging policy であり、新依存 runtime の動作証拠ではありません。Python provider binding/wrapper は延期・未移植です。

## バージョン契約

NeoGraph は `pyproject.toml` からプロジェクトバージョンを読み取ります。
CMake はその値を全ての公開 `neograph_*` バイナリライブラリの `VERSION`、
メジャー番号を `SOVERSION` として設定します。

| リリース系列 | ローダー ABI 世代 | 契約 |
|---|---:|---|
| `0.x` | `0` | v1 前であり、バイナリ互換性は保証されません。C++ コンシューマーの再ビルドを必須にする場合は changelog と移行ガイドで境界を告知します。 |
| `1.x` | `1` | 安定した v1 ABI です。例外的なセキュリティ修正が告知されない限り、minor・patch リリースで公開 virtual 順序とオブジェクトレイアウトを維持します。 |
| `N.x`, `N >= 2` | `N` | メジャーリリースは新しい ABI 世代を導入でき、C++ コンシューマーの再ビルドが必要です。 |

`SOVERSION 0` は全ての `0.x` バイナリが交換可能という意味ではありません。
再ビルド境界は各リリースノートが決定します。

これは v1 前のリスクを明示的に受容する決定です。互換性のない `0.x`
置換も ABI 世代 0 のため動的ローダーでは拒否できません。ヘッダーと
ライブラリを一括更新し、告知された境界を越えて共有ライブラリだけを
hot-swap しないでください。1.0 で世代 1 のレイアウトを固定します。

## インストール名

- Linux は完全版、`.so.0` 互換リンク、バージョンなしリンカー名を
  インストールし、ELF SONAME を `libneograph_core.so.0` とします。
- macOS は対応する `.dylib` 名とメジャーバージョン install name を使います。
- Windows は `neograph_core.dll` のような接尾辞なしの DLL 名を維持します。
- 共有ライブラリは Linux の `$ORIGIN`、macOS の `@loader_path` を使い、
  同じディレクトリの `neograph_*` 依存ライブラリを解決します。
- 静的アーカイブには実行時 SONAME がありません。告知された境界では
  コンシューマーを再コンパイルしてください。

## 必須の再ビルド境界

| アップグレード | 要件 | 理由 |
|---|---|---|
| `0.9.0` 未満から `0.9.0+` | 全 C++ コンシューマーとカスタムノードを再ビルド | `GraphNode` から旧 virtual 8 個が削除され、vtable が変わりました。 |
| `0.11.1` 以下から次のリリース | 全 C++ コンシューマーを再ビルド | bounded runtime/transport 状態のため、`NodeCache`、`EngineConfig`、`CompletionParams`、`Agent`、`RequestOptions`、`SseEventParser`、provider config の公開レイアウトが変わりました。`SyncGraphNode` 自体は追加のみです。 |
| 任意の `0.x` から `1.0.0` | 全 C++ コンシューマーを再ビルド | v1 レイアウトを固定し、ABI 世代を 0 から 1 に変更します。 |

## 公開 virtual インターフェース

- `GraphNode` の正式な実行 virtual は `run(NodeInput)` 一つです。
- `Provider`: `get_name()`, `family()`, `prepare(ProviderRequest)`;
  `invoke(_async)` / `dispatch(_async)` → `sp::runtime::Result`.
  ソースとバイナリの破壊的変更です。全 C++ 利用者とカスタムプロバイダーを新しい一致したヘッダー/ライブラリで再コンパイルします。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter、Responses WebSocket は alias/互換 bridge なしで削除されました。SDK は不安定 `0.0.0`、interface revision 3 / shared ABI 3、out-of-line capability check を使用し、安定リリースの宣言ではありません。現 runtime/archive は Linux/POSIX で、Windows・macOS・WASM runtime の資格検証を意味しません。Python provider binding/wrapper は延期され、この C++ 変更では移植されません。
- 将来の `CheckpointStore` 非同期移行もこのポリシーに従います。v1 後は
  安定レイアウトの変更より、別の capability interface と adapter を優先します。

## 検証

`scripts/test_find_package.sh` は installed-consumer 検査手順であり、存在するだけで現 pass を主張しません。現 SDK ABI3 全再ビルド/CTest は 26/26 pass、shared 設置 consumer は実際の local HTTP 二 turn typed 要求、tool/native/refusal/known-zero 結果と mismatch 拒否を実行しました。NeoGraph・Python・Windows・macOS・WASM・有料 live-provider 互換の資格検証ではありません。NeoGraph 統合検証は別途報告します。
