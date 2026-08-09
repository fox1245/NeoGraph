<!-- neograph-i18n: source=CHANGELOG.md locale=ja source_sha256=0ce9d7195740e020aeb3404702dd07f670ab5c0c1048ae36e34a73f5c83a6604 -->
# 変更履歴

**Languages:** [English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md) | [日本語](CHANGELOG.ja.md) | [简体中文](CHANGELOG.zh-CN.md)

NeoGraph の全注目すべき変更を本ファイルに文書化します。

形式は [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) に従います。
バージョニングは [Semantic Versioning](https://semver.org/spec/v2.0.0.html) に従います。

---

## [Unreleased]

### 追加

- **オプションの Program コンポーネント境界。** オプトインの
  `NEOGRAPH_BUILD_PROGRAM` スイッチ、エクスポートされる
  `neograph::program` ターゲット、`<neograph/program/program.h>` エントリ
  ポイントを追加しました。インストール済みパッケージのコンポーネント検出は
  Program をビルドした場合のみ報告し、Core 専用インストールは既存の
  `neograph::core` リンクインターフェースを維持します。

- **不変 Program 値モデル。** 安定した型付き診断、深く所有される
  canonical JSON/C++ ビルダーの `ProgramSource` 入力、不変でコンテンツ
  アドレス可能な `ProgramBundle`/`ProgramVersion` 値、正規直列化、
  SHA-256 アルゴリズムタグ付き識別子、ソースマップ、import、厳格な
  バージョン付き保存値スキーマを追加しました。`neograph::program` は
  Core のみに依存するコンパイル済みエクスポートライブラリになりました。
  Bundle/version v1 投影では、封印済み Core 定義とプラン識別子、セマンティック
  バージョン付き実行項目ダイジェスト、契約、クロージャ、境界、型付きの承認・
  具現化レシートを必須にしました。識別子は形式と保存バージョンを含み、意味的
  集合は安定順に正規化されます。診断は不正なポインター、逆順 span、不明 enum
  を拒否し、正確なパーサーオフセットがない場合は span を空のままにします。

- **封印された Program アドミッションクロージャ。** ビルダー時の callable
  取り込み、厳格な正規マニフェスト、ドメイン分離フィンガープリントを備えた
  不変の `RegistrySnapshot`、`AdmissionProfile`、`PolicySnapshot` を追加し、
  `ProgramVersion` でフィンガープリント間の整合性を fail-closed で検証します。
  Core には Program 具現化用の明示的なローカル専用 parse/link/validate
  エントリポイントを追加し、既存のローカル優先/グローバルフォールバック
  オーバーロードは変更していません。
  レジストリエントリは推移的な admission closure 用の正確な実行可能オブジェクト
  依存エッジを正規形で記録し、ローカル専用の条件検査はプロセスグローバル
  レジストリを参照せずにレガシーのキー付き edge 文書も処理します。

- **単一ルート `call_core` Program コンパイラー。** 閉じた Program-v1
  エンベロープのみを受理し、封印前にローカル専用 Core
  parse/round-trip/validation を純粋に実行し、RFC 6901 ポインターと
  ソースマップ帰属を持つ集約型診断を返す `ProgramCompiler` を追加しました。
  factory や callable を実行せず、canonical Program、レジストリ、推移的な
  実行項目クロージャ、capability/effect、import Merkle、封印定義、Core
  プラン識別子を決定論的に導出します。作成文書スキーマ、完全な有限予算契約、
  zero-dispatch 拒否テスト、静的・共有インストールコンシューマー検証も追加。
  Core には total な parse/round-trip とローカル validation レポートを追加し、
  既存の例外送出 API の動作は維持します。

- **固定 Program ランタイムの垂直スライス。** `ProgramCatalog`、
  `EngineGenerationCache`、`ProgramRuntime`、共有 `ProgramHandle`、不変
  `ProgramResult`、型付き Program イベントエンベロープ、インメモリ
  `ProgramStore`、追記専用 CAS `ProgramJournal` を追加しました。Admission
  は具現化前に未信頼バンドルの意味を再計算し、各 attempt は一つの不変 Core
  generation を pin して既存の `GraphEngine` 非同期経路だけを呼び出します。
  完了、中断、厳密な checkpoint resume、cancel、timeout、Core step 枯渇、
  checkpoint 非互換、失敗を、非更新予算と checkpoint lineage を保つ型付き
  terminal state に写像します。Journal commit は checkpoint/terminal
  event 配信より先に行われ、同時 resume は一つの CAS 勝者だけを許可し、
  Core broker が用意されるまで effectful または非空 schema の Program を
  拒否します。

- **QuickJS 制御言語フロントエンド。** オプトインの
  `NEOGRAPH_BUILD_QUICKJS_CONTROL`、封印された
  `ProgramSource::from_javascript(...)`、および非公開のコンパイル専用
  QuickJS コンテキストを追加しました。ソースエンベロープはエンジン/言語/
  ホスト API バージョンを固定し、唯一の `ng` ホスト表面はバージョン付きの
  グラフビルダーです。メモリ、スタック、割り込みポーリング上限は
  fail-closed で処理されます。JavaScript は不変の `call_core` Program
  プラン一つだけを生成し、ランタイム VM、バイトコードアーティファクト、
  Core 依存にはなりません。

- **A2A Agent Card 互換候補。** 認証なし・リダイレクト非追従の well-known
  カードを一度だけ収集するコレクターと、factory-only の不変候補コンパイラーを
  追加しました。候補はダイジェストに固定された provenance、境界付き
  プロトコル事実、安全な skill ID のみを保持し、free-form カードテキスト、
  広告された RPC endpoint、provider/security 設定、credential を除外します。
  Copy Ninja PoC は同じダイジェストに固定された独立観測行動を追加で要求し、
  ソース agent を dispatch しません。

- **SQLite Harness レコードストア (issue #147 フォローアップ)。** オプションの
  `neograph::mcp_sqlite` ターゲットと `SqliteHarnessRecordStore` を追加。WAL バック、
  スキーマバージョン管理されたアーティファクト/実行の永続化で、不変のアーティファクトと
  実行-アーティファクトバインディングを提供。Harness MCP バイナリはレコードを
  `runs.db` に保存し、チェックポイントは `checkpoints.db` に残ります。
- **AMD OpenMP GPU オフロードの概念実証。** 同じ数値ファンアウト処理で、
  直列 CPU、OpenMP 自動スレッド化、反復ごとの GPU マッピング、GPU 上に
  データを保持する実行を比較するオプションの `bench_openmp_offload`
  ベンチマークを追加しました。実デバイス実行とホストフォールバック、
  正確性、転送込みレイテンシ、カーネルのみのレイテンシ、直列 CPU 比の
  高速化を個別に報告します。Radeon AI PRO R9700 では
  `NEOGRAPH_OPENMP_OFFLOAD_ARCH=gfx1201` で ROCm/Clang デバイスイメージを
  有効化します。


### 変更

- **C++ ABI と SOVERSION ポリシー (issue #194)。** 全ての公開
  `neograph_*` バイナリライブラリにプロジェクト `VERSION` とメジャー
  `SOVERSION` を設定し、インストール済み共有ライブラリは同じディレクトリの
  依存ライブラリを解決します。v1 前は ABI 世代 0 ですが、必須再ビルド
  境界を告知できます。bounded `NodeCache` を含むリリースでは
  `NodeCache` と `EngineConfig` の公開レイアウトが変わるため、`0.11.1`
  以下でビルドした全 C++ コンシューマーの再ビルドが必要です。1.0 は ABI
  世代を 1 に変更して v1 レイアウトを固定します。CI は隔離した静的・共有
  インストールコンシューマーを実行し、ELF/Mach-O メタデータも検査します。
  詳細は [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照してください。
- **`GraphNode::run(input)` 移行ガイド完了。** Python `GraphNode` 基底クラスは
  削除された `execute*` メソッドを参照しなくなり、`run(input)` がない場合は
  移行ドキュメントパスを含む `NotImplementedError` を発生させます。
  C++/Python リファレンス、非同期/ストリーミングガイド、サンプル README は
  実際の v0.9.0 単一エントリポイントに合わせて調整されました。移行手順は
  [`docs/migration-v0.4-to-v1.0.md`](docs/migration-v0.4-to-v1.0.md) に
  C++ と Python の例付きで文書化されています。
- **Provider API 恒久的互換性ポリシー (issue #5)。** `Provider::complete()`、
  `complete_async()`、`complete_stream()`、`complete_stream_async()`、
  およびコールバックベースの `invoke()` の削除計画は撤回され、
  `[[deprecated]]` 警告も削除されました。既存 API は引き続き互換性と
  セキュリティ修正を受けます。新しい Provider 実装と直接呼出元は
  `CompletionProvider::do_invoke()` と `invoke_request(CompletionRequest)` の
  使用を推奨。既存 API への全新機能のバックポートは保証されません。
  公開シグネチャ、仮想メソッド順序、オブジェクトサイズ、vtable は変更なし。

### 削除

- **非推奨 TransformerCPP 統合サンプル。** `example_inproc_gemma`、
  `NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE`、`TRANSFORMERCPP_DIR` を削除。
  これらはもはや利用できない外部ホストリポジトリに依存していました。
  標準の OpenAI 互換ローカルサーバーを使用する `example_local_transformer` は保持。

### 修正

- **Harness 集約所見の出所 (issue #174)。** 詳細に既存のフラット `findings` 配列と
  整列した `finding_sources` 配列を含めるようになりました。各エントリは集約
  インデックス、ソースワーカー ID、ワーカーローカルインデックスを記録し、
  スキーマ検証されたワーカー出力や確立された `findings` 形状は変更しません。
- **Harness エクスポート結果 lint (issue #173)。** ノード効果契約が、呼出元が
  グラフ実行後に消費する場合にオプションの `exports` 配列で書き込みチャネルを
  宣言できるようになりました。Harness コンパイルと `GraphEngine` 実行時検証の
  両方で、真に書き込み専用のチャネルに対して E6 を保持しつつ `final_result` で
  誤警告しなくなりました。
- **MCP 2025-11-25 tool-client 契約の現代化 (issue #147 M0)。** 初期化が
  冪等になり交渉済みサーバーメタデータを保持。HTTP ツールが発見セッションを
  再利用。`/mcp` エンドポイント構築がリクエストと通知で共有。ツール発見が
  不透明カーソルに従う。JSON-RPC code/data、完全なツールメタデータ、
  非テキストコンテンツ、`structuredContent`、`isError`、`_meta` が
  C++ と Python パスで生存。設定可能な HTTP タイムアウト/静的/動的ヘッダ、
  出力スキーマ検証、厳密な応答 ID チェック、型付き `InitializeResult`、
  `ToolDefinition`、`ListToolsPage`、`CallToolResult` API を追加。
  SSE 検出が JSON 内の `data:` URL を誤分類する代わりに `Content-Type` を使用するように。
- **タスク毎キャンセル状態と published-emit ライフタイム安全性。**
  `GraphEngine::run`、`run_async`、`run_stream`、`run_stream_async` は
  呼出元提供の親から実行ごとに 1 つの実行子を作成し、その子のみを内部
  `co_spawn`/同期ブリッジにバインドし、同じ子を `RunContext` として渡します。
  したがって、単一の親の下での複数同時実行のキャンセルが互いのキャンセル
  スロットを上書きすることはありません。フォークされた実行子は公開 emit を
  通じて既存の `shared_ptr` 所有権を保持し、エンジン作業完了と emit 実行の間の
  use-after-free を防止。キャンセルによる asio `operation_aborted` は
  リトライ可能なノードエラーではなく `CancelledException` として伝播。
  `CancelToken` 0.11.x オブジェクトレイアウトとインライン/ヘッダオンリー動作は
  変更なし。既にコンパイルされた C++ 利用者は更新された `fork()` ライフタイム
  動作を受け取るために再コンパイルが必要。共有ライブラリのみの置換はオブジェクト
  レイアウト互換性を保持するが、利用者バイナリに埋め込まれた既存の
  インライン関数本体は変更されない。ただし、外部コードが自身で作成した
  トークンに `bind_executor()` を呼び出す場合、呼出元はエグゼキュータの
  post された作業が完了するまでトークンを生存させ続ける責任を負う。
- **PostgreSQL 非同期接続グローバルタイムアウトポリシーの文書化。** 非同期初期
  接続と再接続は全ホスト/IP アドレスにわたる単一のタイムアウトを使用。
  正の接続文字列に直接書かれた明示的な `connect_timeout` は最小 2 秒で強制。
  未指定、ゼロ、負、環境変数/サービスファイルのみの値は運用上安全なデフォルト
  30 秒を使用。これは libpq のホスト毎同期タイムアウトと意図的に異なる。
  同期作成/置換動作は変更なし。
- **JARVIS モックビルド修正 (issue #130)。** 音声依存がない場合に `MicCapture` が
  不完全型のままであることによる `cookbook_jarvis` コンパイル失敗を修正。
  ASan CI がランナーのインストール済みパッケージに関わらず常にモック設定を
  ビルドするよう `NEOGRAPH_JARVIS_FORCE_MOCK` を追加。セッションランナーが
  実際の CMake 出力パスとスペシャリストターゲット名を使用し、既存の
  `demo_mcp_server.py` を正しく起動するように。
- **ノード失敗コンテキスト保持 (issue #123)。** C++ 実行エラーが元の
  `exception_ptr`、失敗ノード名、試行回数を含む `NodeExecutionError` として伝播。
  終端 `ERROR` イベントも同じコンテキストを記録。Python では元の例外オブジェクト、
  型、args、ユーザー属性、トレースバックがそのまま保持され、`.node_name` と
  `.attempts` 属性のみ追加。`NodeInterrupt`、キャンセル、メモリ不足例外は
  ラップされずに既存の制御フローに従う。

### 修正 (ドキュメント)

- **Provider クックブックから無視されるノード毎プロンプトを削除 (issue #116)。**
  ビルトイン `llm_call` が読み取らない `config.system` を使用してマルチロール動作を
  記述していた 3 つの Python サンプルを修正。各サンプルを `NodeContext.instructions` を
  使用する厳密な単一呼出グラフに書き直し、関連 README を実際の動作に合わせて調整。
- **予約済み `RunContext::deadline` ドキュメント修正 (issue #115)。**
  `deadline` と `trace_id` を使用可能な実行毎メタデータとして提示していた
  ドキュメントと Doxygen コメントを修正。これらは `RunConfig` 経由で設定できず、
  Python にも公開されていない。
- **`GraphNode::run` サンプルシグネチャ修正 (issue #129)。** パブリックヘッダの
  サンプルが (参照で) `const NodeInput&` を受け取っていたが、実際の値渡し仮想
  メソッドをオーバーライドできず、コルーチン引数ライフタイムに必要な値渡し
  契約をコンパイル時テストで固定。

### 追加

- **後方互換 Provider 移行パス。** 新しい `CompletionRequest` がコールバックの
  存在からストリーミングモードを分離。`CompletionProvider` が新しい実装に
  `do_invoke()` のみの記述を要求。既存の `Provider` vtable、4 つのレガシー
  仮想メソッド、コールバックベースの `invoke()`、Python `complete()` サブクラス
  契約は保持。

- **Python 永続化バックエンド** (#117) — `Store` と `CheckpointStore` が
  Python への C++ 仮想ディスパッチを持つ構築可能なサブクラスベースに。
  `StoreItem`、`CheckpointPhase`、`Checkpoint`、`PendingWrite` が
  JSON 形状のフィールドで公開。チェックポイント pending-write メソッドは
  オプションのまま。
- **Python 同期キャンセル** (#119) — Python 呼出元が `CancelToken` を構築し、
  `RunConfig.cancel_token` に割り当て、別スレッドから協調的に
  `engine.run()` を停止可能。

- **Python チェックポイント履歴** (#118) — `GraphEngine.get_state_history()` が
  最新順のチェックポイントレコードを公開し、呼出元が履歴状態からフォークする前に
  親リンク、メタデータ、ステップ、ID を検査可能。

- **DSL サーフェス (精緻化層) + スキーマ進化ゲート** (#75 M4)。
  - **精緻化器**: `vars` (`{"$var":...}` / `${...}` 補間、非巡回強制) /
    `templates`+`use` (正確なパラメータ一致強制、ノードプレフィックス
    名前変更 — ローカル参照、バリア、ルートを含む。チャネルは共有状態のため
    グローバルにマージ) / `when` 条件付き包含。
    **非チューリング完全かつ全域的**: すべての DSL ドキュメントが有限時間で
    一意のコアに正規化され、そのコアに関して冪等。すべてのエラーは
    DSL ソース座標 (`use[2].args`、`vars.model`) とソースマップ
    (出力位置 → 生成構文) 付きで報告。ロックファイルワークフロー:
    `./example_elaborate harness.dsl.json > harness.json` (サンプル 53)。
  - **`GraphCompiler::upgrade_to_latest()`**: ロスレス v0→v1 機械的変換 —
    厳格が拒否するキーは `x-upgraded-<key>` コメント名前空間に隔離
    (ゼロデータ削除)。空バリアは明示的に除去。コーパス全体がテストされ、
    "レガシー寛容コンパイル IR == アップグレード後厳格コンパイル IR" を保証
    (正準同値、バージョンスタンプを除く)。
  - **スキーマ進化ゲート**: `tests/fixtures/schema_snapshot.json` ベースラインに
    対する追加専用サブセット判定 (JSON Subschema ファミリーの決定可能サブセット) —
    ノード型/プロパティ/リデューサ/条件の削除、required-set 増加、閉条件ラベル変更、
    効果契約変更はすべてテスト失敗 = CI マージブロック。非互換変更はバージョン
    バンプ + アップグレーダ + スナップショット再生成を同じレビューコミットで強制。

- **PBT / デルタ検証ハーネス** (#75 M3)。300 シード決定論的トポロジジェネレータ
  (スキーマエンベロープからの有効な厳格ドキュメント、自己計装された機能カバレッジ —
  conditional_edges/barrier/interrupt の出現率が 30% を下回るとテスト失敗:
  未テスト機能が失敗になり、暗黙の穴にならない)。
  - **変異検出**: 300 シードコーパスで、翻訳検証が 5 つの全ドロップ型
    (conditional_edges/edge/barrier/interrupt/channel) + 3 つの誤配線型
    (ルート折りたたみ / エッジ再ターゲット / ノード名前変更 = ドロップ+捏造
    カウンターバランス) の全適用を捕捉することを確認。適用率下限 (シードの 10%)
    も表明。
  - **参照インタプリタデルタ**: 文書化されたスーパーステップセマンティクス
    (goto プリエンプション、バリア蓄積、辞書式フォールバック、暗黙の __end__) を
    コード分離された実装から再実装した独立モデルを、Scheduler と 12 ステップ ×
    300 グラフで比較 (DESIL の教訓: 検証器だけでは誤実行を捕捉できない)。
  - **エンジン ↔ Studio 共有コーパス**: `tests/fixtures/topology_corpus/` の 15
    バリアント (3 有効 + 12 E3–E11 違反) が NeoGraph-Studio の
    `tests/corpus/` とバイト単位で同一で、両者が同じ判定 (code:severity
    マルチセット) を表明 — 2 つの実装が暗黙に分岐できない。

- **GraphValidator — トポロジ静的意味チェック (E3–E11 + 効果)** (#75 M2)。
  パース (M1) と実行の間のパス層。厳格ドキュメント (schema_version>=1) では
  エラーはコンパイル失敗、警告は stderr lint。寛容ドキュメントではエラーレベル
  診断のみが stderr 警告として表面化 (既存グラフでゼロノイズ)。判定哲学 =
  チェッカー健全性優先: エンジン意味論下で決して正しくありえないもののみが
  エラー (ダングリング参照 E3、シグナルパスのないバリア E8 — goto がバリア
  会計をバイパスするため回復不能、空ルート E10 — ディスパッチが rend() を
  逆参照する UB、未宣言チャネル書き込み E4 — 実行時スロー確認)。
  Command.goto/Send が正当化できるものは警告 (到達可能性 E7、脱出不能サイクル
  E11、バリアなしプレーンファンイン E9、上書き競合 E5、デッドチャネル E6)。
  すべての診断に機械可読な反証 (counterexample) JSON が付随 — Studio キャンバス
  ハイライト用 (M3)。
  - **ルート完全性 (E10)**: `ConditionSpec` ラベル契約導入。
    `register_condition` 3 引数オーバーロードによる条件の出力ラベル集合宣言は、
    閉条件ルートがラベルと正確に一致することを要求 — カバーされないラベルは
    スケジューラの「辞書式最終ルート」フォールバック (順序依存の任意ターゲット)
    に落ち、これはエラー。ビルトイン `has_tool_calls` = 閉 {false,true}、
    `route_channel` = 開 + 既知 {default}。
  - **チャネル効果契約**: `register_type` 4 引数オーバーロードがノード型毎の
    読み取り/書き込みチャネルを宣言。E4/E5/E6 解析はグラフ内の**すべての**
    ノード型が宣言されている場合にのみ有効化 (単一の未知型が解析全体をスキップ —
    カバレッジより健全性)。ビルトイン 3 型 (llm_call/tool_dispatch/intent_classifier)
    は完全に宣言済み。
  - `node_effects` · `condition_specs` を `export_schema()` に追加 (既存の
    `conditions` 配列は後方互換性のため保持)。22 新テスト。

- **トポロジコンパイル時一貫性ゲート — 消費キー会計 + 翻訳検証** (#75 M1)。
  「暗黙の意味損失」クラス (v0.1.0–v0.1.7 `conditional_edges` 暗黙ドロップと
  同種) を構造的にブロックする二重機構:
  - **消費キー会計**: `"schema_version": 1` を宣言するドキュメントは厳格
    コンパイルに切り替え — 未消費キー (タイポ `conditionnal_edges`、サポート
    されないフィールド、空の `wait_for` で暗黙ドロップされるバリア、インライン
    条件での無視される `to`) がすべて収集されコンパイルエラーとして報告。
    マーキングはパースブロック**内部**で発生するため、パース段階の消去は
    マークも消去し、それらの機能を使用する厳格ドキュメントが即座に失敗する —
    ドロップ回帰が暗黙になりえない構造。`_`/`x-` プレフィックスキー
    (`_comment`、`x-studio-*`) はコメント名前空間として常に許可。`schema_version`
    のない既存ドキュメントは寛容動作を保持 (バイト保存)。
  - **翻訳検証**: 全コンパイルで `CompiledGraph::to_json()` 再出力 +
    `GraphCompiler::canon()` 正規形チェック `canon(input) == canon(re-emit)`。
    不一致 (= コンパイラが何かをドロップまたは誤配線) は厳格ドキュメントで
    スロー、寛容ドキュメントで stderr 警告。同値性は構造比較 — スワップされた
    ルートキーのような誤配線も捕捉 (存在比較が見逃すクラス)。
  - `NodeFactory::config_schema(type)` クエリ追加、`schema_version` フィールドを
    `export_schema()` に文書化。27 新テスト (`tests/test_compiler_strict.cpp`)
    — v0.1.x ドロップ変異シミュレーション (conditional_edges/barrier/interrupt
    ドロップ + ルート誤配線) を含む。

## [0.11.1] - 2026-06-25

### 変更

- **stdio MCP 同時呼出 — I/O オーバーラップのための相関 ID デマルチプレクサ。**
  `0.11.0` の同時ツールディスパッチは実際には HTTP MCP のみをオーバーラップさせて
  いた。stdio MCP は `StdioSession::rpc_call_async` で**リクエスト→応答往復全体**に
  対して容量 1 のチャネルロックを保持し、単一セッションパイプを通じて 1 ターン内の
  複数呼出を直列化していた (wall time ≈ レイテンシの合計)。単一パイプは根本原因
  ではなく — JSON-RPC `id` はまさに 1 接続上でパイプライン化するために存在する。
  ロックを相関 ID デマルチプレクサに置換:
  - 容量 1 チャネルを**書き込み専用ロック**として転用 — フレーム書き込みの瞬間のみ
    保持され、2 つの呼出のバイトがインターリーブせず、読み取りは直列化されない。
  - 単一のリーダーコルーチン (`run_reader`) が読み取り側を排他的に所有し、
    各応答行を JSON-RPC `id` 経由で正しい呼出元のシンクに配信。N 同時呼出が
    読み取りをオーバーラップさせるため wall time ≈ max(latency) — ただし
    **ピア MCP サーバーが並行処理する場合のみ** (シングルスレッド逐次サーバーは
    Amdahl の下限に達する)。
  - リーダーは実行中呼出が存在する間のみ遅延実行され、待機者が空になると終了する
    ため、プライベート `run_sync` io_context が正常に返る。待機者は呼出元が
    await している間のみ存在し、`MCPTool` の `shared_ptr` 経由でセッションを
    生存させ続けるため、リーダーが破壊されたセッションに触れることはない
    (デストラクタ join 不要)。パイプ EOF/エラー時にリーダーは全シンクを閉じ、
    await 中の呼出元は無期限ハングの代わりに例外を受け取る。
  - **API/構文変更なし** — 公開ヘッダ変更なし、既存コードの再コンパイル不要。
    エンジンオーバーヘッド回帰 0 (`bench_neograph` 交互 A/B、seq/par Δ 0%)。
  - テスト: スレッドベース遅延フィクスチャ `tests/fixtures/mcp_stdio_slow.py` +
    `ConcurrentStdioCallsOverlapIO` (5×100 ms 呼出が ~130 ms で完了 vs.
    500 ms 直列下限。各応答が `id` 経由で呼出元にルーティングされることを検証)。
    ASan+UBSan ×3 クリーン。

## [0.11.0] - 2026-06-25

### 追加

- **同時ツールディスパッチ — `Tool::execute_async` 公式非同期パス。**
  `ToolDispatchNode` がエンジンの `make_parallel_group` を使用して単一の
  アシスタントターンからの複数の `tool_call` を**同時に**実行するように。
  以前は各呼出が同期 `execute()` で逐次実行され、特に MCP ツールは呼出ごとに
  `run_sync` 経由で独自の `io_context` を生成するためブロックし、並列 MCP
  呼出のオーバーラップを妨げていた (並列 MCP 呼出のある外部 C++ フォークで
  発見)。修正:
  - 仮想 `execute_async()` を `Tool` に追加 — デフォルト実装が同期
    `execute()` にブリッジするため、既存ツールは変更なく動作。
  - `MCPTool` をネイティブ `execute_async` を持つ `AsyncTool` に変換
    (stdio は `rpc_call_async` 使用、HTTP は新しい `MCPClient::initialize_async`/
    `call_tool_async` で非同期ハンドシェイク — `run_sync` 削除)。
  - `ToolDispatchNode::run` がノードファンアウトと同じ `make_parallel_group`
    イディオムで呼出を同時ディスパッチ (単一呼出はインライン化)、結果を呼出順に
    適用。同期 `execute()` ファサード経由で後方互換。
  - 検証: 478/478 ctest、Valgrind 0 リーク、TSAN 0 競合。

### 修正

- **Python 非同期実行の例外保持 (issue #122)。**
  `run_async`、`run_stream_async`、`resume_async` が元の Python ノード例外を
  文字列としてラップする新しい `RuntimeError` で上書きしていたのを修正。
  元の Python 例外オブジェクト、型、ユーザー属性、トレースバックが
  pybind11 の標準例外変換パスを通じて保持され、C++ の `py::type_error` が
  同期実行と一致する Python `TypeError` として配信されるように。
  `resume_async` の空コールバックがコルーチン完了まで保持され、pybind11 3.x で
  露呈したダングリング参照競合も修正。

### 修正 (ドキュメント)

- **README サマリーバッジを欠落条件と sandbox 測定で明らかになった内部矛盾に
  合わせて修正。** 「4 つの軸」サマリーテーブルのバッジが本文/詳細の測定条件を
  剥ぎ取り、誇張に見えていた。本文の測定数値と条件に合わせて修正 (測定データ
  テーブル自体は変更なし):
  - **`p99 17 µs flat` → `p99 7 µs @ 10 K (1 CPU sandbox)`** — バッジの
    17 µs は本文 (`At N=10,000 concurrent ... 7 µs p99`) と矛盾し、
    `flat` は µs 測定ではなく GPU バウンド負荷テストの実行レイテンシ (648 ms) を
    記述していた。バッジを本文の測定数値と条件に合わせて修正。
  - **`1.2 MB 削除済みバイナリ` → `... (MinSizeRel static)`** — `libc.so.6` のみ
    と 1.2 MB は MinSizeRel + 静的 libstdc++ ビルドでのみ成立 (デフォルト
    Release は libstdc++/libgcc_s/libm/libc を動的リンク)。条件は詳細 §サイズ
    で既に文書化済み、バッジに復元。
  - **`2 wheel deps` → `2 direct wheel deps (... ; 7 with transitive)`** —
    直接依存は確かに `certifi` + `pydantic` (2 つ) だが、実際のインストール
    ツリーは pydantic 推移的依存 (pydantic-core、typing-extensions、
    annotated-types、typing-inspection) を含む 7 パッケージ。
- **詳細 MinSizeRel 再現コマンドに `-DNEOGRAPH_BUILD_POSTGRES=OFF` を追加。**
  PostgreSQL はデフォルト ON のため、libpq のないホストでそのまま実行すると
  configure が失敗する。修正。

## [0.10.0] — 2026-05-20

### 追加

- **直列ファンアウト ワンショット stderr 警告 (issue #62, PR #63)。**
  `compile()` のデフォルトは `set_worker_count(1)` — ファンアウト分岐は
  エンジン所有のスレッドプールなしで呼出元のエグゼキュータ上で逐次実行される。
  この意図された動作はドキュメントのみに基づいてマルチ Send グラフを構築した
  ユーザーには暗黙の逐次実行に見える。`NodeExecutor` がプールなしでマルチ Send
  (またはマルチ出力エッジ) ファンアウトをディスパッチする最初の回に
  ワンショットのガイダンスメッセージを stderr に追加。`std::atomic` +
  compare-exchange が同時ファンアウト下でも正確に 1 回の出力を保証。
  `set_worker_count(N>=2)` の呼出は `NodeExecutor` を再構築し、自然に
  フラグをリセット。環境変数 `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1` (または
  `true` / `yes`) で抑制可能 — 意図的な worker=1 逐次実行、ベンチマーク、
  CI stderr 表明ケース用。5 Linux + macOS ユニットテストでカバー
  (`test_fanout_worker_warning.py`): fire / ワンショット / pool opt-in silence /
  env-var silence / single-Send no-warning。Windows: pytest capfd は wheel
  バイナリの MSVC CRT fd キャッシングと非互換のためモジュールレベルスキップ —
  wheel バイナリ stderr 出力自体は正常。

- **トポロジ JSON スキーマエクスポート — `NodeFactory::export_schema()`**
  (issue #56、コード不要ビジュアルブロックエディタの前提条件)。エンジンが
  消費するトポロジ JSON 形式を機械可読スキーマ (JSON Schema Draft 2020-12) として
  1 つに出力: `{ neograph_version, $schema, topology (固定エンベロープ),
  node_types, reducers, conditions }`。別リポジトリのブロックエディタがこの
  スキーマからパレットを自動生成 → エディタとエンジンがバージョン間で
  ドリフト不可能。完全に追加的:
    - `NodeFactory::register_type(type, fn, json config_schema)` 3 引数
      バリアント追加。既存の 2 引数は寛容なデフォルトスキーマに委譲 —
      既存のユーザーノード/呼出は影響なし。
    - `ReducerRegistry::names()` / `ConditionRegistry::names()` /
      `NodeFactory::registered_types()` クエリアクセサ追加。
    - 4 ビルトイン型 (`llm_call`/`tool_dispatch`/`intent_classifier`/
      `subgraph`) に設定スキーマ宣言。`NEOGRAPH_VERSION` をコンパイル定義として
      公開 (pyproject.toml 単一真実源) → スキーマバージョンスタンプ。
    - `examples/52_export_schema.cpp` (`example_export_schema`):
      `./example_export_schema > schema.json` — エディタリポジトリ CI が
      NeoGraph バージョンに固定されたアーティファクトを生成する標準パス。
    - Python: `neograph_engine.export_schema()` → dict (エディタリポジトリ CI が
      `pip install neograph-engine` 後にダンプ)。
    - `tests/test_schema_export.cpp` 8 + `test_export_schema.py` 4。キー:
      トップレベル `conditional_edges` がローダー→コンパイル往復を生存
      (v0.1.0–v0.1.7 暗黙ドロップ再発に対する回帰ガード)。

### 修正

- **トポロジトップレベルコンテナ形式検証 (#126)。** `channels`/`nodes` は
  オブジェクトでなければならない。全モードでオブジェクトでない場合拒否。
  `edges`/`conditional_edges` 配列検証を厳格モードで強制、レガシーキー付き
  エッジマップ互換性を保持。エラーはパスと JSON 型を記録し、入力全体は
  記録しない。
- **`max_steps` 終端状態公開 (#114)。**
  `RunResult::max_steps_exhausted()` と読み取り専用 Python プロパティ
  `RunResult.max_steps_exhausted` 追加。実行すべきノードが残っている状態で
  `max_steps` に達した場合のみ True。同じ状態が gRPC 単一応答とストリーミング
  最終 JSON で提供。C++ 構造体サイズは変更なし。

- **`set_worker_count` / `set_worker_count_auto` docstring 修正
  (issue #62, PR #63)。** v1.0 準備サイクルが意図的に `compile()` ワーカー
  プールデフォルトを `set_worker_count(hardware_concurrency())` から
  `set_worker_count(1)` に戻したが (`src/core/graph_engine.cpp:69-93` コメント参照)、
  4 つのユーザー向け docstring が古い主張を保持 → ドキュメントを信頼して
  マルチ Send ファンアウトグラフを構築したユーザーが単一スレッドで暗黙の
  逐次実行を受けた。ユニットテストでは不可視 (フェイク spawn、インスタント本体)。
  実際の wall-time e2e でのみ露呈。
  - `bindings/python/src/bind_graph.cpp` の両 `set_worker_count` /
    `set_worker_count_auto` Python docstring を実際の動作に合わせて書き直し:
    `compile()` デフォルトは 1、`set_worker_count_auto()` /
    `set_worker_count(N>=2)` は明示的オプトイン。
  - `include/neograph/graph/engine.h` の両 Doxygen コメントも同様に修正。
    Doxygen Pages は master push で自動再構築。
  - `docs/concepts.md` / `docs/troubleshooting.md` / `docs/reference-en.md` の
    同じ古い主張 (デフォルト = hardware_concurrency) も修正。

- **v0.9.0 出荷で不足していた 3 つの API 移行を補充。** v1.0 準備サイクルの
  PR `9b` (`19819d8`) が `GraphNode` レガシー 8 仮想チェーンを破壊的に削除したが、
  PR `#48` (`6e654ad`、"C++ examples migrate to `GraphNode::run()`") は
  `examples/` のみを移行 — 以下の 3 ファイルが見逃され、v0.9.0 がビルド破損
  状態で出荷された:
    - `benchmarks/stress/bench_sustained_concurrent.cpp` (Phase 3 持続バースト
      検証のキーベンチマーク)
    - `benchmarks/concurrent/bench_concurrent_neograph.cpp` (LangGraph や他
      エンジンとのメモリ/並行性比較マトリックス本体)
    - `wasm/smoke.cpp` (Phase 1 WASM 実現性スモーク)

  CI はこれらのターゲットを add_executables として拾っておらず、(Docker ビルド
  依存が) 別環境に隔離していたため、master へのマージとタグは通過。

  **修正**: 3 つすべてを `std::vector<ChannelWrite> execute(const
  GraphState&) override` → `asio::awaitable<NodeOutput> run(NodeInput in)
  override` + `co_return out` パターンに移行。ノードロジックは変更なし。

  **v1.0 キーセールスポイントのネイティブ再検証**
  (`benchmarks/concurrent/results_v0.9.0_native_recheck.jsonl`):
    - 同時実行数 10K · wall 10–23 ms · p99 17–21 µs · ピーク RSS **5.6 MB**
      (v0.3.0 / v0.5.0 測定と一致 — 破壊的 9b 後もメモリセールスポイントの回帰なし)
    - 10K で 0 エラー
  **Docker マトリックス (LangGraph / Haystack / pydantic-graph / LlamaIndex /
  AutoGen 6 方向比較) も同じセッション内で再測定**
  (`results_v0.9.0_docker_recheck.jsonl`)。

  マトリックス再実行中に、不足 API 移行と並んで 1 つの独立した回帰を発見 —
  `benchmarks/concurrent/Dockerfile.neograph` が master 上の CMake オプション
  デフォルト変更を追跡できず、まったくビルドできなかった (v0.9.0 出荷時点で
  同様)。以下のオプションデフォルトが時間とともに OFF → ON に反転:
    - `NEOGRAPH_BUILD_POSTGRES` / `NEOGRAPH_BUILD_SQLITE`
      (それぞれ `libpq-dev` / `libsqlite3-dev` が必要)
    - `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP`
    - `NEOGRAPH_USE_LIBCURL` (以前のインシデント 1 件が
      `feedback_libcurl_unconditional_dep.md` でクローズされていた —
      オプショントグルのみ追加されデフォルトは ON のままで、空コンテナビルド
      パスが再度破損)
    - `find_package(OpenSSL REQUIRED)` はオプショントグルなしで無条件
      (CMakeLists.txt:256) — 別の v1.0 クリーンアップ候補

  **Dockerfile 修正**: `libssl-dev` apt 追加 + 全非コアオプションを明示的
  `-DNEOGRAPH_BUILD_*=OFF` / `-DNEOGRAPH_USE_LIBCURL=OFF` で固定。
  コメントに「2 回のドリフトインシデントのため明示的凍結」と注記。
  `find_package(OpenSSL REQUIRED)` の CMakeLists.txt での条件付き化は別タスク
  として残す — 他のビルドパス (PyPI wheel、ARM64 等) への影響検証が必要。

  **6 方向マトリックス主要結果** (concurrency=10000, 2 cpus / 1 GiB):

  | エンジン         | モード        | wall_ms | p99_us      | peak_MB | ok/err |
  |---|---|---|---|---|---|
  | **neograph**    | threadpool    | **16**  | **18**      | **5.1** | 10000/0 |
  | pydantic-graph  | asyncio       | 895     | 160         | 42.8    | 10000/0 |
  | haystack        | mp-pool-8     | 1472    | 2972        | 68.3    | 10000/0 |
  | langgraph       | mp-pool-8     | 3802    | 74415       | 60.6    | 10000/0 |
  | autogen         | mp-pool-8     | 22428   | 82361       | 49.1    | 10000/0 |
  | llamaindex      | asyncio       | 26303   | 25912204    | 582.7   | 10000/0 |

  NG vs LangGraph (マーケティング比較軸): wall **237× 高速**、p99
  **4134× 高速**、ピーク RSS **12× 低**。

  **厳しいシナリオ** (concurrency=10000, 1 cpu / 512 MiB):
    - NG: 8 ms / 5.2 MB / 0 err / **ok**
    - LangGraph mp-pool-8: 7821 ms / 60.9 MB / 0 err / ok
    - **LlamaIndex asyncio: OOM キル** (512 MB 上限超過)
    - **AutoGen asyncio: OOM キル**

  同じ v0.3.0 / v0.5.0 測定 — **破壊的 9b 後も NeoGraph の「10K 同時ワーカー、
  ピーク RSS 5 MB、OOM なし」セールスポイントに回帰なし。**

## [0.9.0] — 2026-05-14 — v1.0 準備 (Candidate 1 Phase B + Candidate 6)

ROADMAP_v1.md からの 2 つの v1.0 単一ディスパッチ統一が 1 サイクルで合流:

  - **Candidate 1 Phase B (`9b`–`9f`)** — `GraphNode` のレガシー 8 仮想
    メソッドすべて (`execute` / `execute_async` / `execute_stream` /
    `execute_stream_async` / `execute_full` / `execute_full_async` /
    `execute_full_stream` / `execute_full_stream_async`) +
    `add_cancel_hook` + `CurrentCancelTokenScope` + `state.
    run_cancel_token_` + 全 6 `PyGraphNodeOwner` レガシーオーバーライドを削除。
    **破壊的** — 非推奨期間終了。ユーザー GraphNode サブクラス /
    ユーザー Python ノードは単一メソッド `run(NodeInput)` /
    `def run(self, input)` に移行必須。
  - **Candidate 6** — `Provider` 4 仮想直積 → 1 仮想 `invoke()`。
    追加 + 非推奨フェーズにあり — レガシー 4 仮想メソッドは変更なしで機能、
    非推奨警告のみ可視。その側の Phase B (`Provider` レガシー削除) も
    v1.0.0 出荷直前にクローズ。

同じサイクルに b59444f の潜在的な並列回帰 revert (`e5ecb08`) + 明示的
ファンアウトサンプル呼出 + 3 CI 環境修正 (httplib マクロガード / Windows MSVC
unistd.h / pybind pytest 移行) も含まれ、すべてこの [Unreleased] の一部。

### 追加

- **`Provider::invoke(params, on_chunk = nullptr)`** — v1.0 標準単一ディスパッチ
  エントリポイント。非ストリーミング (`on_chunk == nullptr`) とストリーミング
  (`on_chunk` 提供) の両方を 1 メソッドで処理。以前の 4 仮想直積 (`complete` /
  `complete_async` / `complete_stream` / `complete_stream_async`) を 1 つの
  非同期ストリーミングスーパーセットに統合。デフォルト実装が 4 レガシー仮想
  メソッドに転送するため、既存の Provider サブクラスは変更なく動作。
  6 新 ctest (`ProviderInvokeDefault`)。(PR #40)
- **`invoke()` キャンセル伝播パリティ** — `params.cancel_token` が設定されて
  おらず、エンジンのスレッドローカルスコープがアクティブな場合、
  `current_cancel_token()` が自動的に刻印。レガシー同期 `complete()` 動作と同等
  (エンジン内のノード本体が `provider->invoke(params, ...)` を呼び出すと、
  実行中グラフのキャンセルシグナルを自動的に受信)。3 新 ctest
  (`InvokeCancelPropagation`)。(PR #43)
### 変更

- **エンジン内の全内部 LLM 呼出が `invoke()` 経由にルーティング** — `LLMCallNode`、
  `IntentClassifierNode` (PR #41/#42)、`Agent::complete` /
  `Agent::run_stream` (PR #43)、`SupervisorLLMNode` /
  `ResearcherLLMNode` / `CompressNotesNode` / `FinalReportNode` (PR #43)、
  `PlannerNode` / `ExecutorNode` (PR #44)。NeoGraph 内の LLM ディスパッチが
  単一インターフェースに統一。
- **C++ サンプル移行 (2 ファイル)** — `31_local_transformer.cpp`、
  `cookbook/ai-assembly/member_server.cpp` が新しい `invoke()` 使用。
  ユーザービルドで非推奨警告なし。(PR #45)
- **`GraphEngine::compile()` デフォルトワーカー数を 1 に戻した** (`e5ecb08`)。
  `b59444f` が 18 日間 (2026-04-26 → 2026-05-13) 潜在していた並列マイクロ
  ベンチ回帰 11.8 → 283 µs (24×) の根本原因 — バイセクションでコミット特定
  (11 並列 worktree)。v1.0 から default=1 (CPU 微小な逐次/並列ディスパッチに
  最適)。意図的なファンアウトには 1 行 `engine->set_worker_count_auto()` を
  追加して hardware_concurrency を開く。影響を受けた 5 ファンアウトサンプル
  (10/14/21/36 + deep_research_graph ビルダー) に明示的呼出を追加。
  詳細は ROADMAP_v1.md の「Perf retrospective」セクション参照。

### 非推奨

- **`Provider::complete` / `complete_async` / `complete_stream` /
  `complete_stream_async`** — 4 つすべてのレガシー仮想メソッドが
  `[[deprecated("v1.0 single-dispatch: use invoke(...)")]]` マーカーを付与。
  レガシーメソッドは非推奨期間中そのまま動作。v1.0.0 で削除予定だったが
  撤回され警告も除去。内部転送は `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` で
  ラップされ、警告はユーザー向けオーバーライド/呼出サイトでのみ表示。(PR #44)

### 削除 (Candidate 1 Phase B — 破壊的)

- **`GraphNode` レガシー 8 仮想メソッド** — `execute(GraphState&)` /
  `execute_full(...)` / 6 バリアント + `ExecuteDefaultGuard` 再帰ガード
  + 300+ 行のデフォルトチェーン。すべて削除。`run(NodeInput)` が唯一の
  純粋仮想。(commit `19819d8`)
- **`add_cancel_hook` + `Hook` RAII + `hooks_*` メンバ + `cancel()` フック
  反復** — `cancel.h` は `fork()` + `cancel()` + `is_cancelled()` +
  `slot()` のみを保持。(commit `1d786a5`)
- **`CurrentCancelTokenScope` + `current_cancel_token()` thread_local +
  `GraphState::run_cancel_token_` + 3 アクセサ** — `RunContext::cancel_token`
  が唯一のキャンセルチャネル。`src/core/cancel.cpp` はスタブまで空に
  (ファイル自体が将来の削除候補)。(commit `9e8e956`)
- **6 `PyGraphNodeOwner` レガシーオーバーライド** — pybind トランポリン呼出が
   `run(self, input)` のみを呼ぶ。Python ユーザーコードも v0.9.0 から
  単一メソッドが必要。(commit `9e8e956`)
- **2 つの廃止 pytest ファイル** — `test_execute_stream_dispatch.py` (v0.3.2
  ストリーム専用フォールバックディスパッチ検証) + `test_streaming_only_error_
  hint.py` (execute_full_stream が優先 — v1.0 では無意味)。
  (commit `4392fbb`)

### 修正

- **5 ファンアウトサンプルに明示的呼出を追加** — `e5ecb08` のデフォルト
  ワーカー数復帰で埋もれた実際の並列意図を復元:
  `examples/10_send_command.cpp`、`examples/14_plan_executor.cpp`、
  `examples/21_mcp_fanout.cpp`、`examples/36_classifier_fanout.cpp`、
  `src/core/deep_research_graph.cpp` の `create_deep_research_graph()`
  ビルダーが `set_worker_count_auto()` を呼び出すように。
  検証: `classifier_fanout` 4.22× 高速化 (25.2 ms 逐次 → 6.0 ms 並列)。
  (commit `99c470b`)
- **`bench_async_http` httplib マクロガード** — `bench_async_http.cpp` が
  `<neograph/async/conn_pool.h>` 経由で `<httplib.h>` をインクルードするが
  `CPPHTTPLIB_OPENSSL_SUPPORT` が未定義で ODR ガードが拒否。
  CMake ターゲットに `target_compile_definitions(... PRIVATE ...)` を追加。
  (commit `d4be42a`)
- **Windows MSVC `unistd.h` 欠落** — `test_schema_provider_extra_
  fields_temperature.cpp` が POSIX 専用 `mkstemps` + `close` を使用し、
  Windows ビルドが完全に失敗。ファイル全体を `#ifndef _WIN32` ガードでラップ
  (カバレッジは Linux/macOS で保証)。(commit `3c49f12`)
- **16 Python テスト移行** — wheel CI pytest がレガシー `def execute(self, state)`
  パターンの 28 ノードクラスで `AttributeError` に当たった。
  `def run(self, input)` に一括移行。ストリーミングノードは
  `input.stream_cb` None ガードを追加。(commit `4392fbb`)

### 移行 (ユーザーコード)

**Provider 呼出 (Candidate 6 — 非推奨フェーズ)**

新コード:
```cpp
// non-streaming
auto completion = co_await provider->invoke(params, nullptr);

// streaming
auto completion = co_await provider->invoke(params, on_chunk);

// sync site (replaces old complete())
auto completion = neograph::async::run_sync(provider->invoke(params, nullptr));
```

4 レガシー仮想オーバーライドは非推奨期間中も動作し続ける (警告は撤回済み)。

**`GraphNode` サブクラス (Candidate 1 Phase B — 破壊的)**

C++ コード:
```cpp
// old (up to v0.8.x)
class MyNode : public GraphNode {
    NodeResult execute_full(const GraphState& state) override {
        auto x = state.get("x");
        NodeResult out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        return out;
    }
};

// v0.9.0+ current code (single method, coroutine entry)
class MyNode : public GraphNode {
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto x = in.state.get("x");
        // in.ctx.cancel_token / in.ctx.step / in.stream_cb also accessible
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        co_return out;
    }
};
```

Python コード:
```python
# old (up to v0.8.x)
class MyNode(neograph_engine.GraphNode):
    def execute(self, state):
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]

# v0.9.0+ current code
class MyNode(neograph_engine.GraphNode):
    def run(self, input):
        state = input.state  # input.ctx.cancel_token / input.stream_cb etc. also accessible
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]
```

**ファンアウト意図 (ワーカー数デフォルト変更)**

```cpp
// old (v0.x April+): default was hardware_concurrency but micro-bench burden
// v1.0: default=1. Add one line for intentional fan-out.
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // ← this line added (hardware_concurrency)
// or engine->set_worker_count(N);  // explicit N
```

移行 1/2/3 セクションは `docs/migration-v0.4-to-v1.0.md` (run() /
ctx.cancel_token / ワーカー数デフォルト) + Provider セクションに
ケースバイケースの before/after ガイダンスを提供。

## [0.8.0] — 2026-05-13 — DX ポリシー + 下流駆動 API ギャップ対応

実世界の下流 (ProjectDatePop) フィードバックと内部カバレッジ差分から表面化した
8 課題 (#22, #25, #26, #27, #28, #34, #35 + #16 フォローアップ) を 1 つの
マイナーバンプにバンドル。2 つの新しい公開ヘルパー
(`RunResult::channel<T>`、`RunContext::store`)、11 の新オフラインサンプル、
`docs/migration-v0.4-to-v1.0.md` 移行ガイド、新人が最初の 30 分で遭遇する
摩擦を減らす 5 項目の DX バンドル。

### 追加

- **`RunResult::channel<T>(name)` / `channel_raw(name)` / `has_channel(name)`** —
  結果からチャネル値を抽出するワンライナーヘルパー。両方の出力形状
  (ネストされた `output["channels"][name]["value"]` 標準 + `react_graph` のような
  ビルダーが追加するフラットキー) を自動処理。9 新 ctest。(Issue #25)
- **`RunContext::store`** — ノード本体が 1 行 `in.ctx.store->get(ns, key)` で
  Store に到達。古いパターン (`shared_ptr<Store>` を `NodeFactory` ラムダに
  捕捉) も引き続き動作 — 新コードは新しい形状のみ必要。
  3 新 ctest。(Issue #27)
- **`Provider::complete_stream` 非純粋デフォルト本体** — 最小限のモック/テスト
  フィクスチャが `complete()` のみのオーバーライドで済む。既存のストリーミグ
  ネイティブオーバーライドは変更なし。2 新 ctest。(Issue #22)
- **`neograph::json` 配列 `.front()` / `.back()`** — nlohmann 筋反射
  パターン (`msgs.back()["content"]`) がコンパイル可能に。4 新 ctest。(Issue #26)
- **11 の新オフラインサンプル (41-51)** — `resume_if_exists_chat`、
  `custm_reducer_condition`、`stre_personalization`、
  `request_queu_backpressure`、`canceltoken`、`ndecache`、
  `sqitecheckpint`、`openinference`、`asynctol`、`minimal`。すべて rc=0、
  API キー/外部サー ビス依存なし。以前に参照がゼロだった 27/53 `NEOGRAPH_API`
  クラスのギャップを埋める。
- **`examples/51_minmal.cpp`** — 30 行の導入サンプル、1  ノード、LLM なし、
  ツールなし、モックプロバイダなし。5 分未満で NeoGraph の動作を理解。
- **`docs/migration-v0.4-to-v1.0.md`** — ケースバイケース 4 例の before/after +
  `[[deprecated]]` 古い 8 仮想チェーン (`execute` / `execute_async` / etc.) →
  新しい `run(NodeInput) -> awaitable<NodeOutput>` からの移行時のよくある間違い。
  `NEOGRAPH_DEPRECATED_VIRTUAL` マクロメッセージからもリンク。
- **README「Common pitfalls 5」セクション** — 新人が最初の 30 分でハマる 5 つの
  こと (`channel<T>` 使用法、`in.ctx.store`、`neograph::graph::` サブ名前空間、
  `<httplib.h>` マクロ、GCC 13 コルーチン ICE) を 1 か所に。各項目に修正 +
  関連サンプル/課題リンク付き。
- **コンパイル時 `#error` ガード (`include/neograph/api.h`)** — ユーザー TU が
  `CPPHTTPLIB_OPENSSL_SUPPORT` なしで NeoGraph ヘッダより前に `<httplib.h>` を
  インクルードした場合、明確なメッセージ + オプトアウトマクロ
  (`NEOGRAPH_SKIP_HTTPLIB_MACRO_GUARD`) でコンパイル失敗。古い #16 実行時 SEGV を
  コンパイル時失敗に昇格。
- **`example_minimal` 5 つの新しいフレンドリーエラーメッセージ ctest** —
  `Unknown reducer` / `Unknown condition` / `Unknown node type` /
  `Write to unknown channel` メッセージに利用可能な名前 + 登録メソッド +
  トラブルシューティングリンクを埋め込む契約ロック。
- **`docs/troubleshooting.md` 4 新エントリ** — Tracer アダプタ `close()`
  ハング/クラッシュ (#24)、GCC 13 コルーチン ICE (#23)、フレンドリーエラーメッセージ
  ガイダンス (#22)、`RunResult::output` 形状 (#25)。
- **`Tracer` + `OpenInferenceTracerSession::close()` `@warning` ブロック** —
  アダプタ作成者向けに raw-pointer の落とし穴を明示的に文書化。正しいアプローチ
  として `RecordedSpan` + ラッパー分離パターンを指摘。既存の
  `tests/test_openinference_cpp.cpp::InMemoryTracer` + 新しい
  `examples/49_openinference.cpp::PrintTracer` を参照。(Issue #24)

### 修正

- **`SchemaProvider::build_body` が `params.tools` 空時に `extra_fields` を
  暗黙ドロップ。** 古いコードが `extra_fields` 適用を
  `if (!params.tools.empty())` 内にゲートしており、`reasoning` や
  `response_format` のようなコアスキーマフィールドがツールなし呼出から
  完全に消失していた。修正: ツール分岐の外に移動し常に適用。3 新 ctest。(Issue #34)
- **`temperature_path` スキーマ側オプトアウト。** 推論モデル (gpt-5.x、
  o-series) は `temperature` と `reasoning.effort` が相互排他的だが、
  スキーマに「このプロバイダは temperature を受け付けない」と宣言する手段がなく、
  全呼出で `params.temperature = -1.0f` のセンチネル回避策を強制していた。
  修正: スキーマに `"temperature_path": null` を指定すると build_body が完全に
  スキップ。4 新 ctest。(Issue #35)
- **フレンドリー RuntimeError メッセージ** — `ReducerRegistry::get` /
  `ConditionRegistry::get` / `NodeFactory::create` の "Unknown <thing>: foo"
  と `GraphState::write` / `apply_writes` の `Write to unknown channel` が
  利用可能な名前 + 登録メソッド + トラブルシューティングリンクをメッセージ本文に
  埋め込むように。新人がメッセージだけで次のステップを判断可能。
- **`SchemaProvider::complete_stream_async` HTTP/SSE 分岐** — 長寿命の専用
  `bridge_thread_` 上でディスパッチするように (旧: `Provider` 基底デフォルトが
  呼出ごとに新しい `std::thread` を生成)。古い動作はコールドスレッドローカル
  リゾルバ/NSS 状態で glibc `internal_strlen` の SEGV を引き起こしていた。
  WS 分岐は既にネイティブ co_await のため影響なし。awaiter のエグゼキュータでの
  トークンディスパッチは保持 (PR #10 の不変条件)。(Issue #16)
- **`example/09_all_features.cpp`** Store デモ — ノード本体読み取りパターンを
  `examples/43_store_personalization.cpp` に指し示す docstring ポインタを追加。
  オプション 2 — オプション 3 (インラインライブノード) は #27 の
  `RunContext::store` 着地後にまとめてクリーンアップ予定。(Issue #28)

### ドキュメント

- `RunResult::output` の正規形状 (channels-wrapped) と `react_graph` のような
  ビルダーが追加するフラットキー投影との関係をヘッダ docstring に文書化。
  新しいヘルパー (`channel<T>` / `channel_raw` / `has_channel`) の使用を推奨。
  (Issue #25)
- `RunContext::store` フィールド `@brief` ブロック — 2 つの配管パターン
  (`in.ctx.store` 推奨 / 古いファクトリクロージャキャプチャ互換) をコード例と
  並べて記載。(Issue #27)
- 両パスを `examples/43_store_personalization.cpp` ファイルヘッダコメントに文書化。

## [0.7.0] — 2026-05-11 — C++ openinference + 非同期ストリーミングブリッジ

v0.6.0 に対して提出された 4 課題を 1 つのマイナーバンプでクローズ。
ハイライト: `Provider::complete_stream_async` デフォルトが外部エンジン
コルーチン内から await されたときに segfault しなくなった (issue #4) —
NeoGraph の前に座る SSE / ストリーミング HTTP バックエンドの最も一般的な形状。
コンパニオン: v0.6.0 Python OpenInference レイヤの C++ 対応版で、
Phoenix / Arize / Langfuse が C++ 駆動トレースを Python と同様にレンダリング
可能に (issue #9)。加えて: 表面的な Python OTel detach ノイズを沈黙化 (issue #2) と
同一 `thread_id` 同時実行 + `schema_mutex_` × on_chunk ロック不変条件を
docstring に固定 (issue #6)。

### 追加

- `neograph_engine.openinference` の C++ 対応版 (issue #9)。新しい
  `neograph::observability` モジュールが 2 つの部分をカバー:
  - `Tracer` / `Span` — 小さな依存なし抽象インターフェースで、NeoGraph 自体が
    opentelemetry-cpp を引き込まない。下流が自身のバックエンド (OTel SDK、
    インメモリテストフェイク、ロギングレコーダ等) をラップするアダプタを提供。
    4 つの属性セッター (string, int64, double, bool — bool は `const char*`
    リテラルが誤解決しないよう意図的に `set_attribute_bool` に改名)、
    ストリームトークン診断用の `add_event`、status、`end()` を含む。
  - `openinference_tracer(tracer)` — CHAIN 種別のルートスパンを開き、
    `OpenInferenceTracerSession` を返す。その `cb` フィールドが
    `engine.run_stream()` に接続され、ノードごとに CHAIN 種別の子スパンを開き、
    `NODE_START`/`END` ペイロードを `input.value` / `output.value` JSON ブロブに
    詰め込み、`LLM_TOKEN` イベントを個別のスパンイベントとして記録。
  - `OpenInferenceProvider(inner, tracer)` — 任意の `Provider` をラップし、
    すべての `complete*` 呼出で OpenInference LLM 種別属性セット
    (`llm.model_name`、`llm.invocation_parameters`、
    `llm.input_messages.{i}.message.{role,content}`、
    `llm.output_messages.0.message.{role,content}`、
    `llm.token_count.{prompt,completion,total}`) を付与。
    ストリーミングオーバーロードはさらに `llm.token` イベントと最終的に
    組み立てられた `output.value` を追加。
  - 7 パリティテスト in `tests/test_openinference_cpp.cpp` が
    `InMemoryTracer` リファレンスアダプタを駆動 — ルート + ノード毎 CHAIN
    スパン階層、ERROR / INTERRUPT ステータス表面化、LLM_TOKEN スパンイベント
    記録、セッション close 時の straggler-span クリーンアップ、LLM プロバイダ
    属性セット、ストリーミングトークンイベント、例外ステータス伝播。

### 修正

- `Provider::complete_stream_async` デフォルトブリッジがストリームの期間中
  await 中のコルーチンのエグゼキュータをブロックしなくなった。
  修正前のデフォルトは `co_return complete_stream(...)` をインラインで行い、
  (a) エンジンの `io_context` ワーカースレッドを HTTP/SSE recv ループ全体の間
  サスペンド — 同じエグゼキュータ上の他のノードコルーチンがストール —
  (b) `SchemaProvider` の WebSocket Responses 分岐で、さらにエンジンワーカーの
  上に新しい `run_sync` io_context をネストして
  `run_sync(complete_stream_ws_responses(...))` を実行し、共有プロバイダ状態で
  競合し `GraphEngine::run_stream_async` 内から呼ばれた場合に断続的な
  segfault を生成。新しいデフォルトは同期 `complete_stream` 用に専用
  ワーカースレッドを生成し、各トークンを awaiter のエグゼキュータに
  ディスパッチバック (ユーザーの `on_chunk` が await 中のコルーチンと
  シングルスレッド実行 — 再入なし)、ワンショット `steady_timer.cancel()` で
  コルーチンを再開。ワーカースレッド例外は awaiter で再スロー。
  `SchemaProvider` は WebSocket パスに対してワーカースレッドさえもスキップする
  ネイティブ `complete_stream_async` オーバーライドを追加し、
  `complete_stream_ws_responses` を直接 `co_await`。`OpenAIProvider` は
  新しい基底デフォルトから透過的に利益を得る (WS パスなし、特別ケースなし)。
  `tests/test_provider_async_default.cpp` に 2 新テスト:
  `StreamAsyncBridgeDoesNotBlockExecutor` (ストリーム中に同時ティッカー
  コルーチンが進行 + チャンクがワーカースレッドではなく awaiter のスレッドで
  配信) と `StreamAsyncBridgeRethrowsWorkerException`。(Issue #4)

- `openinference_tracer`: OTel SDK が `engine.run_stream_async` +
  `StreamMode.ALL` でトレーサーを使用した場合に毎回シャットダウン時に
  出力していた `Failed to detach context` stderr トレースバックを沈黙化。
  NODE_START で作成された OTel contextvars トークンが別の `asyncio.Task` から
  detach されていたため (NODE_END コールバックが呼出元のタスクではなく
  エンジンの継続から発火)、`Context.reset(token)` が `ValueError` を発生。
  SDK は raise を飲み込んだが full traceback を `logger.exception` 経由で
  ルーティングし、本番ログを汚染 (セマンティクスに影響なし)。
  修正: attach 時の (thread, task) を記録し、不一致時は detach をスキップ。
  加えて `opentelemetry.context` に狭い `logging.Filter` をインストールし、
  我々の `_safe_detach` がスタック上にある間のみメッセージをドロップ。
  同期呼出元と同一タスク非同期呼出元は依然としてノードスパンの下に適切な
  LLM スパンネストを得る。(Issue #2)

---

## [0.6.0] — 2026-05-07 — OpenInference 可観測性レイヤ

LangSmith UX ギャップをクローズ。NeoGraph は既に OTel 形状のスパンを
出力していたため (トレースは任意の OTel バックエンドに流れた)。本リリースで
Phoenix / Arize / Langfuse がトレースをフラットな汎用アプリケーションスパン
リストではなくチャットバブル + トークンカウント UI としてレンダリングするために
使用する LLM 固有の属性レイヤを追加。ローカル Phoenix コンテナに対して
エンドツーエンド検証済み — writer→critic グラフがモデル名、プロンプト/応答、
トークンカウントが Phoenix UI で可視の 6 スパン階層 (CHAIN ルート → ノード
スパン → LLM スパン) を生成。

### 追加

- `neograph_engine.openinference` モジュール:
  - `openinference_tracer(tracer)` — `otel_tracer` をミラーするが、ルート +
    ノードスパンに `openinference.span.kind = "CHAIN"` をタグ付けし、
    ノードペイロードを `input.value` / `output.value` JSON ブロブに詰め込む
    コンテキストマネージャ。
  - `OpenInferenceProvider(inner, tracer)` — 任意の `Provider` をラップ。
    各 `complete()` で `span.kind = "LLM"` とタグ付けされた `llm.complete`
    子スパンを開き、`llm.model_name`、`llm.invocation_parameters`、
    `llm.input_messages.{i}.message.{role,content}`、
    `llm.output_messages.0.message.{role,content}`、
    `llm.token_count.{prompt,completion,total}`、
    Langfuse 互換の `input.value` / `output.value` ブロブをキャプチャ。
- `bindings/python/tests/test_openinference.py` に 4 テスト —
  InMemorySpanExporter による属性存在、スパン階層、例外パス、ノード入出力
  JSON ブロブの表明。

### 修正

- `openinference_tracer` が各ノードスパンを OTel *現在の* コンテキストとして
  attach するように (via `otel_context.attach`)。これによりノード本体内で
  開かれた子 LLM スパンがノードスパンの下にネストされる。これがないと、
  C++→Python pybind コールバック境界を越えた contextvar 伝播が実行ごとに
  期待される単一トレースツリーではなく 3+ の無関係な trace_id を生成していた。
  トークンは NODE_END / ERROR / INTERRUPT で detach され、以前の現在スパンを
  復元。既存の `otel_tracer` が文書化するのと同じパターン — 一致する
  `__exit__` なしで使用するのが安全でない `trace.use_span(...).__enter__()` の
  代わりに明示的な attach/detach。

### ノート

- OpenTelemetry はオプトイン依存のまま。`neograph_engine.openinference` の
  インポートは `opentelemetry-api` がインストールされていない場合、最初の
  使用時にのみ明確な ImportError を発生。インポート時ではない。
- Phoenix エンドツーエンド実行:

      docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
      pip install opentelemetry-exporter-otlp

  OTLP gRPC エクスポータを `http://localhost:4317` に設定し、
  `http://localhost:6006` を開いてトレースを表示。モジュール docstring に
  完全なスニペットあり。

---

## [0.5.0] — 2026-05-07 — バインディングエルゴノミクス: ライブ変更リストプロパティ

バインディング経由で公開されたメッセージ / writes / sends リストを変更する
最も自然な Python イディオムでの暗黙の no-op トラップをクローズ。
以前は `params.messages.append(msg)` がコピーを変更し、背後にある C++
vector が新しい項目を決して見なかった — 優雅な失敗 (クラッシュなし、警告なし) で
劣化した LLM 応答を生成。現在は `.append()` がライブな std::vector に
反映される。

### 追加

- `bindings/python/src/opaque_types.h` — 5 つの vector 型に対する
  `PYBIND11_MAKE_OPAQUE`: `std::vector<ChatMessage>`、`<ChatTool>`、
  `<ToolCall>`、`<graph::ChannelWrite>`、`<graph::Send>`。
- `module.cpp` `init_opaque_vectors` — `py::bind_vector` が各型を Python
  クラス (`ChatMessageList`、`ChatToolList`、`ToolCallList`、
  `ChannelWriteList`、`SendList`) として登録し、ライブ C++ vector に対する
  完全な可変シーケンスプロトコルをサポート。
- `py::implicitly_convertible<py::list, …>` 各型 — レガシーな
  build-then-assign パターン (`params.messages = [m1, m2]`) は変更なく
  動作継続。代入が Python リストを自動的にバウンドクラスに変換。
- `bindings/python/examples/23_evolving_chat_agent.py` — スレッド毎進化
  チャットエージェント (ライブ LLM): エージェントの JSON 定義が蓄積された
  会話履歴に基づいてターン間で書き換えられる。進化をまたいだ
  チェックポイント再開 (以前のメッセージが生存)、`__graph_meta__` 監査
  チャネルパターン、バリデータ境界 (ホワイトリストノード型、必須チャネル)
  を示す。

### 変更

- `params.messages` / `.tools` / `chat_message.tool_calls` /
  `node_result.writes` / `.sends` がプレーンな `list` の代わりに
  バウンドクラスを返すように。`len()`、イテレーション、`__getitem__`、
  `__setitem__`、`.append()`、`.extend()`、スライシング — すべて Python
  リストのように動作。`isinstance(x, list)` のみ False を返す。
  リポジトリ + 下流 grep でそのような isinstance 呼出サイトはゼロを確認。
- `.github/workflows/nightly.yml` — `ops/s ≥ 600K` ゲートを削除。
  4 回連続失敗 (`err=0` かつ `leak=false`) の後、しきい値 (ローカル
  ハードウェアで 969K ops/s にキャリブレーション) は共有 GitHub ホスト
  ランナーで到達不能 (233~273K ops/s を測定、ローカルの 3-4× 以下)。
  スループット回帰検出は PR 時 `bench-regression` ジョブ (安定ハードウェア、
  単発ディスパッチ µs) に存在。nightly soak の実際の価値は 5 分間の
  `err==0` + `leak_suspect==false` — 両方をハードゲートとして維持。

### ノート

- `ChatMessage.image_urls` (`std::vector<std::string>`) は意図的に移行せず —
  `vector<string>` はバインディング全体で広く使用されており、全呼出サイトを
  網羅せずにグローバル OPAQUE できない。残る制限として文書化。v0.6+
  候補。

---

## [0.4.0] — 2026-05-05 — v1.0 準備: 統一 `run(NodeInput)` ディスパッチ

v1.0 研ぎ澄ましトラック (ROADMAP_v1.md) の開幕リリース。
8 仮想 `GraphNode` 直積 (`execute` / `execute_async` / `execute_full` / … /
`execute_full_stream_async`) が単一の正規メソッドに集約:
`run(NodeInput) -> awaitable<NodeOutput>`。実行毎キャンセルメタデータが
非チャネルセット `GraphState` メンバ + スレッドローカル密輸チャネルから
明示的な `RunContext` 引数に移動。`deadline` と `trace_id` は予約拡張
スロットとしてのみ追加され、`RunConfig` から値は設定されない。
`CancelToken` が階層的 `fork()` を獲得し、マルチ Send ファンアウト
ワーカーがそれぞれプライベートシグナルを所有し、親の `cancel()` が
カスケードする。

### 追加

- `RunContext` (`include/neograph/graph/engine.h`) — 明示的な実行毎メタデータ:
  使用可能な `cancel_token`、`thread_id`、`step`、`stream_mode`、加えて予約
  `deadline` と `trace_id` スロット。エンジンがすべての `NodeExecutor::run`
  呼出にスレッド化。**PR 1, commit `a473f0e`.**
- `GraphNode::run(NodeInput) -> awaitable<NodeOutput>` — 単一正規ディスパッチ
  エントリポイント。`NodeInput { state, ctx, stream_cb }`; `NodeOutput { writes, command, sends }`。
  デフォルト本体がレガシー 8 仮想メソッドに転送するため、既存サブクラスは
  コンパイル継続。**PR 2, commit `607ce66`.**
- `CancelToken::fork() -> shared_ptr<CancelToken>` — 自身の
  `cancellation_signal` を持つ子トークン。親 `cancel()` が全ライブ子に
  カスケード (孫にも再帰的)。`run_sync(aw, parent_token)` が
  `parent_token->fork()` に切り替わり、各ネスト操作が自身のスロットを
  バインド — v0.3.x emit-vs-bind 競合とマルチ Send 単一ハンドラ上書きを
  クローズ。v0.3.x `add_cancel_hook` リストは非推奨を通じて動作継続。
  **PR 3, commit `897645c`.**
- `[[deprecated]]` on 8 レガシー `GraphNode` 仮想メソッド + `add_cancel_hook`。
  内部呼出サイト (graph_node.cpp デフォルトチェーン、デフォルト `run()`
  転送) は新しい `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` マクロ (`api.h` —
  GCC / clang / MSVC ポータブル) でブラケット。非推奨仮想メソッドを
  オーバーライドするユーザーコードは移行警告を表示。エンジン内部はクリーン。
  **PR 4, commit `35a4517`.**
- `engine.get_state_view(thread_id) -> StateView` が正規の状態読み取りに。
  生辞書 `engine.get_state(...)` は docstring でソフト非推奨 (警告は出さない —
  生辞書は有効なエスケープハッチのまま)。**PR 5, commit `f31aa53`.**
- 7 C++ + 19 Python サンプルを `run(NodeInput)` に移行。スモーク実行が
  v0.3.2 出力とビット単位で一致。**PR 6a/6b, commits
  `a2a24ef` / `0a76e3a`.**
- Pybind `PyGraphNodeOwner` が `run(NodeInput)` をオーバーライドし、
  Python ユーザーの `run` メソッド (定義されている場合) にディスパッチ。
  それ以外はレガシーチェーンにフォールスルー。`RunContext` /
  `NodeInput` / `CancelToken` を Python に公開。`cancel_token` が
  スレッドローカル密輸なしで `input.ctx.cancel_token` として到達可能。
  **PR 7, commit `4e186a5`.**
- `docs/reference-en.md` §6 GraphNode を単一 `run()` に集約。
  RunContext + `fork()` 例サブセクションを §7 の下に追加。
  README "Differences from LangGraph" が "One node method" エントリを取得。
  **PR 8, commit `519a00b`.**
- ビルトイン C++ ノード (`LLMCallNode`、`ToolDispatchNode`、
  `RouteToNode`) を `run(NodeInput)` オーバーライドに移行。
  **PR 9a, commit `d1070dc`.**
- 新人モードトラップ修正: README CMake スニペットが `graph::` サブ名前空間、
  cppdotenv パス、`OpenAIProvider::create()` vs `create_shared()`、
  `neograph::json` as nlohmann サブセット、3 引数 vs 2 引数 `compile()` を
  文書化。Python `compile(def, ctx, store=None)` キーワード引数追加
  (追加的、非破壊)。**commit `ee11ed6`.**

### 変更

- README: "10K-worker measured stress test" セクション — RTX 4070 Ti +
  Gemma 4 E2B Q4 on neoclaw, N=10000 done @ 0 err / 424s / 2572 MB
  peak / ~1 KB marginal worker cost / p99 648 ms (`7840b81`)。
- README: "Production economics" セクション — フリート安全性 + RAM デルタ
  フレーミング (`b82b15a`)。
- README: "No Docker required" + "Dependency-drift immunity"
  箇条書きを LangGraph デルタリストに追加 (`333b482`、`a6061d7`)。

### 非推奨

- `GraphNode::execute / execute_async / execute_full /
  execute_full_async / execute_stream / execute_stream_async /
  execute_full_stream / execute_full_stream_async` — v0.5.x まで
  `[[deprecated]]` 注釈付きで動作継続、v1.0 で削除。
- `CancelToken::add_cancel_hook` — `fork()` に置換。同じ非推奨期間。

### ノート

- 検証: 442 → 452 ctest (3 NodeRunDispatch + 7 CancelTokenFork 追加) +
  96 pytest + 5 ライブ LLM/WS グリーン at v0.4.0 tag。
- サブ PR (`run(const NodeInput&)` 参照パラメータ) が pybind 非同期パス下で
  v0.2.0 RunConfig コルーチン参照 UAF クラッシュ形状を発動。
  修正はマージ前に着地: `NodeInput in` 値渡し。`node.h` に文書化。

---

## [0.3.2] — 2026-05-05 — キャンセル伝播強化 (5 ラウンド)

v0.3.0 単発キャンセルが露呈したギャップをクローズする 5 ラウンドパッチ
シリーズ: Send ファンアウト伝播、インプロセスポーリング、Python 用フック、
C++ スコープ、例外型付け。また FastAPI SSE チャットデモ評価からの
TODO_v0.3.md フィードバックバッチ — `resume_if_exists`、dict-or-list
`update_state`、型付き状態読み取り用の StateView も着地。

### 追加

- `RunConfig::resume_if_exists` — 明示的な `resume()` 呼出なしで以前の
  スレッドのチェックポイントを再開するオプトイン。標準的なマルチターン
  チャットセマンティクス: `engine.run(cfg)` が `thread_id` 存在時に
  会話を継続。
- `engine.update_state(thread_id, dict | list[ChannelWrite],
  as_node="")` — 両方の形状を受け付け。修正前は `dict` のみ動作。リストを
  渡すと暗黙の no-op。リスト形式はすべてのノード本体の emit 形状と対称。
- `StateView` (`bindings/python/neograph_engine/state_view.py`) —
  Pydantic 型付き状態読み取り。`engine.get_state_view(thread_id) ->
  StateView` がフラットなドットアクセス (`view.messages` / `view.foo`)
  と dict エスケープハッチ `view.raw` を返す。型付きチャネル定義のための
  サブクラス化: `class ChatState(ng.StateView): messages: list[dict] = []`。
- `bindings/python/tests/test_async_cancel_live_llm_fanout.py` —
  飛行中キャンセルが全 Send 生成兄弟をソケット層で本当に中断することを
  表明 (v0.3.1 根本原因パッチ)。
- `examples/22_self_evolving_graph.py` — v0.3.2 に移動、
  TODO_v0.3.md #9 クックブック折り畳み。
- ROADMAP_v1.md — キャンセルラウンド事後分析から導出された設計研ぎ澄まし
  候補 (単一ディスパッチ、RunContext、階層的 CancelToken — すべて
  v0.4.0 で配信)。
- Doxygen `/* */` ワイルドカード修正 — `acp/types.h` がパスワイルドカード
  (`fs/*`、`terminal/*`) を含む `/**` ブロックを持ち、ネストされた
  コメントを開き後続の全診断を抑制していた。`&#42;` HTML エンティティに置換。

### 修正

- キャンセル伝播、5 累積ラウンド:
  1. v0.3.0 単一ノード — `cancel_token` が `Provider::complete` に到達。
  2. v0.3.1 マルチ Send ポインタドロップ — ファンアウトワーカーが
     `run_cancel_token_shared()` を共有 (チャネルセット外のワーカー毎状態を
     `init_state + restore` が再構築した際に失われていた)。
  3. v0.3.1+ インプロセスポーリング — エンジンスーパーステップループが
     ステップ間でポーリング、LLM I/O 時のみではない。
  4. v0.3.2 Python 用フック — `add_cancel_hook` が実行毎トークンに
     コールバックを登録、`cancel()` 時に発火。同期 Python `execute()` が
     スレッドローカルスコープなしでアドホックキャンセルハンドラを
     インストール可能に。
  5. v0.3.2 C++ スコープ + リトライ + 例外型付け — メインスレッドで
     新規スロー `NodeInterrupt` (libstdc++ `__exception_ptr::_M_release`
     競合を回避)、リトライ予算がキャンセルを尊重、ランタイム vs ロジック
     例外分離。
- `execute_stream` 専用 Python ノードが暗黙にデフォルト `execute` パスに
  フォールスルーしていた (NotImplementedError)。現在 `run_stream` が
  ユーザーがストリーミングバリアントのみをオーバーライドした場合に
  `execute_stream` を直接配線。
- `update_state` が list[ChannelWrite] を受け付け — 暗黙の no-op をクローズ
  (TODO_v0.3.md #5)。

### ノート

- 442 ctest + 96 pytest + 2 ライブ LLM (単一 + ファンアウトキャンセル)
  グリーン at v0.3.2 tag (`915e90e`)。
- 27/30 C++ サンプル + 20/22 Python サンプルが `examples/run_all.py` 下で
  パス。スキップされたテストは外部サービスが必要 (Postgres / Crawl4AI /
  ライブ OpenAI)。
- Valgrind 6 サンプル 0 エラー、815 allocs / 815 frees クリーン。
- ベンチ中央値 5.185 µs/iter on seq path (v0.3.0 ベースライン) —
  ラウンド全体でゼロパフォーマンス回帰。

---

## [0.3.0] — 2026-05-04 — 協調的キャンセル伝播

FastAPI SSE チャットデモ評価中に報告された本番コスト漏洩ギャップをクローズ:
フロントエンド `AbortController` が asyncio タスクをキャンセルしても、
上流の OpenAI リクエストが完了まで実行され続けることがなくなった。
キャンセルが実行の全層を通じて伝播。

### 追加

- `neograph::graph::CancelToken` (アトミックフラグ + asio
  `cancellation_signal`) と `CancelledException` —
  `include/neograph/graph/cancel.h`。協調的キャンセルプリミティブ。
  `RunConfig::cancel_token` (オプション `shared_ptr`) 経由で渡す。
  エンジンスーパーステップループがステップ間で `is_cancelled()` を
  ポーリングし `CancelledException` で脱出。トークンの `cancellation_slot()`
  が実行の `co_spawn` にバインドされ、飛行中 LLM HTTP ソケット操作が
  ワイヤ上で中断される (asio `operation_aborted`)。
- `CompletionParams::cancel_token` — 複数の `provider.complete()` 呼出に
  またがって中断をスレッド化するユーザー向けの明示的ピン。`Provider::complete`
  がそれを読み取り (または `PyGraphNode::execute_full_async` が設定する
  スレッドローカル `current_cancel_token()` にフォールバック)、スロットを
  内部 `run_sync` io_context にバインド。キャンセルに当たった同期 Python
  ノードも課金停止。
- `GraphState::run_cancel_token()` — pybind `PyGraphNode` が同期 Python
  `execute()` 呼出の周りに `CurrentCancelTokenScope` をインストールするために
  使用する実行毎・非直列化ハンドル。同期 Python ユーザーがノードコードを
  変更せずに透過的なキャンセル伝播を得る仕組み。
- pybind `engine.run_async` / `run_stream_async`: asyncio
  `Future.cancel()` が `add_done_callback` 経由で `CancelToken::cancel()` に
  配線され、`co_spawn` がトークンのキャンセルスロットをバインド。
- pybind 安全解決ヘルパー `_safe_set_future_result` /
  `_safe_set_future_exception` — `call_soon_threadsafe` 経由で post された
  `future.set_result` / `set_exception` 呼出をキャンセル済み future の
  `InvalidStateError` ストームからガード。
- `bindings/python/tests/test_async_cancel_live_llm.py` — ライブ OpenAI
  E2E が `Future.cancel()` 後 3 秒未満で OpenAI HTTP が完了することを表明
  (実用上即時。修正前は ~7–8 秒のキャンセルされないストリーミング)。
  `NEOGRAPH_LIVE_LLM=1` でない限りスキップ。
- `examples/22_self_evolving_graph.py` — 自己進化グラフ PoC:
  `prompted_llm` ノードが自身のプロンプトを JSON 設定から読み取り、
  LLM リライタが実行間でグラフ定義を変異させ再コンパイル可能。
  `0.0 → 0.4` スコア改善を示す。リライタのチャネルフロー推論ギャップを
  文書化。

### 変更

- `Provider::complete(params)` が `params.cancel_token` が設定されているか
  スレッドローカル `current_cancel_token()` がアクティブな場合に内部
  キャンセルスロットをその `run_sync` にバインドするように。オプトイン
  しない呼出元には以前のデフォルト動作 (キャンセルなし) を保持。
- `neograph::async::run_sync` がオプションの `graph::CancelToken*`
  パラメータを獲得。非 null 時、バインドされた spawn がトークンの
  スロットをバインド。
- pybind `resolve_future_async` が `call_soon_threadsafe` 経由で
  `future.set_result` を直接呼ぶ代わりに安全解決ヘルパー経由でルーティング。

### ロードマップ (v0.3.x に延期 — `TODO_v0.3.md` 参照)

- 同一 `thread_id` での LangGraph スタイル自動チェックポイント再開。
- `run_async` エラーメッセージでのストリーミング専用ノードヒント。
- `cb.emit_token(node, data)` エルゴノミックヘルパー。
- README「LangGraph との違い」セクション。
- `update_state` シグネチャとドキュメントの整合。
- `get_state` フラットヘルパー / Pydantic アクセサ。
- `run_parallel_async` と `run_sends_async` 分岐ファンアウトでの
  キャンセル伝播のライブ検証。
- pgvector RAG サンプル。

---

## [Unreleased] — Stage 4

Stage 4 が非同期パス上の最後の `run_sync` ホップをクローズ。`run_async`
が呼出元のエグゼキュータ上でエンドツーエンドに留まるように: 1 つの
`io_context` スレッド上の 3 つの 50 ms エージェントが ~150 ms (逐次) から
~50 ms (オーバーラップ) に短縮 (`examples/27_async_concurrent_runs`)。

### 破壊的変更

- **`GraphNode::execute_full_async` デフォルトが非同期優先に反転。**
  `NodeResult` にラップする際に同期 `execute_full(state)` の代わりに
  `co_await execute_async(state)` を呼び出すように。同期 `execute_full`
  オーバーライドのみから `Command`/`Send` を送出するサブクラスは
  1 行の `execute_full_async` ブリッジを追加する必要がある:
  ```cpp
  asio::awaitable<NodeResult>
  execute_full_async(const GraphState& state) override {
      co_return execute_full(state);
  }
  ```
  ブリッジなしでは `Command`/`Send` が非同期パスで暗黙ドロップされる —
  3.0 がスーパーステップ毎 `io_context` spawn のコストで同期経由ルーティング
  により修正した 2.0 潜在ディスパッチバグ。全ツリー内サブクラス
  (`deep_research_graph`、サンプル 10/14/21、テスト 5 サイト) がブリッジを
  持つ。

### パフォーマンス

- サンプル 27 wall time: **152 ms → 53 ms** (3 エージェント × 50 ms タイマー
  ステップ on 1 `io_context` スレッド、完全オーバーラップ)。
- 単一実行ベンチマークで測定可能な回帰なし。`run()` は依然として
  `run_sync` 経由で新しいシングルスレッド `io_context` を通じて同じ
  コルーチンを駆動。

### テスト

- 341/341 ctest グリーン
- 295/295 ASan+UBSan グリーン
- コルーチン重いサブセットで Valgrind クリーン (20 テスト、2.4 s)

### リリース後検証 (同日)

- **全 30 サンプル再実行:** 26/29 PASS、0 FAIL、3 環境ゲート
  (clay_chatbot → raylib、postgres_react_hitl → docker compose、
  deep_research フルループ → crawl4ai サービス)。`21_mcp_fanout`
  実ネットワーク I/O 下で 3 MCP 呼出 / 8 ms wall を測定 — Stage 4
  オーバーラップが実ネットワーク I/O 下で保持。

- **ARM64 互換性 (docker buildx --platform linux/arm64):**
  `Dockerfile.arm64-smoke` at repo root。ubuntu:24.04-arm64 +
  core+llm+async+sqlite+tests ビルドが QEMU エミュレーション下で ~15 分で
  完了。**306/306 ctest グリーン** on ARM64。削除済みバイナリサイズ
  0.81-0.88 MB (x86_64 とほぼ同一)。サンプル 27 がエミュレーション下で
  65 ms で実行 (ネイティブ x86_64: 53 ms)。Linux/ARM64 が macOS ベータ
  (Apple Silicon) と並んでサポート対象として確認。

- **キャッシュ局所性 (Ryzen 5800X / Zen 3, Valgrind cachegrind,
  32 KB L1i/d 8-way, 32 MB L3 16-way):**
  `bench_concurrent_neograph` sweep N=1 → 10,000。

  | N | I refs | LLi misses | LLi miss% | Native p50 |
  |---:|---:|---:|---:|---:|
  | 1 | 5.3 M | 4,313 | 0.08% | 17 µs |
  | 100 | 11.8 M | 4,320 | 0.04% | 6 µs |
  | 10,000 | 648 M | 4,329 | 0.00% | 5 µs |

  最終レベル命令ミスが N の 4 桁にわたって ~4,320 でフラット。固有ホット
  コードワーキングセット ≈ 277 KB (L3 の 0.85%)。N=10,000 で 648 M 命令が
  4,329 LL ミスのみを発生 — 約 150,000 命令につき 1 ミス。ネイティブ p50
  が I キャッシュウォーミングだけで 17 µs から 5 µs に低下。「バースト
  並行ロバスト性」ポジショニングの初の測定証拠。

---

## [3.0.0] — 2026-04-22

3.0 は Taskflow 依存を削除し、同期と非同期のスーパーステップ実行を単一の
asio コルーチンパスに統一。グラフ定義 JSON、ノード ABI、チェックポイント
スキーマ、公開エントリポイント (`run`、`run_async`、`run_stream`、`resume`)
は 2.0 とソース互換。破壊的変更は同期 `execute_full` オーバーライドのみから
`Command`/`Send` を送出する `GraphNode` サブクラスに限定。

### 破壊的変更

- **`deps/taskflow/` と Taskflow INTERFACE ターゲットが消滅。**
  同期スーパーステップループ、`run_one`、`run_parallel`、`run_sends`、
  プロセスワイド `tf::Executor` 静的変数を削除。NeoGraph のインクルード
  パス経由で `#include <taskflow/...>` する下流利用者は Taskflow を
  別途ベンダー提供する必要がある。
- **`GraphNode::execute_full_async` デフォルトが直接呼出で同期
  `execute_full` にブリッジ (no `co_await execute_async`)。**
  これにより同期専用オーバーライドから送出された `Command`/`Send` を保持 —
  一般的な 2.0 パターン — 全エントリポイントが現在共有する非同期パスを通じて。
  ノンブロッキング I/O と `Command`/`Send` の両方を必要とする非同期
  ネイティブノードは `execute_full_async` を直接オーバーライドする必要がある。
  docstring は 2.0 以来これを述べているが、2.0 は同期 `run()` がコルーチン
  パスを完全にバイパスしていたためこれを行使しなかった。
- **`NodeExecutor::run_one` / `run_parallel` / `run_sends` 同期
  メソッドを削除。** `_async` 対応版を使用。
- **CPU 並列ファンアウトはオプトイン。** 以前は Taskflow がデフォルトで
  プロセスワイドスレッドプールを提供。3.0 では `run_parallel_async` と
  `run_sends_async` のマルチ Send 分岐がコルーチンを駆動するエグゼキュータ
  上で分岐をディスパッチ — 同期 `run()` が生成するシングルスレッド
  io_context、または `run_async()` の呼出元自身のエグゼキュータ。
  I/O バウンドファンアウトは依然オーバーラップ (単一スレッドでの co_await
  サスペンション)。CPU バウンドファンアウトは呼出元が `run_async()` に
  マルチスレッドエグゼキュータを使用するか、
  `engine->set_worker_count(N)` でエンジン所有プールをオプトインしない限り
  直列化。

### 追加

- `neograph::async::run_sync_pool(awaitable, n_threads)` — 既存の
  シングルスレッド `run_sync` と並ぶ N ワーカー同期↔非同期ブリッジ。
  呼出用に新しい `asio::thread_pool` を生成し、内部
  `make_parallel_group` 分岐が別々のワーカーで実行されるように。
- `GraphEngine::set_worker_count(n)` — `NodeExecutor` が並列ファンアウト
  ディスパッチに使用するオプトインエンジン所有 thread_pool。
  エグゼキュータを再構築。任意の同時実行の前に呼び出す必要がある。

### 変更

- `GraphEngine::execute_graph` (同期) が消滅。全エントリポイント
  (`run`、`run_stream`、`resume`) が `execute_graph_async` 経由で
  `neograph::async::run_sync` を通じてルーティングされるため、
  スーパーステップループ、リトライバックオフ、チェックポイント I/O、
  並列ファンアウトが 1 つのコルーチンパスでエンドツーエンドに存在。
- `benchmarks/concurrent/bench_concurrent_neograph.cpp` が
  `tf::Executor` / `tf::Taskflow` から `asio::thread_pool` +
  `asio::post` に切替 (呼出元側ドライバ用)。

### パフォーマンス (bench_neograph Release -O3 -DNDEBUG on reference Linux, 10-run median)

- `seq` エンジンオーバーヘッド (3 ノードチェーン、カウンタ): **~5.0 µs** per call。
- `par` エンジンオーバーヘッド (5 ワーカーファンアウト + サマライザ): **~11.8 µs**
  per call。
- ベンチプロセス全体のピーク RSS (ウォームアップ + seq + par iters):
  **4.8 MB**。
- 同一ワークロードで LangGraph 1.1.9 と比較: **131× faster seq, 199×
  faster par** per iteration。RSS ~12× lighter。

本 CHANGELOG の以前のドラフトは 3.0 回帰として "~46 µs seq / ~114 µs par"
を記載していた。これらの数値は `CMAKE_BUILD_TYPE` が未設定のビルドツリーから
来ており、ベンチバイナリが `-O3 -DNDEBUG` なしでコンパイルされていた。
適切な Release ビルドでは非同期対応版集約は 2.0 の Taskflow 同期パス
(2.0 README が同一ホストで 20.65 µs seq / 150.7 µs par と宣伝) に対して
**勝ち**。修正された図は
[`docs/images/bench-engine-overhead.png`](docs/images/bench-engine-overhead.png) に。

### 移行

- ノードが `execute()` / `execute_async()` をオーバーライドし
  `Command` / `Send` を送出しない場合は対応不要。
- `Command` / `Send` を送出するために同期 `execute_full` をオーバーライド
  している場合: 変更不要 — 3.0 非同期パスデフォルトが現在は同期
  オーバーライドを直接呼び出す。`Command.goto_node` ルーティングが
  同期・非同期エントリポイント同様に動作。
- `execute_async` (非同期ネイティブ I/O) をオーバーライドし、かつ
  `Command` / `Send` を望む場合: `execute_full_async` を直接オーバーライドし、
  そこで `NodeResult` を組み立てる。`execute_async` のみのオーバーライドは
  デフォルト `execute_full_async` が非同期 `execute_async` ではなく
  同期 `execute_full` を経由するため `Command` / `Send` を暗黙ドロップする。
- CPU 並列ファンアウトに `engine->run()` 経由で Taskflow のプロセスワイド
  プールに依存していた場合: コンパイル後に 1 回
  `engine->set_worker_count(N)` を呼び出すか、自身のマルチスレッド
  `asio::thread_pool` / io_context 上で `run_async()` 経由でエンジンを駆動。

---

## [2.0.0] — 2026-04-22

Stage 3 非同期 API を伴う最初の公開リリース。これは破壊的リリースです。
以下の変更はコンパイル (C++ 標準) と ABI (抽象基底クラスが非同期対応版を
獲得) に影響します。同期呼出サイトはビット単位で保持されるため、
**`Provider` / `CheckpointStore` / `GraphNode` / `Tool` をオーバーライド
しないアプリケーションコードは変更なく動作し続けます**。

### 破壊的変更

- **C++20 必須。** 公開 API が `std::coroutine` サポートを必要とする
  `asio::awaitable<T>` 戻り値型を公開。利用者は `-std=c++20` (以上) で
  コンパイルする必要がある。GCC 13+、Clang 15+ テスト済み。
  GCC 13 コルーチン回避策は `docs/ASYNC_GUIDE.md` §4.1 参照。
- **libpqxx 依存を削除。** `neograph::postgres` が直接 libpq にリンク。
  Ubuntu 24.04 ユーザーは libpqxx-7.8t64 の C++17/C++20 ABI 分離により
  導入された `pqxx::argument_error::argument_error(..., std::source_location)`
  リンクエラーに当たらなくなった。CMake find が `PostgreSQL::PostgreSQL`
  (CMake バンドル FindPostgreSQL) を対象とするように。`libpqxx-dev` のみを
  インストールしていた利用者は `libpq-dev` もインストール/保持する必要がある。
- **`Provider`、`CheckpointStore`、`GraphNode`、`MCPClient` ABI 拡張。**
  各々が非同期対応版仮想関数 (`complete_async`、`save_async`、
  `execute_async`、`rpc_call_async` とそれらのバリアント) を獲得。
  下流サブクラスは 2.0 ヘッダに対して再コンパイル。実 I/O を行う実装者は
  ネイティブ非同期オーバーライドを提供することが推奨されるが、
  ソースはサブクラスが望まない限り変更なし。
- **`CheckpointStore::save` / `load_latest` / `load_by_id` / `list`
  / `delete_thread` はもはや純粋仮想ではない。** これらは
  `neograph::async::run_sync` 経由で一致する `_async` 対応版にブリッジする
  デフォルト実装を持つようになった。同期側をオーバーライドするサブクラスは
  動作継続。どのオーバーライドも提供しなかったサブクラス (以前は
  コンパイルエラーだったはず) は無限再帰する — 契約: 各同期/非同期ペアの
  少なくとも一方をオーバーライドすること。

### 追加

- **非同期 API** 全 I/O 層にわたって
  (`docs/ASYNC_GUIDE.md` 完全リファレンス):
  - `Provider::complete_async` 基底クラスと全ビルトインプロバ イダ
    (OpenAI、Schema、RateLimited) に。
  - `MCPClient::rpc_call_async` HTTP と stdio トラン スポート両方に。
    stdio は `asio::posix::stream_descriptor` 使用。
  - `CheckpointStore::*_async` 全 8 同期メソッドに。
  - `GraphNode::execute_async` + stream / full / full_stream バリアント、
    非同期ネイティブクロスオーバーデフォルト付き。
  - `GraphEngine::run_async` / `run_stream_async` / `resume_async`
    `execute_graph_async` を駆動 — `asio::experimental::make_parallel_group`
    経由の並列ファンアウトを含むエンドツーエンドコルーチンスーパーステップ
    ループ。
  - `neograph::AsyncTool` アダプタ — 同期 `Tool` インターフェー スを保持しつつ
    コルーチン本体を望むユーザーツール用。
- **`neograph::async` 名前空間** — HTTP クライアント、接続プール、
  SSE パーサー、run_sync ブリッジ、URL エンドポイント分割。参照
  `include/neograph/async/*.h`。
- **新サンプル**:
  - `examples/27_async_concurrent_runs.cpp` — 1 つの `io_context` 上で
    複数エージェント。
  - `examples/05_parallel_fanout.cpp` (書き直し) — `run_parallel_async`
    を使用した単一グラフ実行内の非同期ファンアウト。
- **CI ベンチ回帰ゲート** (`.github/workflows/ci.yml`) —
  PR チェックが `bench_async_http` / `bench_async_fanout` /
  `bench_neograph` の下限を強制。

### パフォーマンス

feat/async-api ブランチで Stage 2 同期ベースラインに対して測定:

- `bench_async_http --mode async_pool --concur 1000`:
  6064 ops/s → **17834 ops/s** (2.9×)。
- `bench_async_fanout --concur 50000`:
  スレッド毎エージェント達成不能 → **541K ops/s / 67 MB RSS**。
- `examples/27_async_concurrent_runs` (3 × 50ms async work):
  150ms (sync) → **50ms** (1 io_context thread)。
- `examples/05_parallel_fanout` (3 × 100-150ms async work):
  370ms (sequential) → **150ms** (1 io_context thread)。
- `bench_neograph` エンジンオーバーヘッド: 変更なし (~30 µs seq /
  ~205 µs par)。コルーチン機構はホットパスを回帰させない。

### 2.0.0 に未収録

- **Taskflow 依存** が残存。同期 `engine.run()` パスが依然ファンアウトに
  使用。Sem 4.5 で同期パスが `run_sync(*_async)` で置換可能か再評価し、
  依存を完全に削除できるか検討。

### クロスプラットフォーム

3 プラットフォームが 2.0.0 で異なる安定性階層でサポート。
階層はリリース前にプラットフォームが受けた実世界検証の量を反映 —
機能カバレッジではない (コードベースは単一ソースで `#ifdef _WIN32` 分割。
テストが通れば機能はプラットフォーム間で同等)。

#### Linux — **GA** (本番対応)

* Ubuntu 24.04、GCC 13。
* 完全 332/332 ctest グリーン ローカル (Postgres via docker
  `postgres:16-alpine`) + コミットされた CI 下限内の全ベンチ。
* MCP stdio on fork/pipe/execvp + `asio::posix::stream_descriptor`。
* Postgres 非同期対応版 on libpq nonblocking + `asio::posix::stream_
  descriptor` wrapping `PQsocket`。
* 上記の全パフォーマンス数値のリファレンスプラットフォーム。

#### macOS — **ベータ**

* macos-latest (Apple Silicon)、Clang via Xcode。
* CI ビルド + 非 Postgres テスト実行。Postgres 統合ケースはサービス
  コンテナなしで自己スキップ。POSIX パス (同じ fork/pipe + asio::posix
  コード) が行使される。
* `CoreFoundation` + `Security` フレームワークが TLS のシステム証明書
  読み込みのために httplib 経由でリンク。
* 2-4 週間の CI 実行とユーザー報告で実行時動作の違いがないことが確認
  されるまでベータ扱い (コルーチンスケジューリング、SIGPIPE / EPIPE
  形状、パイプバッファサイジング)。インシデントなしにそれらが集まれば
  GA への昇格を目標。

#### Windows — **アルファ**

* windows-latest、MSVC 19.44 (VS 2022)、x64。
* CI 範囲: **core + async + MCP + LLM のみ**。Postgres と SQLite
  バックエンドは Windows CI ジョブで無効。vcpkg が毎回 OpenSSL /
  libpq / zlib / lz4 をソースからコンパイルするため (~20 分、
  `x-gha` 削除以降上流に動作するバイナリキャッシュバックエンドなし)。
  Windows ユーザーは自身の vcpkg / choco セットアップ経由でこれらを
  ローカルコンパイル。
* OpenSSL via runner のプリインストール choco パッケージ
  (`C:/Program Files/OpenSSL-Win64/`)。httplib + asio::ssl の TLS パスが
  コンパイル・リンク。
* MCP stdio: `CreateProcess` + named-pipe (FILE_FLAG_OVERLAPPED) +
  `asio::windows::stream_handle`。オーバーラップドパイプパスは
  ローカル Windows 検証なしで MSDN 仕様に対して記述。最初のユーザーが
  エッジケース (ERROR_IO_PENDING 処理、大きな JSON 応答でのパイプ
  バッファ境界) を表面化することが予想される。
* Postgres 非同期対応版 (ローカル有効時): `asio::ip::tcp::
  socket::assign` wrapping `PQsocket` が返す SOCKET (64-bit SOCKET 値を
  保持するため `native_handle_type` 経由でキャスト)。Windows CI では
  行使されず — ローカルのみ。
* コルーチン機構は MSVC の `<coroutine>` に存在。仕様上 GCC/Clang と
  一致することが期待されるが、`examples/27` のクロス実行オーバーラップ
  測定は Windows でまだ確認されていない。
* 2.0.0 を通じて **アルファ** 扱い。1 人の本番ユーザーが stdio/pipe
  またはコルーチンスケジューラ問題に当たらずに 1 週間マルチエージェント
  ワークロードを実行し、かつ Postgres 非同期対応版が vcpkg のフル
  libpq ビルドを実行する意思のあるユーザーによってローカル検証されれば
  ベータに昇格。

> **パターン**: CI グリーンは下限であり、上限ではない。レイヤ 3 実行時
> 動作の違い (コルーチンスケジューリングタイミング、パイプバッファ境界、
> ソケット引き継ぎセマンティクス) は実際のワークロード下でのみ表面化する。
> 上記の階層言語は、初日から 3 つすべてが交換可能であるかのように装うのでは
> なく、各プラットフォームに対して正しい期待をユーザーに与える。

### バンプ後修正

- **`async::HttpResponse` headers map** — レスポンスインターフェースが
  ワイヤ順序と元の大文字小文字を保持する `(name, value)` ペアの `headers`
  ベクトルを公開。加えて大文字小文字を区別しないアクセサとして
  `get_header(name)` を提供。Retry-After と Location は後方互換性のため
  専用フィールドとして残存。以下の MCP セッション追跡修正のブロック解除。
- **MCP `Mcp-Session-Id` ヘッダ追跡** — Sem 2.6 httplib→async_post
  移行がこれを暗黙にドロップしていた。初期化後すべての RPC が新しい
  headers アクセサ経由でサーバー割り当てセッション ID をエコーバックし、
  サーバーのセッション状態がルーティング可能に。
- **MCP stdio awaitable mutex** — `StdioSession::rpc_call_async` が
  `std::mutex` を使用しており、同じシングルスレッド io_context 上の
  2 つのコルーチンが同じセッションを呼び出すとデッドロック (2 番目の
  `lock_guard` が 1 番目に必要なワーカーをブロック)。
  `asio::experimental::channel<void(error_code)>` 容量 1 セマフォに置換し、
  2 番目の取得者が協調的にサスペンドするように。
- **`PostgresCheckpointStore` 非同期対応版** — 全 8 CheckpointStore
  非同期メソッド (`save_async`、`load_latest_async`、
  `load_by_id_async`、`list_async`、`delete_thread_async`、
  `put_writes_async`、`get_writes_async`、`clear_writes_async`)
  が真に非同期に。内部: `PQsetnonblocking(1)` +
  `PQsendQueryParams` + `PQsocket()` 上の `asio::posix::stream_descriptor` +
  `co_await sock.async_wait(wait_read/wait_write)`。
  4 スロットのプール上の 4 つの同時 `save_async` 呼出が
  `run_sync` を通じて直列化される代わりにワイヤレベルで並列に
  commit-fsync するように。

---

## [0.1.0] — 2026-04 以前

プレリリース開発。公開 API 安定性保証なし。
