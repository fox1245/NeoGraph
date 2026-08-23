<!-- neograph-i18n: source=CHANGELOG.md locale=ja source_sha256=7532000a606d31bd33b572489fe0c8ae482c0497c04cdf645187a820824f1258 -->
# 変更履歴

**Languages:** [English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md) | [日本語](CHANGELOG.ja.md) | [简体中文](CHANGELOG.zh-CN.md)

NeoGraph に対するすべての重要な変更は、このファイルに記載されています。

形式は [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) に従います。バージョニングは [Semantic Versioning](https://semver.org/spec/v2.0.0.html) に従います。

---

## [未リリース]

### 追加
- **Program ベース A2A サービスの厳格認証。** Program A2A コンストラクタに
  オプトインの `require_authenticated_requests` を追加しました。有効時は
  collaboration envelope だけでなく通常の message、stream、task 取得・取消
  RPC にも authenticator を適用します。互換性のため既定値は false です。
- **ホスト所有 A2A 制御メッセージのインターセプト。**
  `ProgramAgentAdapter` は認証済み typed message を `ProgramRuntime` 開始前に
  ホスト callback へ渡せます。callback は task/context identity を保持し、
  返された Task は通常の task 観測用に保存されます。
- **信頼されていない補助ランタイムコンテキスト。**
  `ContextArtifactKind::UntrustedSupplemental` と
  `ContextPlacement::AfterHistory` を追加しました。既存の artifact kind の
  system message 動作は維持しつつ、明示的に信頼されていない RAW または
  derived context を receipt に結び付け、完全な Human/AI/Tool 時系列履歴の
  後に user data として渡せます。

## [0.12.1] - 2026-08-23

### 修正
- **Pythonサンプルのランタイム契約。** ルートチャネルのサンプルを予約済み
  `__route__`へ更新し、動的`Send`ペイロードチャネルを宣言し、純粋なキャッシュ
  サンプルを`CacheScope::Reusable`へ切り替えました。さらにOpenRouterのベース
  URLを保持し、Windowsコンソール出力をUTF-8安全にし、成功扱いでも空の
  reasoningモデル応答を拒否または再試行します。
- **Responsesトランスポートのデモ。** 公式OpenAIでは実際のWebSocket
  ストリーミングエントリポイントを使用し、互換ゲートウェイではビルド済みの
  HTTP/2またはHTTP/1.1を選択します。ディープリサーチデモには上限付きの
  トークン・時間予算と3-way/2-worker fan-outを適用し、OpenRouterで実地検証しました。
- **対処可能なWindowsネットワーク障害。** Pythonで`_HAVE_LIBCURL`を公開し、
  ロケール符号化された`std::system_error`メッセージをcategoryとcode付きの安全な
  ASCII診断へ変換します。Winsock 10060などの原因が`UnicodeDecodeError`に
  置き換わることはありません。

## [0.12.0] - 2026-08-23

### 追加
- **厳格なランタイム・インターセプション完了。** `StrictRuntimeProfile`、永続的なプロバイダ終端結果レシート、SQLite スキーマ v3 マイグレーション、トランスポートに依存しない必須 Hook バックエンド（JSON-RPC stdio/HTTP による成果物公開）、一般必須コンテキスト、`HardConstraint` 成果物、完全保存の `ContextTransformReceipt`、永続的なランタイム開発者指示決定、およびコンパイル前の予約 `ProgramSynthesisGateway` を追加しました。生成されたソースは依然として自身をアクティブ化、バインド、マイグレート、または生成できません。これらは引き続きホスト所有の Program 遷移です。`DurableProviderDispatchReceiptStore` のカスタムサブクラスは、今後、終端の `settle`/`outcome` を実装する必要があります。C++ コンシューマーはこの ABI 変更に対して再ビルドする必要があります。

- **分離された PostgreSQL Program ストア統合フィクスチャ。** ダイジェスト固定、ループバックのみの `tests/fixtures/q7-postgres/compose.yaml` を tmpfs ストレージとヘルスゲート付きで追加しました。`NEOGRAPH_TEST_POSTGRES_URL` が使い捨てテストデータベースを指定する場合、`ProgramCatalogTest.PostgreSQLProgramStoreReopensActivationAndOwnerVisibility` は公開、アクティベーション、再オープン、およびオーナー分離を対象とします。これはテストインフラストラクチャであり、Q7 最終証明スナップショットではありません。

- **フェイルクローズ QuickJS レガシードレイン監査。** `scripts/audit_legacy_drain.py` とその CTest 契約を追加しました。このツールは、明示的に列挙された凍結 Program/Harness ストレージスナップショットから、検証可能なコンテンツアドレス指定の証明を生成します。未知のレコードや変更可能なレコード、未分類のレガシーソース、ドレイン専用レコード、アクティブまたはリカバリ可能なレガシー実行を拒否します。ライブの `-wal`、`-shm`、または `-journal` のサイドカーが付属する SQLite 入力は、開く前に拒否されるため、アクティブな WAL データベースの生のコピーは最終証明として機能しません。これにより Q7 の証拠メカニズムが確立されますが、デプロイメント固有の最終ドレインやレガシーパーサーの削除が完了したとは主張していません。

- **PostgreSQL最終ドレインアーカイブスキャン。**レガシードレイン監査人は、現在、凍結された `program_postgres_dump` カスタムアーカイブを受け入れ、 `pg_restore` をデータのみ、厳密テーブル、スクリプト出力モードでのみ呼び出します。データベースへの復元は決して行いません。Programバンドル、バージョン、およびアクティベーションテーブルを、永続化されたIDに対して検証し、必須テーブルを省略または変更したアーカイブを拒否し、レガシーProgramバージョンのアクティベーションを最終削除のブロッカーとして扱います。

- **デプロイメントなしQ7最終証明モード。** レガシードレイン監査人は、プレリリースまたは本番のNeoGraphデプロイメントが一度も存在しない場合にのみ、名前付き演算子の証明文を受け入れるようになりました。このモードは、ストレージ対象も過去のレガシーアーティファクトも受け入れず、生成された証明に `evidence_mode: "no_deployment_attestation"` とラベル付けし、混合または未証明の空のインベントリの場合はフェイルクローズします。消去、削除、紛失、またはアクセス不能な過去の状態をカバーすることはできません。

- **OpenRouterプロバイダールーティング。** `OpenAIProvider` は、以下で渡されたオブジェクトを `CompletionParams::extra_fields.provider` から `provider`としてChat Completionsリクエストボディへ転送する。非オブジェクトの値はHTTPリクエスト前に失敗する。これにより、OpenRouterの文書化された per-call ルーティング設定が公開される一方、他のネイティブな `extra_fields` キーは無視される。ライブのBeast cookbookはプロバイダを固定し、4,000トークン生成予算に対して明示的な180秒タイムアウトを使用する。

- **Copy Ninja ローカルグラフノードブリッジ。** トランスポートフリーの `a2a::CopyNinjaNode`を追加しました。これは、別途マテリアライズされた Copy Ninja ハーネスをラップし、`prompt`を読み取り、`response`を上書きします。`cookbook_the_beast_copy_ninja` ライブcookbook を追加しました： その LLM はこの固定されたローカルノードのみをオーサリングでき、通常のCore ゲートの後に4番目のローカルバインディングゲートをパスする必要があり、合成ソースエージェントが RPC を観測した場合はフェイルクローズします。カードテキスト、エンドポイント、資格情報、ソースは未承認のキャンディデートから除外され、呼び出し元プロンプトがオーサリング LLM リクエストに入ることはありません。


- **オプションのProgramコンポーネント境界。** オプトインの`NEOGRAPH_BUILD_PROGRAM`スイッチ、エクスポートされた`neograph::program`ターゲット、および`<neograph/program/program.h>`エントリポイントを追加しました。インストールされたパッケージコンポーネントの検出は、ビルド時のみProgramを報告するようになりました。Coreのみのインストールでは、既存の`neograph::core`リンクインターフェースを維持します。

- **不変のProgram値モデル。** 安定した型付き診断、深く所有されたcanonical-JSONおよびC++ビルダー `ProgramSource` 入力を追加。不変のコンテンツアドレス指定 `ProgramBundle`/`ProgramVersion` 値、canonicalシリアライゼーション、SHA-256アルゴリズムタグ付きID、ソースマップ、インポート、および厳格なバージョン付き保存値スキーマ。 `neograph::program` は現在コンパイル済みのエクスポートライブラリであり、Coreのみに依存し続ける。Bundle/version v1プロジェクションは現在、封印されたCore定義とプランID、セマンティックバージョン付き実行可能ダイジェスト、契約、クロージャ、境界、および型付きadmission/materializationレシートを必要とする。それらのIDはフォーマットとストレージバージョンをバインドし、セマンティックセットは安定した順序を使用し、診断は無効なポインタ、逆転したスパン、未知の列挙を拒否しつつ、正確なオフセットが利用できない場合はパーサースパンを省略する。

- **Sealed Program の admission クロージャ。** イミュータブルな `RegistrySnapshot`、`AdmissionProfile`、および `PolicySnapshot` の値を追加しました。ビルダー時の呼び出し可能キャプチャ、厳密な正規マニフェスト、ドメイン分離されたフィンガープリント、および `ProgramVersion` におけるフェイルクローズのクロスフィンガープリント検証を備えています。Core は現在、Program の実体化のために、明示的に名前付けられたローカルのみの parse/link/validate エントリポイントを公開しています。既存のローカルファースト/グローバルフォールバックのオーバーロードは変更されません。レジストリエントリは現在、推移的 admission クロージャのための正規の正確な実行依存エッジを記録しており、ローカルのみの条件チェックは、プロセスグローバルレジストリを参照せずにレガシーキーエッジドキュメントをカバーしています。

- **単一ルート`call_core` Programコンパイラ。** `ProgramCompiler`を追加しました。これは閉じたProgram-v1エンベロープのみを受け入れ、シール前に純粋なローカルCoreパース/ラウンドトリップ/検証を実行し、RFC 6901ポインタとソースマップ帰属を備えた集約型の型付き診断を出力します。コンパイルは、ファクトリや呼び出し可能を起動することなく、正規のProgram、レジストリ、推移的実行可能クロージャ、機能/効果、インポートMerkle、シール済み定義、およびCoreプラン同一性を導出します。著者ドキュメントスキーマ、完全な有限予算契約、ゼロディスパッチ拒否テスト、静的および共有インストール済みコンシューマカバレッジがコンパイラに含まれています。Coreは加算的な全量パース/ラウンドトリップおよびローカル検証レポートを獲得しましたが、レガシーのthrowing APIは既存の動作を維持しています。

- **Pinned Program ランタイム垂直スライス。** `ProgramCatalog`、`EngineGenerationCache`、`ProgramRuntime`、共有の`ProgramHandle`、不変の`ProgramResult`、型付きのProgramイベントエンベロープ、インメモリの`ProgramStore`、そして追加専用CASの`ProgramJournal`を追加した。Admissionは、マテリアライズ前に未検証のバンドルセマンティクスを再計算する。各試行は不変のCore世代を1つピン留めし、既存の`GraphEngine`非同期パスを呼び出す。ランタイム実行は現在、完了、中断、正確なチェックポイント再開、キャンセル、タイムアウト、Coreステップ枯渇、チェックポイント非互換性、および失敗を型付き終端状態にマッピングし、更新不能なバジェットとチェックポイント系統を保持する。ジャーナルコミットはチェックポイント/終端イベント配信に先行し、同時再開には1つのCAS勝者のみが存在し、PR6スライスはCoreブローカーが存在するまで副作用のある、または空でないスキーマのProgramを拒否する。

- **QuickJS 制御言語フロントエンド。** オプトインの`NEOGRAPH_BUILD_QUICKJS_CONTROL`、シールドされた`ProgramSource::from_javascript(...)`、そしてプライベートなコンパイル専用のQuickJSコンテキストを追加した。ソース・エンベロープはエンジン/言語/ホストAPIのバージョンを固定する。その唯一の`ng`ホストサーフェスはバージョン管理されたグラフビルダーであり、メモリ、スタック、割り込みポーリングの制限はフェイルクローズする。JavaScriptは、1つの不変の`call_core` Program プランを生成し、ランタイムVM、バイトコードアーティファクト、またはCore依存関係になることは決してない。

- **A2A Agent Card 互換性候補。** ワンリクエスト、未認証、リダイレクトなしのwell-known-cardコレクタと、ファクトリー専用の不変候補コンパイラを追加した。候補は、ダイジェストピン留めされた来歴、有界なプロトコルファクト、安全なスキルIDのみを保持し、自由形式のカードテキスト、アドバタイズされたRPCエンドポイント、プロバイダー/セキュリティ設定、資格情報は除外される。Copy名PoCはさらに、そのダイジェストに固定された独立して観察された挙動を必要とし、ソースエージェントをディスパッチすることは決してない。

- **SQLite Harness レコードストア (issue #147 フォローアップ).** オプションの`neograph::mcp_sqlite`ターゲットと、WAL裏付け、スキーマバージョン管理されたアーティファクト/ラン永続化のための`SqliteHarnessRecordStore`を追加した。不変のアーティファクトとアーティファクトへのアーティファクト・バインドを追加した。Harness MCP バイナリは、現在、`runs.db`にレコードを格納し、チェックポイントは`checkpoints.db`に残る。
- **AMD OpenMP ターゲット・オフロードの概念実証。** 同一の数値 fan-out ワークロードで、シリアル CPU 実行、OpenMP 自動スレッド化、反復毎の GPU マッピング、永続 GPU データを比較する、オプトインの`bench_openmp_offload`ベンチマークを追加した。実デバイスとホストフォールバック実行、正確性、転送を含むレイテンシ、カーネルのみのレイテンシ、そしてスピードアップをレポートする。`NEOGRAPH_OPENMP_OFFLOAD_ARCH=gfx1201`は、Radeon AI PRO R9700のROCم/Clangデバイスイメージを可能にする。


### 変更

- **C++ ABI および SOVERSION ポリシー (issue #194)。** コンパイルされたすべての公開 `neograph_*` ライブラリは、プロジェクト `VERSION` とメジャー `SOVERSION` を保持します。インストールされた共有ライブラリは、自身のディレクトリから依存関係を解決します。v1 より前のリリースは ABI 世代 0 を使用しますが、必須の再ビルド境界を宣言することがあります。`0.11.1` 以前に対してビルドされたすべての C++ コンシューマは、次のリリースのために再ビルドする必要があります。`NodeCache`、`EngineConfig`、`CompletionParams`、`Agent`、`RequestOptions`、`SseEventParser`、およびプロバイダー構成の公開レイアウトが変更されたためです。境界付き `UsageAccumulator` 予約を含むリリースは、別の必須の再ビルド境界です。その公開オブジェクトのレイアウトには、予約会計状態が含まれるようになりました。バージョン 1.0 は、ABI 世代を 1 に変更し、サポートされる v1 レイアウトを固定します。CI は、静的および共有のインストール済みコンシューマの分離ビルドとテストを実行し、ELF/Mach-O ローダーメタデータをチェックします。[`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照してください。
- **`GraphNode::run(input)` マイグレーションガイド完了。** Python `GraphNode` 基底クラスは削除された `execute*` メソッドを参照しなくなりました。`run(input)` が欠落している場合、マイグレーションドキュメントのパスを含む `NotImplementedError` を送出します。C++/Python リファレンス、async/streaming ガイド、およびサンプル README は、実際の v0.9.0 単一エントリポイントに合わせて調整されています。マイグレーション手順は、[`docs/migration-v0.4-to-v1.0.md`](docs/migration-v0.4-to-v1.0.md) に C++ と Python の例とともに文書化されています。
- **Provider API 恒久互換性ポリシー (issue #5)。** `Provider::complete()`、`complete_async()`、`complete_stream()`、`complete_stream_async()`、およびコールバックベースの`invoke()`の削除予定は撤回され、`[[deprecated]]`警告は削除されました。既存のAPIは引き続き互換性とセキュリティ修正を受けます。新規のProvider実装と直接呼び出し元には、それぞれ`CompletionProvider::do_invoke()` と`invoke_request(CompletionRequest)`の使用が推奨されます。既存APIへの全新機能のバックポートは保証されません。パブリックシグネチャ、仮想順序付け、オブジェクトサイズ、vtableは変更されていません。

### 削除

- **非推奨のTransformerCPP統合例。** 削除されました`example_inproc_gemma`、`NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE`、および`TRANSFORMERCPP_DIR`。これらは、利用できなくなった外部ホスト型リポジトリに依存していました。`example_local_transformer`は、標準のOpenAI互換ローカルサーバーを使用しており、保持されています。

### 修正済み

- **Python Program wheel の依存関係を完結。** PyPI ビルドは QuickJS 制御ランタイムを有効化し、拡張モジュールの隣に `neograph_program` ローダーを同梱し、Windows の Program/Harness DLL 境界を越えて使用される非公開評価器をエクスポートするようになりました。これにより、`P_JS_UNAVAILABLE`、ローダー不足による import 失敗、および共有ビルドの `LNK2019` 失敗を防ぎます。また、コマンド identity の構築ではコンポーネント dylib 境界を越えるネストした initializer-list JSON コピーを回避し、Program 実行中に macOS arm64 で発生し得たクラッシュを防ぎます。Windows wheel の repair は外部 DLL 名をハッシュでマングルし、先に読み込まれたシステムの `sqlite3.dll` が同梱ビルドを上書きして `SQLiteContextStore` の構築中にクラッシュすることを防ぎます。
- **境界付きのリモート輸送と資格情報の起源。** HTTP/1.1、HTTP/2、SSE、WebSocket の受信は、信頼されないサイズが割り当てられる前に、保守的な応答、ヘッダー、チャンク、ライン、フレーム、ハンドシェイク、メッセージ制限を強制する。リダイレクトされた POST リクエストは、正規化された同一オリジン内でのみフォローされ、プロバイダーの資格情報は、明示的な数値ループバック開発例外が有効でない限りTLS を必要とし、WebSocket デバッグ出力には、リクエストヘッダーやペイロードは含まれません。
- **QuickJS `all` join 起動時レース。** 完了ハンドラは、JavaScript の join を閉じる前に、初期メンバー起動登録を待つようになりました。即座に完了する子は、その兄弟の初期コマンドまたは置換コマンドがディスパッチされる前に、ジェネレーターを再開できなくなりました。繰り返しのランタイムリグレッションは、両方のパスをカバーしています。
- **Harness 集約ファインディングの来歴（issue #174）。** 詳細には、 `finding_sources` 配列が既存のフラットな `findings` 配列に揃えて含まれるようになりました。各エントリは、スキーマ検証済みのワーカー出力や確立された `findings` 形状を変更することなく、その集約インデックス、ソースワーカーID、およびワーカーローカルインデックスを記録します。
- **Harness エクスポート結果 lint (issue #173)。** Node effect 契約では、呼び出し側がグラフ実行後にそれらを消費する場合、オプションの `exports` 配列で書き込みチャネルを宣言できるようになりました。したがって、Harness のコンパイルと `GraphEngine` のランタイム検証の両方が、真に書き込み専用のチャネルに対して E6 を維持し、`final_result` に対して誤って警告することはありません。
- **MCP 2025-11-25 ツールクライアント契約の近代化（issue #147 M0）。** 初期化は現在冪等であり、ネゴシエーションされたサーバーメタデータを保持します；HTTPツールはディスカバリーセッションを再利用します； `/mcp` エンドポイント構築はリクエストと通知で共有されます；ツールディスカバリーは不透明なカーソルに従います；そしてJSON-RPCのcode/data、完全なツールメタデータ、非テキストコンテンツ、 `structuredContent`, `isError`、および `_meta` はC++とPythonのパスで存続します。構成可能なHTTPタイムアウト/静的/動的ヘッダー、出力スキーマ検証、厳格なレスポンスIDチェック、および型付き `InitializeResult`, `ToolDefinition`, `ListToolsPage`、および `CallToolResult` APIsが追加されました。SSE検出は現在、 `Content-Type` を使用し、 `data:` URLを含むJSONを誤分類することはありません。
- **タスクごとのキャンセルステータスと公開emitのライフタイム安全性。** `GraphEngine::run`、`run_async`、`run_stream`、`run_stream_async`はそれぞれ、呼び出し元が提供する親から実行ごとに1つの実行子を作成し、その子のみを内部の`co_spawn`/同期ブリッジにバインドし、同じ子を`RunContext`として渡します。したがって、単一の親の下で並行するすべての実行をキャンセルしても、互いのキャンセルスロットを上書きすることはできません。フォークされた実行子は、既存の`shared_ptr`の所有権を公開されたemitを通じて保持し、エンジン作業完了とemit実行の間でのuse-after-freeを防止します。キャンセルによって引き起こされるasio `operation_aborted`は、再試行可能なノードエラーではなく`CancelledException`として伝播されます。`CancelToken` 0.11.xのオブジェクトレイアウトとインライン/ヘッダー専用の動作は変更されていません。更新された`fork()`のライフタイム動作を反映するには、すでにコンパイルされたC++コンシューマーの再コンパイルが必要です。共有ライブラリのみを置き換えるとオブジェクトレイアウトの互換性は維持されますが、コンシューマーバイナリに埋め込まれた既存のインライン関数本体は変更されません。ただし、外部コードが直接作成したトークンに対して`bind_executor()`を呼び出す場合、エグゼキューターの投稿された作業が完了するまでトークンを生かし続ける責任は呼び出し元に残ります。
- **PostgreSQL非同期接続のグローバルタイムアウトポリシーを文書化。** 非同期の初期接続と置換では、すべてのホスト/IPアドレスに対して単一のタイムアウトが使用されます。正の接続文字列に直接書かれた明示的な `connect_timeout` は、最低2秒で適用が強制されます。未指定、ゼロ、負の値、または環境変数/サービスファイル専用の値は、運用上安全なデフォルト（30秒）が使用されます。これは、libpq のホストごとの同期タイムアウトと意図的に異なりますが、同期の作成/置換動作は変更されません。
- **JARVIS mock ビルド修正（issue #130）。** 修正しました `cookbook_jarvis` のコンパイル失敗。原因は `MicCapture` がオーディオ依存関係が存在しない場合に不完全型のままであることでした。追加しました `NEOGRAPH_JARVIS_FORCE_MOCK` これにより、ASan CI はランナーにインストールされたパッケージに関係なく、常に mock 構成をビルドします。セッションランナーは現在、実際の CMake 出力パスと専門ターゲット名を使用し、既存の `demo_mcp_server.py` を正しく起動します。
- **ノード障害コンテキストの保持（issue #123）。** C++実行エラーは、 `NodeExecutionError` 元の `exception_ptr`、失敗したノード名、および試行回数を含む形で伝播されます。終端の `ERROR` イベントにも同じコンテキストが記録されます。Pythonでは、元の例外オブジェクト、型、引数、ユーザー属性、およびトレースバックがそのまま保持され、追加されるのは `.node_name` と `.attempts` 属性のみです。 `NodeInterrupt`、キャンセル、およびメモリ不足の例外は、ラップされずに既存の制御フローに従います。

### 修正済み（ドキュメント）

- **Providerクックブックから無視されていたノード単位のプロンプトを除去しました（issue #116）。** 以下の使用に関する説明をした3つのPythonサンプルを修正しました： `config.system` 組み込みの `llm_call` で読み取られない複数役割の動作。各サンプルは、 `NodeContext.instructions`を使用した厳密な単一呼び出しグラフとして書き直され、関連するREADMEも実際の動作に合わせて整えられています。
- **予約済み`RunContext::deadline`ドキュメント修正（issue #115）。** `deadline`と`trace_id`を実行ごとのメタデータとして使用可能であると提示していたドキュメントとDoxygenコメントを修正しました。これらは`RunConfig`を介して設定できず、Pythonでは公開されていません。
- **`GraphNode::run`例のシグネチャ修正（issue #129）。** `const NodeInput&`（参照渡し）を受け入れる公開ヘッダーの例を修正しました。これは実際の値渡し仮想関数のオーバーライドに失敗しており、コルーチン引数の有効期間に必要な値渡し契約をコンパイル時テストで固定しました。

### 追加

- **後方互換性のあるProvider移行パス。** 新しい`CompletionRequest`はストリーミングモードをコールバックの存在から分離し、`CompletionProvider`は新しい実装が`do_invoke()`のみを書き込むことを要求します。既存の`Provider`vtable、4つのレガシー仮想関数、コールバックベースの`invoke()`、およびPythonの`complete()`サブクラス契約は保持されます。

- **Python永続化バックエンド**（#117）— `Store`と`CheckpointStore`は現在、C++仮想ディスパッチをPythonに持つ構築可能なサブクラスベースです。`StoreItem`、`CheckpointPhase`、`Checkpoint`、および`PendingWrite`はJSON形式のフィールドで公開されています。チェックポイントの保留中書き込みメソッドはオプションのままです。
- **Python同期キャンセル**（#119）— Python呼び出し元は`CancelToken`を構築し、それを`RunConfig.cancel_token`に割り当て、別のスレッドから`engine.run()`を協調的に停止できます。

- **Pythonチェックポイント履歴**（#118）— `GraphEngine.get_state_history()`は新しい順のチェックポイントレコードを公開するため、呼び出し元は履歴状態からフォークする前に親リンク、メタデータ、ステップ、IDを検査できます。

- **DSLサーフェス（精緻化レイヤー）+ スキーマ進化ゲート**（#75 M4）。
  - **エラボレータ**：`vars`（`{"$var":...}` / `${...}` 補間、非巡回強制）/ `templates`+`use`（パラメータ完全一致の強制、ノードプレフィックスリネーム—ローカル参照、バリア、ルートを含む; チャネルは共有状態であるためグローバルにマージされる）/ `when` 条件付き組み込み。**チューリング完全ではなく全関数である**：すべてのDSLドキュメントは有限時間で一意のCoreに正規化され、そのCoreに関して冪等である。すべてのエラーはDSLソース座標（`use[2].args`、`vars.model`）とともに報告され、ソースマップ（出力位置→生成構文）が含まれる。ロックファイルワークフロー：`./example_elaborate harness.dsl.json > harness.json`（例53）。
  - **`GraphCompiler::upgrade_to_latest()`**: 可逆的なv0→v1機械的変換 — strictが拒否するキーは`x-upgraded-<key>`コメント名前空間に分離され（データ削除ゼロ）、空のバリアは明示的に削除される。コーパス全体がテストされ、「レガシー寛容コンパイルIR == アップグレード後strictコンパイルIR」（正準等価性、バージョンスタンプは除く）が保証される。
  - **スキーマ進化ゲート**：`tests/fixtures/schema_snapshot.json` ベースラインに対する追加のみの部分集合判定（JSON Subschema ファミリの決定可能な部分集合）—ノードタイプ/プロパティ/リデューサー/条件の削除、required 集合の増加、閉条件ラベルの変更、およびエフェクトコントラクトの変更はいずれもテスト失敗 = CI マージブロックを引き起こす。非互換な変更は、同じレビューコミット内でバージョンバンプ + アップグレーダー + スナップショット再生成を強制する。

- **PBT / デルタ検証ハーネス**（#75 M3）。300シードの決定論的トポロジージェネレータ（スキーマエンベロープからの有効な厳格なドキュメント、自己計測された特徴カバレッジ—conditional_edges/barrier/interrupt の出現率が30%を下回るとテストが失敗する：テストされていない特徴は失敗になり、静かな穴にはならない）。
  - **変異検出**: 300シードのコーパスで、変換検証が5種類すべてのドロップ型(conditional_edges/edge/barrier/interrupt/channel)と3種類の誤配線型(ルート崩壊/エッジ再ターゲット/ノード改名=ドロップ+捏造の相殺)の全適用を捕捉することを確認した。適用率の下限(シードの10%)も検証した。
  - **参照インタプリタのデルタ**：コードが分離された実装から文書化されたスーパーステップセマンティクス（goto preemption、バリア蓄積、辞書式フォールバック、暗黙的__end__）を再実装する独立モデルで、12ステップ× 300グラフでスケジューラと比較される（DESILの教訓：単独の検証パイプラインでは誤った実行条件を把握できない）。
  - **Engine ↔ Studio 共有コーパス**: `tests/fixtures/topology_corpus/` 15のバリアント（3つの有効 + E3–E11に違反する12）はNeoGraph-Studioとバイト単位で同一である `tests/corpus/`、両者は同じ判定（code:severity 多重集合）を主張する — 2つの実装は暗黙的に乖離することはできない。

- **GraphValidator — トポロジー静的意味チェック (E3–E11 + 効果)** (#75 M2)。パース (M1) と実行の間のパスレイヤー。厳密なドキュメント (schema_version>=1) では、エラーはコンパイル失敗、警告は stderr の lint です。寛容なドキュメントでは、エラーレベルの診断のみが stderr 警告として表面化します (既存のグラフではノイズゼロ)。判定哲学 = チェッカーの健全性を優先: エンジンセマンティクスで決して正しくなり得ないものだけがエラーです (未解決参照 E3、シグナルパスのないバリア E8 — goto はバリア会計をバイパスするため回復不能、空ルート E10 — ディスパッチは rend() UB を逆参照する、未宣言チャネル書き込み E4 — ランタイムで送出が確認済み)。Command.goto/Send が正当化できるものは警告です (到達可能性 E7、脱出不能サイクル E11、バリアなしのプレーン fan-in E9、上書き競合 E5、デッドチャネル E6)。すべての診断には、マシン可読な証拠 (反例) JSON が付随します — Studio キャンバスのハイライト用 (M3)。
  - **ルート完全性（E10）**: `ConditionSpec` ラベルコントラクトを導入。`register_condition` の3引数オーバーロードを介して条件の出力ラベルセットを宣言する場合、クローズド条件ルートがラベルと正確に一致することが要求される — カバーされていないラベルはスケジューラの「辞書式順序最後のルート」フォールバック（順序依存の任意ターゲット）に落ち、これはエラーである。組み込みの `has_tool_calls` = クローズド {false,true}、`route_channel` = オープン + 既知の {default}。
  - **チャネル効果契約**: `register_type` の4引数オーバーロードは、ノードタイプごとの読み取り/書き込みチャネルを宣言する。E4/E5/E6分析は、グラフ内の**すべての**ノードタイプが宣言されている場合にのみ有効化される（単一の不明なタイプで分析全体がスキップされる — カバレッジに対する健全性）。組み込みの3タイプ（llm_call/tool_dispatch/intent_classifier）は完全に宣言されている。
  - `node_effects` · `condition_specs` を `export_schema()` に追加（既存の `conditions` 配列は後方互換用に維持）。新規テスト22件。

- **トポロジーコンパイル時整合性ゲート — 消費キー会計 + 翻訳検証** (#75 M1)。「サイレントな意味的損失」クラス (v0.1.0–v0.1.7 `conditional_edges` サイレントドロップと同種) を構造的にブロックする二重メカニズム:
  - **消費キー会計（Consumed-key accounting）**: 以下の文書で宣言されている `"schema_version": 1` は厳密コンパイルに切り替わる — 未消費キー（タイポ `conditionnal_edges`、未対応フィールド、空の `wait_for`によって静かに破棄されるバリア、インライン条件分岐で無視される `to` ）はすべて収集され、コンパイルエラーとして報告される。マーク付けはパースブロックの**内部**で行われるため、パース段階を消去するとマークも消去され、それらの機能を使用する厳密なドキュメントは直ちに失敗する — ドロップ回退（回退 regressions）が決して静かにならない構造である。 `_`/`x-` 接頭辞を持つキー（`_comment`, `x-studio-*`）は常にコメント名前空間として許可される。既存のドキュメント without `schema_version` は寛容な動作（バイト保存）を維持し続ける。
  - **翻訳検証**: `CompiledGraph::to_json()` 再出力 + `GraphCompiler::canon()` 正規形チェック `canon(input) ==
    canon(re-emit)` をすべてのコンパイルで実施。不一致 (= コンパイラが何かを落とした、または誤配線した) は、厳密なドキュメントでは throw、寛容なドキュメントでは stderr 警告になります。等価性は構造比較です — ルートキーの入れ替えのような誤配線も検出されます (存在比較では見逃されるクラス)。
  - `NodeFactory::config_schema(type)` query が追加され、 `schema_version` フィールドが `export_schema()`に文書化されました。27件の新しいテスト (`tests/test_compiler_strict.cpp`) — v0.1.x ドロップミュータントシミュレーション (conditional_edges/barrier/interrupt のドロップ)
    + route miswirings) included.

## [0.11.1] - 2026-06-25

### 変更

- **stdio MCP並行呼び出し — I/Oオーバーラップ用の相関IDデマルチプレクサ。** `0.11.0`の並行ツールディスパッチは、実際にはHTTP MCPのみがオーバーラップしていた。stdio MCPは`StdioSession::rpc_call_async`において、**リクエスト→レスポンスの往復全体**にわたって容量1のチャネルロックを保持しており、単一のセッションパイプを通じて1ターン内の複数呼び出しを直列化していた(ウォールタイム≈レイテンシの合計)。単一パイプが根本原因ではなかった — JSON-RPC `id`は、まさに1つの接続上でパイプライン処理を行うために存在する。ロックを相関IDデマルチプレクサに置き換えた:
  - 容量1のチャネルを write-only ロックとして再利用 — フレーム書き込みの瞬間のみ保持されるため、2つの呼び出しのバイトは絶対に interleave せず、読み取りはもはや serial 化されない。
  - 単一のリーダーコルーチン(`run_reader`)が読み取り側を排他的に所有し、JSON-RPC `id`を介して各レスポンス行を正しい呼び出し元のシンクに配信する。N個の並行呼び出しは読み取りをオーバーラップさせるため、ウォールタイム≈最大(レイテンシ)となる — ただし、**ピアMCPサーバーが並行処理する場合のみ**(シングルスレッドの逐次サーバーはAmdahlの限界に達する)。
  - リーダーは、実行中の呼び出しが存在する間のみ遅延実行され、待機者がいなくなると終了するため、プライベートな`run_sync` io_contextは正常に戻る。待機者は、呼び出し元が待機している間のみ存在し、`MCPTool`の`shared_ptr`を介してセッションを維持するため、リーダーが破棄されたセッションに触れることはない(デストラクタのjoinは不要)。パイプのEOF/エラー時、リーダーはすべてのシンクを閉じるため、待機中の呼び出し元は無限にハングする代わりに例外を受け取る。
  - **API/構文の変更なし** — 公開ヘッダーは変更なし、既存コードの再コンパイルは不要。エンジンオーバーヘッドの回帰は0(`bench_neograph`のインターリーブA/B、seq/par Δ 0%)。
  - テスト: スレッドベースの遅延フィクスチャ`tests/fixtures/mcp_stdio_slow.py` + `ConcurrentStdioCallsOverlapIO`(5×100 msの呼び出しが約130 msで完了、500 msの直列下限に対して; 各レスポンスが`id`を介して呼び出し元にルーティングされることを検証)。ASan+UBSan ×3クリーン。

## [0.11.0] - 2026-06-25

### 追加

- **並行ツールディスパッチ — `Tool::execute_async` 公式非同期パス。** `ToolDispatchNode` は単一のアシスタントターンから複数の `tool_call`をエンジンの `make_parallel_group`を使用して**並行して**実行します。以前は各呼び出しが同期 `execute()`を介して順次実行され、MCPツールは特に、呼び出しごとに `io_context` を `run_sync` 経由で生成する際にブロックされ、並行MCP呼び出しの重複を妨げていました（並行MCP呼び出しを持つ外部C++フォークで発見）。修正:
  - 仮想`execute_async()`を`Tool`に追加 — デフォルト実装は同期`execute()`にブリッジするため、既存ツールは変更なしで動作する。
  - `MCPTool`を`AsyncTool`に変換し、ネイティブ`execute_async`を使用(stdioは`rpc_call_async`、HTTPは新しい`MCPClient::initialize_async`/`call_tool_async`を非同期ハンドシェイクに使用 — `run_sync`は削除)。
  - `ToolDispatchNode::run`は、ノードfan-outと同じ`make_parallel_group`イディオムを介して呼び出しを並行してディスパッチし(単一呼び出しはインライン化)、結果は呼び出し順に適用される。同期`execute()`ファサードを介して後方互換。
  - 検証: 478/478 ctest、Valgrind 0 リーク、TSAN 0 競合。

### 修正済み

- **Python非同期実行の例外保持(issue #122)。** `run_async`、`run_stream_async`、および`resume_async`が、元のPythonノード例外を文字列としてラップする新しい`RuntimeError`で上書きする問題を修正。現在、元のPython例外オブジェクト、型、ユーザー属性、トレースバックはpybind11の標準例外変換パスを通じて保持され、C++ `py::type_error`は同期実行と一致するPython `TypeError`として配信される。`resume_async`の空のコールバックは、コルーチンが完了するまで保持され、pybind11 3.xで露呈したダングリング参照の競合も修正。

### 修正済み（ドキュメント）

- **README 概要バッジを、サンドボックス測定で明らかになった欠落条件と内部矛盾について修正しました。**「4つの軸」の概要表バッジは、本文/詳細から測定条件が取り除かれ、誇張して読めていました。本文の測定値と条件に一致するように修正しました（測定データ表自体は変更なし）：
  - **`p99 17 µs flat` → `p99 7 µs @ 10 K (1 CPU sandbox)`** — バッジの17 µsは本文(`At N=10,000 concurrent ... 7 µs p99`)と矛盾し、`flat`はGPU負荷テストの実行レイテンシ(648 ms)を説明しており、µs測定ではなかった。バッジを本文の測定値と条件に合わせた。
  - **`1.2 MB stripped binary` → `... (MinSizeRel static)`** — `libc.so.6`専用で1.2 MBという条件は、MinSizeRel + 静的libstdc++ビルドにのみ当てはまる（デフォルトのReleaseはlibstdc++/libgcc_s/libm/libcを動的リンクする）。deep-dive §sizeに既に記載されている条件をバッジに復元した。
  - **`2 wheel deps` → `2 direct wheel deps (... ; 7 with transitive)`** — 直接依存は確かに`certifi` + `pydantic`（2つ）だが、実際のインストールツリーはpydanticの推移的依存（pydantic-core、typing-extensions、annotated-types、typing-inspection）を含む7パッケージである。
- **deep-diveのMinSizeRel再現コマンドに`-DNEOGRAPH_BUILD_POSTGRES=OFF`を追加。** PostgreSQLはデフォルトでONのため、libpqを持たないホストではそのまま実行するとconfigureが失敗する。修正した。

## [0.10.0] — 2026-05-20

### 追加

- **シリアルfan-outのワンショットstderr警告（issue #62、PR #63）。** `compile()`のデフォルトは`set_worker_count(1)` — fan-outブランチはエンジン所有のスレッドプールなしで呼び出し側のexecutor上でシリアルに実行される。この意図された動作は、ドキュメントのみに基づいてマルチSendグラフを構築したユーザーには無言のシリアル実行に見える。`NodeExecutor`がプールなしでマルチSend（またはマルチ出力エッジ）fan-outをディスパッチする最初の回に、ワンショットのガイダンスメッセージをstderrに追加した。`std::atomic` + compare-exchangeにより、並行fan-out下でも正確に1回の出力が保証される。`set_worker_count(N>=2)`を呼び出すと`NodeExecutor`が再構築され、フラグは自然にリセットされる。環境変数`NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`（または`true` / `yes`）で抑制可能 — 意図的なworker=1シリアル実行、ベンチマーク、CIのstderrアサーションケース向け。Linux + macOSの5つのユニットテスト（`test_fanout_worker_warning.py`）でカバー：発火 / ワンショット / プールオプトインの沈黙 / 環境変数の沈黙 / シングルSendの警告なし。Windows：pytest capfdはホイールバイナリのMSVC CRT fdキャッシュと非互換のためモジュールレベルでスキップ — ホイールバイナリのstderr出力自体は正常である。

- **トポロジーJSON Schemaエクスポート — `NodeFactory::export_schema()`**（issue #56、コード不要のビジュアルブロックエディタの前提条件）。エンジンが消費するトポロジーJSON形式を、機械可読なスキーマ（JSON Schema Draft 2020-12）として1つの`{ neograph_version, $schema, topology (fixed
  envelope), node_types, reducers, conditions }`にエクスポートする。別リポジトリのブロックエディタはこのスキーマからパレットを自動生成する → エディタとエンジンはバージョン間でドリフトできない。完全に追加的：
    - `NodeFactory::register_type(type, fn, json config_schema)` 3-argument
      variant added. Existing 2-argument delegates to permissive default
      schema — existing user nodes/calls unaffected.
    - `ReducerRegistry::names()` / `ConditionRegistry::names()` /
      `NodeFactory::registered_types()` query accessors added.
    - Configuration schema declared for 4 built-in types (`llm_call`/
      `tool_dispatch`/`intent_classifier`/`subgraph`). `NEOGRAPH_VERSION`
      exposed as a compile definition (pyproject.toml single source of truth)
      → schema version stamp.
    - `examples/52_export_schema.cpp` (`example_export_schema`):
      `./example_export_schema > schema.json` — standard path for the editor
      repo CI to produce the artifact pinned to a NeoGraph version.
    - Python: `neograph_engine.export_schema()` → dict (editor repo CI
      dumps after `pip install neograph-engine`).
    - `tests/test_schema_export.cpp` 8 + `test_export_schema.py` 4. Key:
      top-level `conditional_edges` surviving the loader→compile round-trip
      (regression guard against v0.1.0–v0.1.7 silent-drop recurrence).

### 修正済み

- **トポロジー最上位コンテナ形式の検証（#126）。** `channels`/`nodes`はオブジェクトでなければならない。そうでない場合は全モードで拒否される。`edges`/`conditional_edges`の配列検証はstrictモードで強制され、レガシーのキー付きエッジマップ互換性は維持される。エラーはパスとJSON型を報告し、入力全体は報告しない。
- **`max_steps`終了状態の公開（#114）。** `RunResult::max_steps_exhausted()`と読み取り専用のPythonプロパティ`RunResult.max_steps_exhausted`を追加。`max_steps`に到達し、実行すべきノードが残っている場合にのみTrueになる。同じ状態がgRPCの単一応答とストリーミング最終JSONの両方で提供される。C++構造体のサイズは変更なし。

- **`set_worker_count` / `set_worker_count_auto`のdocstring修正（issue #62、PR #63）。** v1.0準備サイクルは意図的に`compile()`のworkerプールデフォルトを`set_worker_count(hardware_concurrency())`から`set_worker_count(1)`に戻した（`src/core/graph_engine.cpp:69-93`のコメントに根拠あり）が、4つのユーザー向けdocstringには古い記述が残っていた → ドキュメントを信じてマルチSend fan-outグラフを構築したユーザーは、単一スレッドで無言のシリアル実行を得た。ユニットテストでは見えない（モックのspawn、即時ボディ）。実時間のe2eでのみ露呈する。
  - `set_worker_count` / `set_worker_count_auto`の両方のPython docstringを`bindings/python/src/bind_graph.cpp`で実際の動作に合わせて書き直した：`compile()`のデフォルトは1、`set_worker_count_auto()` / `set_worker_count(N>=2)`は明示的なオプトインである。
  - `include/neograph/graph/engine.h`の両方のDoxygenコメントをそれに合わせて修正した。Doxygen Pagesはmasterへのpush時に自動再構築される。
  - `docs/concepts.md` / `docs/troubleshooting.md` / `docs/reference-en.md`の同じ古い記述（デフォルト = hardware_concurrency）を修正した。

- **v0.9.0から欠落していた3つのAPI移行を補足して出荷。** v1.0準備サイクルのPR `9b`（`19819d8`）は`GraphNode`のレガシー8仮想チェーンを破壊的に削除したが、PR `#48`（`6e654ad`、「C++ examples migrate to `GraphNode::run()`」）は`examples/`のみを移行した — 以下の3ファイルが漏れ、v0.9.0がビルド破損状態で出荷された：
    - `benchmarks/stress/bench_sustained_concurrent.cpp` (Phase 3
      sustained-burst verification key benchmark)
    - `benchmarks/concurrent/bench_concurrent_neograph.cpp` (memory/
      concurrency comparison matrix body against LangGraph and other engines)
    - `wasm/smoke.cpp` (Phase 1 WASM feasibility smoke)

CIは、これらのターゲットをadd_executablesとして検出せず、(Dockerビルド依存関係により)別の環境に分離したため、masterへのマージとタグ付けは通過した。

**修正**: 3つすべてが `std::vector<ChannelWrite> execute(const
  GraphState&) override` → `asio::awaitable<NodeOutput> run(NodeInput in)
  override` + `co_return out` パターンから移行。ノードロジックは変更なし。

**v1.0 主要セールスポイントのネイティブ再検証** (`benchmarks/concurrent/results_v0.9.0_native_recheck.jsonl`):
    - Concurrency 10K · wall 10–23 ms · p99 17–21 µs · peak RSS **5.6 MB**
      (matches v0.3.0 / v0.5.0 measurements — no memory selling-point
      regression after destructive 9b)
    - 0 errors at 10K
  **Docker matrix (LangGraph / Haystack / pydantic-graph / LlamaIndex /
  AutoGen 6-way comparison) also re-measured within the same session**
  (`results_v0.9.0_docker_recheck.jsonl`).

マトリックス再実行中に、欠落していた API 移行と並行して独立した回帰が1件発見された — `benchmarks/concurrent/Dockerfile.neograph` は master 上の CMake オプションのデフォルト変更を追跡できなかったため、まったくビルドできなかった（v0.9.0 出荷時も同様）。時間の経過とともに、以下のオプションのデフォルトが OFF → ON に反転した:
    - `NEOGRAPH_BUILD_POSTGRES` / `NEOGRAPH_BUILD_SQLITE`
      (requiring `libpq-dev` / `libsqlite3-dev` respectively)
    - `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP`
    - `NEOGRAPH_USE_LIBCURL` (one prior incident closed in
      `feedback_libcurl_unconditional_dep.md` — only the option toggle was
      added while the default remained ON, breaking the empty-container build
      path again)
    - `find_package(OpenSSL REQUIRED)` is unconditional without an option
      toggle (CMakeLists.txt:256) — separate v1.0 cleanup candidate

**Dockerfile 修正**: `libssl-dev` apt 追加 + すべての非 Core オプションを明示的な `-DNEOGRAPH_BUILD_*=OFF` / `-DNEOGRAPH_USE_LIBCURL=OFF` で固定。コメントには「2件のドリフト事故による明示的な凍結」と記載。`find_package(OpenSSL REQUIRED)` の CMakeLists.txt での条件化は別タスクとして残す — 他のビルドパス（PyPI wheel、ARM64 など）への影響検証が必要。

**6方向マトリックス主要結果** (並行度=10000, 2 CPU / 1 GiB):

    | engine（エンジン）          | モード          | wall_ms | p99_us      | peak_MB | ok/err |
  |---|---|---|---|---|---|
    | **neograph**    | スレッドプール    | **16**  | **18**      | **5.1** | 10000/0 |
    | pydantic-graph  | asyncio       | 895     | 160         | 42.8    | 10000/0 |
    | haystack        | mp-pool-8     | 1472    | 2972        | 68.3    | 10000/0 |
    | langgraph       | mp-pool-8     | 3802    | 74415       | 60.6    | 10000/0 |
    | autogen         | mp-pool-8     | 22428   | 82361       | 49.1    | 10000/0 |
    | LlamaIndex      | asyncio       | 26303   | 25912204    | 582.7   | 10000/0 |

NG vs LangGraph（マーケティング比較軸): **237倍高速**、p99 **4134倍高速**、ピークRSS **12倍低減**。

**過酷なシナリオ**(並行度=10000、1 CPU / 512 MiB):
    - NG: 8 ms / 5.2 MB / 0 err / **ok**
    - LangGraph mp-pool-8: 7821 ms / 60.9 MB / 0 err / ok
    - **LlamaIndex asyncio: OOM killed** (exceeded 512 MB cap)
    - **AutoGen asyncio: OOM killed**

Token: ⟦c39661f30e9e⟧ 同じv0.3.0/v0.5.0の測定値 — **破壊的な9bの後も、NeoGraphの「10K同時ワーカー、ピークRSS 5 MB、OOMなし」という売りポイントにリグレッションはありません。**

## [0.9.0] — 2026-05-14 — v1.0 準備 (Candidate 1 Phase B + Candidate 6)

ROADMAP_v1.mdの2つのv1.0単一ディスパッチ統合が1つのサイクルで収束する：

  - **候補 1 フェーズ B (`9b`–`9f`)** — `GraphNode` のレガシー 8 仮想関数 (`execute` / `execute_async` / `execute_stream` / `execute_stream_async` / `execute_full` / `execute_full_async` / `execute_full_stream` / `execute_full_stream_async`) + `add_cancel_hook` + `CurrentCancelTokenScope` + `state.
    run_cancel_token_` + すべての 6 つの `PyGraphNodeOwner` レガシーオーバーライドが削除された。**破壊的** — 非推奨期間は終了。ユーザーの GraphNode サブクラス / ユーザー Python ノードは、単一メソッド `run(NodeInput)` / `def run(self, input)` に移行する必要がある。
  - **候補 6** — `Provider` 4仮想関数のクロス積 → 1仮想関数 `invoke()`。まだ追加 + 非推奨フェーズ中 — レガシー 4 仮想関数は変更されず機能し、非推奨警告のみ表示される。その側のフェーズ B (`Provider` レガシー削除) も v1.0.0 出荷直前に終了する。

同じサイクルには、b59444f の並行回帰リバートの可能性 (`e5ecb08`) + 明示的な fan-out の例呼び出し + 3 件の CI 環境修正 (httplib マクロガード / Windows MSVC unistd.h / pybind pytest 移行) も含まれ、すべてこの [Unreleased] の一部である。

### 追加

- **`Provider::invoke(params, on_chunk = nullptr)`** — v1.0 標準の単一ディスパッチエントリポイント。非ストリーミング (`on_chunk == nullptr`) とストリーミング (`on_chunk` 提供時) の両方を 1 つのメソッドで処理する。以前の 4 仮想関数のクロス積 (`complete` / `complete_async` / `complete_stream` / `complete_stream_async`) を 1 つの非同期ストリーミングスーパーセットに統合する。デフォルト実装は 4 つのレガシー仮想関数に転送するため、既存の Provider サブクラスは変更なしで動作する。新しい ctest 6 件 (`ProviderInvokeDefault`)。(PR #40)
- **`invoke()` キャンセル伝播のパリティ** — `params.cancel_token` が設定されておらず、エンジンのスレッドローカルスコープがアクティブな場合、`current_cancel_token()` が自動的にスタンプされる。レガシー同期 `complete()` の動作と同等 (エンジン内のノード本体が `provider->invoke(params, ...)` を呼び出すと、実行中のグラフのキャンセル信号を自動的に受信する)。新しい ctest 3 件 (`InvokeCancelPropagation`)。(PR #43)
### 変更

- **エンジン内のすべての内部 LLM 呼び出しが `invoke()` 経由にルーティング** — `LLMCallNode`、`IntentClassifierNode` (PR #41/#42)、`Agent::complete` / `Agent::run_stream` (PR #43)、`SupervisorLLMNode` / `ResearcherLLMNode` / `CompressNotesNode` / `FinalReportNode` (PR #43)、`PlannerNode` / `ExecutorNode` (PR #44)。NeoGraph 内の LLM ディスパッチが単一のサーフェスに統合された。
- **C++ 例の移行 (2 ファイル)** — `31_local_transformer.cpp`、`cookbook/ai-assembly/member_server.cpp` は新しい `invoke()` を使用するようになった。ユーザービルドで非推奨警告は発生しない。(PR #45)
- **`GraphEngine::compile()` デフォルトワーカー数を1に戻した** (`e5ecb08`)。`b59444f` は、潜在的な18日間（2026-04-26 → 2026-05-13）の並列マイクロベンチ回帰 11.8 → 283 µs（24倍）の根本原因であり、コミットは二分探索（11のワークツリーを並列使用）で特定された。v1.0 からデフォルト=1（CPU負荷の小さい逐次/並列ディスパッチに最適）。意図的な fan-out には、`engine->set_worker_count_auto()` に1行追加して hardware_concurrency を開放する。影響を受ける5つの fan-out サンプル（10/14/21/36 + deep_research_graph ビルダー）に明示的な呼び出しを追加した。詳細は ROADMAP_v1.md の「Perf retrospective」セクションを参照。

### 非推奨

- **`Provider::complete` / `complete_async` / `complete_stream` / `complete_stream_async`** — 4つのレガシー仮想関数すべてに `[[deprecated("v1.0 single-dispatch: use invoke(...)")]]` マーカーが付いている。レガシーメソッドは非推奨期間中はそのまま機能する。v1.0.0 で削除。内部フォワーダーは `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` でラップされ、警告はユーザー向けのオーバーライド/呼び出しサイトでのみ表示される。(PR #44)

### 削除済み (候補1 フェーズB — 破壊的)

- **`GraphNode` レガシー仮想関数8つ** — `execute(GraphState&)` / `execute_full(...)` / 6つのバリアント + `ExecuteDefaultGuard` 再帰ガード
  + デフォルトチェーン300行以上。すべて削除。`run(NodeInput)` のみが唯一の純粋仮想関数。(コミット `19819d8`)
- **`add_cancel_hook` + `Hook` RAII + `hooks_*` メンバー + `cancel()` フック反復** — `cancel.h` は `fork()` + `cancel()` + `is_cancelled()` + `slot()` のみを保持。(コミット `1d786a5`)
- **`CurrentCancelTokenScope` + `current_cancel_token()` thread_local + `GraphState::run_cancel_token_` + 3つのアクセサ** — `RunContext::cancel_token` が唯一のキャンセルチャネル。`src/core/cancel.cpp` はスタブまで縮小（ファイル自体が将来の削除候補）。(コミット `9e8e956`)
- **6つの `PyGraphNodeOwner` レガシーオーバーライド** — pybind トランポリンは `run(self, input)` のみを呼び出す。Python ユーザーコードも v0.9.0 から単一メソッドを必要とする。(コミット `9e8e956`)
- **2つの廃止された pytest ファイル** — `test_execute_stream_dispatch.py` (v0.3.2 ストリーム専用フォールバックディスパッチ検証) + `test_streaming_only_error_
  hint.py` (execute_full_stream が優先される — v1.0 では無意味)。(コミット `4392fbb`)

### 修正済み

- **5つの fan-out サンプルに明示的な呼び出しを追加** — `e5ecb08` のデフォルトワーカー数戻しで埋もれていた真の並列意図を復元: `examples/10_send_command.cpp`、`examples/14_plan_executor.cpp`、`examples/21_mcp_fanout.cpp`、`examples/36_classifier_fanout.cpp`、`src/core/deep_research_graph.cpp` の `create_deep_research_graph()` ビルダーが `set_worker_count_auto()` を呼び出すようになった。検証: `classifier_fanout` 4.22倍の高速化 (25.2 ms 逐次 → 6.0 ms 並列)。(コミット `99c470b`)
- **`bench_async_http` httplib マクロガード** — `bench_async_http.cpp` は `<httplib.h>` を `<neograph/async/conn_pool.h>` 経由でインクルードするが、`CPPHTTPLIB_OPENSSL_SUPPORT` が未定義で、ODR ガードが拒否していた。CMake ターゲットに `target_compile_definitions(... PRIVATE ...)` を追加。(コミット `d4be42a`)
- **Windows MSVC `unistd.h` が欠落** — `test_schema_provider_extra_
  fields_temperature.cpp` は POSIX 専用の `mkstemps` + `close` を使用しており、Windows ビルド全体が失敗していた。ファイル全体を `#ifndef _WIN32` ガードでラップした (カバレッジは Linux/macOS で保証)。(コミット `3c49f12`)
- **Pythonテスト16件を移行** — wheel CIのpytestが `AttributeError` を28ノードクラスでレガシー `def execute(self, state)` パターンに遭遇しました。バッチ移行を `def run(self, input)`に実施。ストリーミングノードには `input.stream_cb` Noneガードを追加。(コミット `4392fbb`)

### 移行（ユーザーコード）

**プロバイダー呼び出し（候補6 — 非推奨フェーズ）**

新しいコード:
```cpp
// non-streaming
auto completion = co_await provider->invoke(params, nullptr);

// streaming
auto completion = co_await provider->invoke(params, on_chunk);

// sync site (replaces old complete())
auto completion = neograph::async::run_sync(provider->invoke(params, nullptr));
```

4つのレガシー仮想オーバーライドは非推奨期間を通じて引き続き動作しますが、ユーザーのオーバーライド箇所では`-Wdeprecated-declarations`警告が表示されます。削除はv1.0.0の直前に発生します。非推奨期間内での移行が推奨されます。

**`GraphNode`サブクラス（候補1フェーズB — 破壊的）**

C++コード：
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

Pythonコード：
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

**fan-out意図（ワーカー数のデフォルト変更）**

```cpp
// old (v0.x April+): default was hardware_concurrency but micro-bench burden
// v1.0: default=1. Add one line for intentional fan-out.
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // ← this line added (hardware_concurrency)
// or engine->set_worker_count(N);  // explicit N
```

`docs/migration-v0.4-to-v1.0.md`内の移行1/2/3セクション（run() / ctx.cancel_token / ワーカー数デフォルト）+ Providerセクション（次のドキュメント更新で追加予定）は、ケースバイケースの移行前/移行後ガイダンスを提供します。

## [0.8.0] — 2026-05-13 — DX ポリシー + ダウンストリーム主導のAPIギャップに対処しました。

実際のダウンストリーム（ProjectDatePop）からのフィードバックと内部カバレッジ差分によって明らかになった8件の問題（#22、#25、#26、#27、#28、#34、#35 + #16のフォローアップ）を、単一のマイナーバンプにまとめます。2つの新しい公開ヘルパー（`RunResult::channel<T>`、`RunContext::store`）、11の新しいオフライン例、`docs/migration-v0.4-to-v1.0.md`移行ガイド、および新規ユーザーが最初の30分で遭遇する摩擦を軽減する5項目のDXバンドルが含まれます。

### 追加

- **`RunResult::channel<T>(name)` / `channel_raw(name)` / `has_channel(name)`** — 結果からチャネル値を抽出するワンライナーヘルパー。両方の出力形状（ネストされた`output["channels"][name]["value"]`標準 + `react_graph`などのビルダーによって追加されるフラットキー）が自動的に処理されます。新しいctestが9件追加されました。（Issue #25）
- **`RunContext::store`** — ノード本体が1行の`in.ctx.store->get(ns, key)`でStoreに到達します。従来のパターン（`shared_ptr<Store>`を`NodeFactory`ラムダでキャプチャする）は引き続き動作します — 新しいコードは新しい形状のみを必要とします。新しいctestが3件追加されました。（Issue #27）
- **`Provider::complete_stream`非純粋デフォルト本体** — 最小限のモック/テストフィクスチャは`complete()`のみをオーバーライドする必要があります。既存のストリーミングネイティブオーバーライドは変更されません。新しいctestが2件追加されました。（Issue #22）
- **`neograph::json`配列 `.front()` / `.back()`** — nlohmannの筋記憶パターン（`msgs.back()["content"]`）がコンパイルされるようになりました。新しいctestが4件追加されました。（Issue #26）
- **11件の新しいオフライン例（41-51）** — `resume_if_exists_chat`、`custom_reducer_condition`、`store_personalization`、`request_queue_backpressure`、`cancel_token`、`node_cache`、`sqlite_checkpoint`、`openinference`、`async_tool`、`minimal`。すべてrc=0、APIキー/外部サービス依存なし。以前は参照がゼロだった27/53の`NEOGRAPH_API`クラス間のギャップを埋めます。
- **`examples/51_minimal.cpp`** — 1つのノード、LLMなし、ツールなし、モックプロバイダーなしの30行の入門例。NeoGraphの実行方法を5分以内で理解できます。
- **`docs/migration-v0.4-to-v1.0.md`** — `[[deprecated]]` の古い8仮想チェーン (`execute` / `execute_async` / など) から新しい `run(NodeInput) ->
  awaitable<NodeOutput>` への移行に関する、ケースバイケースの前後4例と一般的な間違い。`NEOGRAPH_DEPRECATED_VIRTUAL` マクロメッセージからもリンクされています。
- **README「Common pitfalls 5」セクション** — 新規ユーザーが最初の30分で遭遇する5つの事柄（`channel<T>`の使用法、`in.ctx.store`、`neograph::graph::`サブ名前空間、`<httplib.h>`マクロ、GCC 13コルーチンICE）を1か所にまとめています。各項目には修正方法と関連する例/問題へのリンクがあります。
- **コンパイル時 `#error` ガード (`include/neograph/api.h`)** — ユーザーのTUが `<httplib.h>` をNeoGraphヘッダーの前に `CPPHTTPLIB_OPENSSL_SUPPORT`なしでインクルードすると、コンパイルは明確なメッセージとオプトアウトマクロ (`NEOGRAPH_SKIP_HTTPLIB_MACRO_GUARD`) で失敗します。旧#16のランタイムSEGVをコンパイル時失敗に昇格させます。
- **`example_minimal` 5つの新しい親切なエラーメッセージctest** — `Unknown reducer` / `Unknown condition` / `Unknown node type` / `Write to
  unknown channel`メッセージに対する契約ロック。メッセージ本文に利用可能な名前、登録方法、トラブルシューティングリンクを埋め込みます。
- **`docs/troubleshooting.md` 4つの新しいエントリ** — Tracerアダプタ`close()`のハング/クラッシュ（#24）、GCC 13コルーチンICE（#23）、親切なエラーメッセージガイダンス（#22）、`RunResult::output`の形状（#25）。
- **`Tracer` + `OpenInferenceTracerSession::close()` `@warning`ブロック** — アダプタ作成者向けの生ポインタの落とし穴を明示的に文書化します。`RecordedSpan` + ラッパー分離パターンを正しいアプローチとして指摘します。既存の`tests/test_openinference_cpp.cpp::InMemoryTracer` + 新しい`examples/49_openinference.cpp::PrintTracer`を参照します。（Issue #24）

### 修正済み

- **`SchemaProvider::build_body` サイレントドロップ `extra_fields` が `params.tools` が空の場合。** 旧コードは `extra_fields` の適用を `if (!params.tools.empty())`内でゲートしており、 `reasoning` や `response_format` のようなコアスキーマフィールドが、ツールなしの呼び出しから完全に消えていました。修正: ツールブランチの外に移動し、常に適用されるようにしました。新しいctestを3件追加。(Issue #34)
- **`temperature_path`スキーマ側のオプトアウト。** 推論モデル（gpt-5.x、oシリーズ）は`temperature`と`reasoning.effort`が相互排他的ですが、スキーマには「このプロバイダーはtemperatureを受け付けない」と宣言する方法がなく、毎回の呼び出しで`params.temperature = -1.0f`センチネル回避策を強制していました。**修正：** スキーマで`"temperature_path": null`を指定すると、build_bodyがそれを完全にスキップします。新しいctestを4つ追加。（Issue #35）
- **親切なRuntimeErrorメッセージ** — `ReducerRegistry::get`/`ConditionRegistry::get`/`NodeFactory::create`「不明な<thing>: foo」および`GraphState::write`/`apply_writes``Write to unknown channel`が、メッセージ本文に利用可能な名前+登録方法+トラブルシューティングリンクを埋め込むようになりました。新規参入者はメッセージだけで次のステップを判断できます。
- **`SchemaProvider::complete_stream_async` HTTP/SSEブランチ** は、長寿命の専用`bridge_thread_`でディスパッチするようになりました（旧：`Provider`ベースのデフォルトが呼び出しごとに新しい`std::thread`を生成）。旧動作は、コールドのスレッドローカルリゾルバ/NSS状態でglibc`internal_strlen`のSEGVを引き起こしました。WSブランチはすでにネイティブのco_awaitなので影響を受けません。トークンのディスパッチはawaiterのexecutor上で維持されます（PR #10の不変条件）。（Issue #16）
- **`example/09_all_features.cpp`** ストアデモ — ノードボディ読み取りパターンについて`examples/43_store_personalization.cpp`を指すdocstringポインタを追加。オプション2 — オプション3（インラインライブノード）は、#27の`RunContext::store`が着地したら一緒にクリーンアップします。（Issue #28）

### ドキュメント

- `RunResult::output`（チャネルラップ）の正規の形状と、`react_graph`のようなビルダーが追加するフラットキープロジェクションとの関係をヘッダーのdocstringに文書化。新しいヘルパー（`channel<T>`/`channel_raw`/`has_channel`）の使用を推奨。（Issue #25）
- `RunContext::store`フィールド`@brief`ブロック — 2つの配管パターン（`in.ctx.store`推奨/旧ファクトリクロージャキャプチャ互換）をコード例を並べて提示。（Issue #27）
- 両方のパスを`examples/43_store_personalization.cpp`ファイルヘッダーコメントに文書化。

## [0.7.0] — 2026-05-11 — C++ openinference + 非同期ストリーミングブリッジ

v0.6.0に対して提出された4つの問題を1つのマイナーバンプでクローズします。見出し: `Provider::complete_stream_async`デフォルトは、外部エンジンコルーチン内から待機されたときにセグフォルトしなくなりました(issue #4) — NeoGraphの前にあるSSE / ストリーミングHTTPバックエンドの最も一般的な形状です。付随: v0.6.0 Python OpenInferenceレイヤーのC++版で、Phoenix / Arize / LangfuseがC++駆動のトレースをPython版と同じようにレンダリングします(issue #9)。さらに: 装飾的なPython OTelデタッチノイズを抑制(issue #2)、同じ`thread_id`の並行実行 + `schema_mutex_` × on_chunkロック不変条件がdocstringに固定されました(issue #6)。

### 追加

- C++ 版の`neograph_engine.openinference`（issue #9）。新しい`neograph::observability`モジュールは2つの部分をカバーする：
  - `Tracer` / `Span` — 依存関係のない小さな抽象インターフェースで、NeoGraph自体がopentelemetry-cppを引き込まないようにする。下流側は自身のバックエンド（OTel SDK、インメモリテストフェイク、ロギングレコーダーなど）をラップするアダプタを提供する。4つの属性セッター（string、int64、double、bool — boolは意図的に`set_attribute_bool`に改名され、`const char*`リテラルが誤って解決されないようにしている）、さらにストリームトークン診断用の`add_event`、ステータス、`end()`。
  - `openinference_tracer(tracer)` — CHAIN種別のルートスパンを開き、`OpenInferenceTracerSession`を返す。その`cb`フィールドは`engine.run_stream()`に接続され、ノードごとにCHAIN種別の子スパンを開き、`NODE_START`/`END`ペイロードを`input.value` / `output.value` JSONブロブに詰め込み、`LLM_TOKEN`イベントを個別のスパンイベントとして記録する。
  - `OpenInferenceProvider(inner, tracer)` — 任意の`Provider`をラップし、OpenInference LLM種別の属性セット（`llm.model_name`、`llm.invocation_parameters`、`llm.input_messages.{i}.message.{role,content}`、`llm.output_messages.0.message.{role,content}`、`llm.token_count.{prompt,completion,total}`）をすべての`complete*`呼び出しに付与する。ストリーミングオーバーロードはさらに`llm.token`イベントと最終的に組み立てられた`output.value`を追加する。
  - `tests/test_openinference_cpp.cpp`内の7つのパリティテストが`InMemoryTracer`参照アダプタを駆動する — ルート＋ノードごとのCHAINスパン階層、ERROR / INTERRUPTステータスの表示、LLM_TOKENスパンイベントの記録、セッションクローズ時の遅延スパンのクリーンアップ、LLMプロバイダ属性セット、ストリーミングトークンイベント、例外ステータスの伝播を検証する。

### 修正済み

- `Provider::complete_stream_async`のデフォルトブリッジは、ストリームの実行中に待機中コルーチンのエグゼキュータをブロックしなくなりました。修正前は`co_return complete_stream(...)`をインラインで実行していたため、(a) HTTP/SSE受信ループの間ずっとエンジンの`io_context`ワーカースレッドを占有し、同じエグゼキュータ上の他のノードコルーチンを停止させていました。また(b) `SchemaProvider`のWebSocket Responses分岐では、新しい`run_sync` io_contextを`run_sync(complete_stream_ws_responses(...))`を通じてエンジンワーカー上にさらにネストしていました。このため共有プロバイダー状態が競合し、外側の`GraphEngine::run_stream_async`内から呼び出すと断続的なセグメンテーションフォールトが発生していました。新しいデフォルトは同期`complete_stream`専用のワーカースレッドを生成し、各トークンを待機側のエグゼキュータへ戻します。これによりユーザーの`on_chunk`は待機中コルーチンと同じ単一スレッド上で実行され、再入も起きません。コルーチンは一度限りの`steady_timer.cancel()`で再開され、ワーカースレッドの例外は待機側で再送出されます。`SchemaProvider`にはネイティブの`complete_stream_async`オーバーライドが追加され、WebSocket経路ではワーカースレッドさえ使わず、`co_await`で`complete_stream_ws_responses`を直接待機します。`OpenAIProvider`はWebSocket経路や特別処理を持たないため、新しい基底クラスのデフォルトをそのまま利用します。`tests/test_provider_async_default.cpp`には、ストリーム中も並行ティッカーコルーチンが進み、チャンクがワーカーではなく待機側スレッドへ届くことを検証する`StreamAsyncBridgeDoesNotBlockExecutor`と、`StreamAsyncBridgeRethrowsWorkerException`の2テストを追加しました。（issue #4）

- `openinference_tracer`: OTelのSDKが、トレーサーを `Failed to detach context` と `engine.run_stream_async` + `StreamMode.ALL`と共に使用した際、毎回のシャットダウン時にstderrへ出力していたトレースバックを抑制する。NODE_STARTで作成されたOTelのcontextvarsトークンが、異なる `asyncio.Task` からデタッチされていた（NODE_ENDコールバックは呼び出し元のタスクではなくエンジンの継続から発火する）ため、 `Context.reset(token)` が `ValueError`を発生させた。SDKはこのraiseを握りつぶしたが、完全なトレースバックを `logger.exception`経由でルーティングし続けたため、意味論には影響を与えずに本番ログを汚染した。修正では、アタッチ時に（スレッド、タスク）を記録し、不一致の場合はデタッチをスキップする。さらに、 `logging.Filter` 上に、当該 `opentelemetry.context` をインストールする。同期呼び出し元と同一タスクの非同期呼び出し元は、ノードスパンの下でLLMスパンのネストを引き続き正しく取得する。（Issue #2） `_safe_detach` がスタック上にある間のみメッセージを破棄する狭い

---

## [0.6.0] — 2026-05-07 — OpenInferenceオブザーバビリティレイヤー

LangSmithのUXギャップを解消します。NeoGraphはすでにOTelシェイプのスパンを出力していました(したがってトレースは任意のOTelバックエンドに流れました)。今回のリリースでは、Phoenix / Arize / Langfuseがトレースをフラットな汎用アプリケーションスパンリストではなく、チャットバブル + トークンカウントUIとしてレンダリングするために使用するLLM固有の属性レイヤーを追加します。ローカルのPhoenixコンテナに対してエンドツーエンドで検証済み — ライター→クリティックのグラフは、6スパンの階層(CHAINルート → ノードスパン → LLMスパン)を生成し、モデル名、プロンプト/レスポンス、トークンカウントがPhoenix UIで表示されます。

### 追加

- `neograph_engine.openinference`モジュール:
  - `openinference_tracer(tracer)` — `otel_tracer`をミラーリングするが、ルートノードとノードスパンに`openinference.span.kind = "CHAIN"`をタグ付けし、ノードペイロードを`input.value` / `output.value` JSONブロブに詰め込むコンテキストマネージャ。
  - `OpenInferenceProvider(inner, tracer)` — 任意の`Provider`をラップする。各`complete()`で`llm.complete`子スパンを開き、`span.kind = "LLM"`をタグ付けし、`llm.model_name`、`llm.invocation_parameters`、`llm.input_messages.{i}.message.{role,content}`、`llm.output_messages.0.message.{role,content}`、`llm.token_count.{prompt,completion,total}`、およびLangfuse互換の`input.value` / `output.value`ブロブをキャプチャする。
- `bindings/python/tests/test_openinference.py`内の4つのテスト — 属性の存在、スパン階層、例外パス、ノード入力/出力JSONブロブに関するInMemorySpanExporterアサーション。

### 修正済み

- `openinference_tracer`は、各ノードスパンをOTelの*現在の*コンテキストとして(`otel_context.attach`経由で)アタッチするようになったため、ノード本体内部で開かれた子LLMスパンは、そのノードスパンの下にネストされる。これがないと、C++→Python pybindコールバック境界をまたぐcontextvar伝播により、期待される単一のトレイスツリーではなく、実行ごとに3つ以上の無関係なtrace_idが生成された。トークンはNODE_END / ERROR / INTERRUPTでデタッチされ、以前の現在のスパンを復元する。既存の`otel_tracer`が文書化しているのと同じパターンである。つまり、`trace.use_span(...).__enter__()`ではなく明示的なアタッチ/デタッチであり、`__exit__`が一致しないと安全に使用できない。

### 注記

- OpenTelemetryはオプション依存のままです。`neograph_engine.openinference`のインポートは、`opentelemetry-api`がインストールされていない場合にのみ、最初の使用時に明確なImportErrorを発生させます。インポート時ではありません。
- Phoenixのエンドツーエンド実行の場合::

      docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
      pip install opentelemetry-exporter-otlp

OTLP gRPCエクスポーターを`http://localhost:4317`に設定し、`http://localhost:6006`を開いてトレースを表示します。モジュールのdocstringに完全なスニペットがあります。

---

## [0.5.0] — 2026-05-07 — バインディングの操作性: ライブミューテーションリストプロパティ

バインディング経由で公開されるmessage / writes / sendsリストを変更する最も自然なPythonイディオムにおける、サイレントなno-opトラップを解消します。以前は`params.messages.append(msg)`がコピーを変更し、基盤となるC++ベクターは新しい項目を認識しませんでした——グレースフルな失敗（クラッシュも警告もなし）で、劣化したLLM応答を生み出していました。現在は`.append()`がライブのstd::vectorまで押し通します。

### 追加

- `bindings/python/src/opaque_types.h` — 5つのベクター型に対する`PYBIND11_MAKE_OPAQUE`: `std::vector<ChatMessage>`、`<ChatTool>`、`<ToolCall>`、`<graph::ChannelWrite>`、`<graph::Send>`。
- `module.cpp` `init_opaque_vectors` — `py::bind_vector`は各々をPythonクラス（`ChatMessageList`、`ChatToolList`、`ToolCallList`、`ChannelWriteList`、`SendList`）として登録し、ライブのC++ベクトルに対する完全な可変シーケンスプロトコルをサポートします。
- 各々に対する`py::implicitly_convertible<py::list, …>` — レガシーのビルドしてから代入するパターン（`params.messages = [m1, m2]`）は変更なしで動作し続けます。代入はPythonリストをバインドされたクラスに自動変換します。
- `bindings/python/examples/23_evolving_chat_agent.py` — スレッドごとに進化するチャットエージェント（ライブLLM）: エージェントのJSON定義は、蓄積された会話履歴に基づいてターン間で書き換えられます。進化をまたぐチェックポイント再開（以前のメッセージが存続）、`__graph_meta__`監査チャネルパターン、およびバリデータ境界（ノードタイプのホワイトリスト、必須チャネル）を示します。

### 変更

- `params.messages` / `.tools` / `chat_message.tool_calls` / `node_result.writes` / `.sends` は、プレーンな`list`ではなく、バインドされたクラスを返すようになりました。`len()`、反復、`__getitem__`、`__setitem__`、`.append()`、`.extend()`、スライシング——すべてPythonリストのように動作します。`isinstance(x, list)`のみがFalseを返します。リポジトリと下流のgrepで、そのようなisinstance呼び出しサイトがゼロであることを確認しています。
- `.github/workflows/nightly.yml` — `ops/s ≥ 600K`ゲートを削除します。`err=0`と`leak=false`で4回連続失敗した後、しきい値（ローカルハードウェアで969K ops/sに較正）は共有GitHubホストランナーでは到達不能でした（測定値233〜273K ops/s、ローカルの3〜4分の1）。スループット回帰検出はPR時の`bench-regression`ジョブ（安定したハードウェア、µs単位のシングルショットディスパッチ）にあります。夜間ソークの実際の値は、5分間の`err==0` + `leak_suspect==false`です——両方ともハードゲートとして維持されます。

### 注記

- `ChatMessage.image_urls`（`std::vector<std::string>`）は意図的に移行されていません——`vector<string>`はバインディング全体で広く使用されており、すべての呼び出しサイトを掃討せずにグローバルなOPAQUEを適用するのは安全ではありません。残存する制限として文書化されています。v0.6+の候補です。

---

## [0.4.0] — 2026-05-05 — v1.0準備: 統一された`run(NodeInput)`ディスパッチ

v1.0シャープニングトラック（ROADMAP_v1.md）の最初のリリース。8つの仮想`GraphNode`クロス積（`execute` / `execute_async` / `execute_full` / … / `execute_full_stream_async`）は、単一の正規メソッド`run(NodeInput) -> awaitable<NodeOutput>`に収束します。実行ごとのキャンセルメタデータは、非チャネルセットの`GraphState`メンバーとスレッドローカルの密輸チャネルから、明示的な`RunContext`引数に移動します。`deadline`と`trace_id`は、予約済みの拡張スロットとしてのみ追加され、`RunConfig`によっては設定されません。`CancelToken`は階層的な`fork()`を獲得し、マルチSend fan-outワーカーはそれぞれ、親の`cancel()`がカスケードするプライベートシグナルを所有します。

### 追加

- `RunContext`（`include/neograph/graph/engine.h`）— 実行ごとの明示的なメタデータ：使用可能な`cancel_token`、`thread_id`、`step`、`stream_mode`、および予約済みの`deadline`と`trace_id`スロット。エンジンはこれをすべての`NodeExecutor::run`呼び出しに通します。**PR 1、コミット`a473f0e`。**
- `GraphNode::run(NodeInput) -> awaitable<NodeOutput>` — 単一の正規ディスパッチエントリポイント。`NodeInput { state, ctx,
  stream_cb }`; `NodeOutput { writes, command, sends }`。デフォルトのボディはレガシーの8つの仮想関数に転送するため、既存のサブクラスはコンパイルを続けます。**PR 2、コミット `607ce66`。**
- `CancelToken::fork() -> shared_ptr<CancelToken>` — 独自の`cancellation_signal`を持つ子トークン。親の`cancel()`は、すべてのライブ子（および再帰的に孫）にカスケードします。`run_sync(aw, parent_token)`は`parent_token->fork()`に切り替わり、各ネストされたopが独自のスロットをバインドします — v0.3.xのemit対bindの競合とマルチSendの単一ハンドラー上書きを閉じます。v0.3.xの`add_cancel_hook`リストは、非推奨を通じて機能し続けます。**PR 3、コミット `897645c`。**
- `[[deprecated]]` 8つのレガシー`GraphNode`仮想関数 + `add_cancel_hook`上。内部呼び出しサイト（graph_node.cppのデフォルトチェーン、デフォルトの`run()`フォワーダー）は、新しい`NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED`マクロ（`api.h` — GCC / clang / MSVCポータブル）で囲まれています。非推奨の仮想関数をオーバーライドするユーザーコードは移行警告を確認します。エンジン内部はクリーンなままです。**PR 4、コミット`35a4517`。**
- `engine.get_state_view(thread_id) -> StateView`が正規の状態読み取りになりました。生の辞書`engine.get_state(...)`は、docstringでソフト非推奨です（警告は発行されません — 生の辞書は有効なエスケープハッチのままです）。**PR 5、コミット `f31aa53`。**
- 7つのC++ + 19のPython例が`run(NodeInput)`に移行されました。スモーク実行はv0.3.2の出力とビット単位で一致します。**PR 6a/6b、コミット `a2a24ef` / `0a76e3a`。**
- Pybindの`PyGraphNodeOwner`は`run(NodeInput)`をオーバーライドし、Pythonユーザーの`run`メソッド（定義されている場合）にディスパッチし、それ以外の場合はレガシーチェーンにフォールスルーします。`RunContext` / `NodeInput` / `CancelToken`はPythonに公開されます。`cancel_token`は、スレッドローカルのスマイグリングなしで`input.ctx.cancel_token`として到達可能です。**PR 7、コミット `4e186a5`。**
- `docs/reference-en.md` §6 GraphNodeは単一の`run()`に収縮しました。RunContext + `fork()`の例のサブセクションが§7の下に追加されました。READMEの「Differences from LangGraph」に「One node method」エントリが追加されました。**PR 8、コミット `519a00b`。**
- 組み込みのC++ノード（`LLMCallNode`、`ToolDispatchNode`、`RouteToNode`）は、`run(NodeInput)`オーバーライドに移行されました。**PR 9a、コミット `d1070dc`。**
- 新規参入者モードのトラップ修正：READMEのCMakeスニペットは、`graph::`サブ名前空間、cppdotenvパス、`OpenAIProvider::create()`と`create_shared()`の比較、nlohmannサブセットとしての`neograph::json`、3引数と2引数の`compile()`を文書化しています。Pythonの`compile(def, ctx, store=None)`キーワード引数が追加されました（追加的、非破壊的）。**コミット `ee11ed6`。**

### 変更

- README：「10Kワーカー測定ストレステスト」セクション — neoclaw上のRTX 4070 Ti + Gemma 4 E2B Q4、N=10000完了 @ 0エラー / 424秒 / 2572 MBピーク / ~1 KB追加ワーカーコスト / p99 648 ms（`7840b81`）。
- README: 「本番経済性」セクション — フリート安全性 + RAM デルタの枠組み (`b82b15a`)。
- README: LangGraph デルタリスト内の「Docker 不要」+「依存関係ドリフト耐性」の箇条書き (`333b482`、`a6061d7`)。

### 非推奨

- `GraphNode::execute / execute_async / execute_full /
  execute_full_async / execute_stream / execute_stream_async /
  execute_full_stream / execute_full_stream_async` — v0.5.x まで `[[deprecated]]` アノテーションと併用して動作、v1.0 で削除。
- `CancelToken::add_cancel_hook` — `fork()` に置き換え。同じ非推奨期間。

### 注記

- 検証: v0.4.0タグで442 → 452 ctest (NodeRunDispatch 3件 + CancelTokenFork 7件追加) + 96 pytest + 5件のライブLLM/WSがグリーン。
- サブPR (`run(const NodeInput&)` 参照パラメータ) が、pybind 非同期パスにおける v0.2.0 RunConfig コルーチン参照 UAF クラッシュ形状を引き起こした。修正はマージ前に適用: `NodeInput in` 値渡し。`node.h` に文書化。

---

## [0.3.2] — 2026-05-05 — キャンセル伝搬の強化 (5ラウンド)

v0.3.0 の単発キャンセルが明らかにしたギャップを埋める5 ラウンドのパッチシリーズ: Send fan-out 伝播、インプロセスポーリング、Python 用フック、C++ スコープ、例外型。また、FastAPI SSE チャットデモ評価からの TODO_v0.3.md フィードバックバッチも収録 — `resume_if_exists`、dict-or-list `update_state`、型付き状態読み取り用の StateView。

### 追加

- `RunConfig::resume_if_exists` — 明示的な `resume()` 呼び出しなしで、以前のスレッドのチェックポイントをオプトインで再開。標準的なマルチターンチャットのセマンティクス: `engine.run(cfg)` は `thread_id` が存在する場合に会話を継続する。
- `engine.update_state(thread_id, dict | list[ChannelWrite],
  as_node="")` — 両方の形状を受け入れる。修正前は `dict` のみが機能し、リストを渡すと黙って no-op となった。リスト形式は、すべてのノード本体の emit 形状と対称。
- `StateView` (`bindings/python/neograph_engine/state_view.py`) — Pydantic 型付き状態読み取り。`engine.get_state_view(thread_id) ->
  StateView` はフラットなドットアクセス (`view.messages` / `view.foo`) に加え、dict エスケープハッチ用の `view.raw` を返す。型付きチャンネル定義用のサブクラス: `class ChatState(ng.StateView): messages: list[dict] = []`。
- `bindings/python/tests/test_async_cancel_live_llm_fanout.py` — 実行中のキャンセルが、ソケット層で Send が生成したすべての兄弟を本当に中止することを検証 (v0.3.1 の根本原因パッチ)。
- `examples/22_self_evolving_graph.py` — TODO_v0.3.md #9 クックブックの折り込みとともに v0.3.2 に移動。
- ROADMAP_v1.md — キャンセルラウンドのポストモーテムから導出された設計を精密化する候補（単一ディスパッチ、RunContext、階層的CancelToken — すべてv0.4.0で提供）。
- Doxygen `/* */` ワイルドカード修正 — `acp/types.h` に `/**` ブロックが含まれており、パスワイルドカード（`fs/*`, `terminal/*`）がネストされたコメントを開き、後続のすべての診断を抑制していました。置き換え先： `&#42;` HTMLエンティティ。

### 修正済み

- キャンセル伝播、累計5ラウンド:
  1. v0.3.0 シングルノード — `cancel_token` が `Provider::complete` に到達。
  2. v0.3.1 マルチSendポインタのドロップ — fan-outワーカーが `run_cancel_token_shared()` を共有するようになった（`init_state +
     restore` がチャネルセット外のワーカーごとの状態を再構築した際に、このポインタは失われていた）。
  3. v0.3.1+ プロセス内ポーリング — エンジンのスーパーステップループがステップ間でポーリングするようになった（LLM入出力時のみではない）。
  4. v0.3.2 Python用フック — `add_cancel_hook` が実行ごとのトークンにコールバックを登録し、`cancel()` で発火する。同期Python `execute()` がスレッドローカルスコープなしでアドホックなキャンセルハンドラをインストールできるようにする。
  5. v0.3.2 C++スコープ + リトライ + 例外型付け — メインスレッドでの新規スロー `NodeInterrupt`（libstdc++ `__exception_ptr::_M_release` の競合を回避）、リトライ予算はキャンセルを尊重、ランタイム対ロジックの例外分割。
- `execute_stream` のみのPythonノードがデフォルトの `execute` パスに黙ってフォールスルーしていた（NotImplementedError）。現在は `run_stream` が、ユーザーがストリーミングバリアントのみをオーバーライドした場合に `execute_stream` を直接配線する。
- `update_state` が list[ChannelWrite] を受け入れる — 黙示的なno-opを解消（TODO_v0.3.md #5）。

### 注記

- 442 ctest + 96 pytest + 2 ライブLLM（シングル + fanoutキャンセル）が v0.3.2 タグでグリーン（`915e90e`）。
- 27/30 C++ 例 + 20/22 Python 例が `examples/run_all.py` で合格。スキップされたテストは外部サービス（Postgres / Crawl4AI / ライブOpenAI）を必要とする。
- Valgrind 6 例で 0 エラー、815 allocs / 815 frees クリーン。
- ベンチマーク中央値 5.185 µs/iter（seq パス、v0.3.0 ベースライン） — ラウンド全体でパフォーマンス回帰ゼロ。

---

## [0.3.0] — 2026-05-04 — 協調的キャンセル伝播

FastAPI SSEチャットデモ評価中に報告された本番コスト漏れのギャップを解消：フロントエンドの `AbortController` が asyncio タスクをキャンセルしても、上流の OpenAI リクエストが完了まで実行され続けることはなくなった。キャンセルは実行のすべてのレイヤーに伝播する。

### 追加

- `neograph::graph::CancelToken`（アトミックフラグ + asio `cancellation_signal`）と `CancelledException` — `include/neograph/graph/cancel.h`。協調的キャンセルプリミティブ。`RunConfig::cancel_token` 経由で渡す（オプションの `shared_ptr`）；エンジンのスーパーステップループはステップ間で `is_cancelled()` をポーリングし、`CancelledException` で脱出する。トークンの `cancellation_slot()` は実行の `co_spawn` にバインドされるため、実行中の LLM HTTP ソケット操作はワイヤ上で中止される（asio `operation_aborted`）。
- `CompletionParams::cancel_token` — 複数の `provider.complete()` 呼び出しにまたがってabortをスレッド化するユーザー向けの明示的なピン。 `Provider::complete` はそれを読み取り（または `current_cancel_token()` によって設定されたスレッドローカル `PyGraphNode::execute_full_async`にフォールバックし）、スロットをその内部の `run_sync` io_context にバインドするため、キャンセルがヒットした同期Pythonノードでも課金が停止する。
- `GraphState::run_cancel_token()` — 実行ごとの、非シリアライズ化されたハンドルであり、pybind `PyGraphNode` が `CurrentCancelTokenScope` を同期Python `execute()` 呼び出しの周囲にインストールするために使用します。これにより、同期Pythonユーザーはノードコードを変更することなく、透過的なキャンセル伝播を得られます。
- pybind `engine.run_async` / `run_stream_async`: asyncio `Future.cancel()` は現在 `add_done_callback` を経由して `CancelToken::cancel()` に配線され、`co_spawn` はトークンのキャンセルスロットをバインドします。
- pybind safe-resolve ヘルパー `_safe_set_future_result` / `_safe_set_future_exception` — ガード `future.set_result` / `set_exception` 経由でポストされた呼び出しを `call_soon_threadsafe` キャンセル済みフューチャーの `InvalidStateError` ストームから保護します。
- `bindings/python/tests/test_async_cancel_live_llm.py` — ライブ OpenAI E2E テスト。OpenAI HTTP が `Future.cancel()` から 3 秒未満で完了することを検証します（実際には即時。修正前は約 7〜8 秒の未キャンセルストリーミングでした）。`NEOGRAPH_LIVE_LLM=1` が設定されていない限りスキップされます。
- `examples/22_self_evolving_graph.py` — 自己進化グラフの PoC: `prompted_llm` ノードが自身のプロンプトを JSON 設定から読み取り、LLM リライターが実行間でグラフ定義を変更して再コンパイルできるようにします。`0.0 → 0.4` スコアの改善を示し、リライターにおけるチャネルフロー推論のギャップを文書化します。

### 変更

- `Provider::complete(params)` は、次の場合に内部キャンセルスロットをその `run_sync` にバインドするようになりました： `params.cancel_token` が設定されている場合、またはスレッドローカルの `current_cancel_token()` がアクティブな場合。オプトインしない呼び出し元に対しては、以前のデフォルト動作（キャンセルなし）が維持されます。
- `neograph::async::run_sync` はオプションの `graph::CancelToken*` パラメータを取得しました。非 null の場合、バインドされたスパウンはトークンのスロットをバインドします。
- pybind `resolve_future_async` は、`future.set_result` を `call_soon_threadsafe` 経由で直接呼び出す代わりに、safe-resolve ヘルパーを経由してルーティングされる。

### ロードマップ（v0.3.x に延期 — `TODO_v0.3.md` を参照）

- 同じ `thread_id` での LangGraph スタイルの自動チェックポイント再開。
- `run_async` エラーメッセージ内のストリーミング専用ノードのヒント。
- `cb.emit_token(node, data)` の使いやすいヘルパー。
- README の「LangGraph との違い」セクション。
- `update_state` のシグネチャをドキュメントに合わせて調整。
- `get_state` フラットヘルパー / Pydantic アクセサ。
- `run_parallel_async`および`run_sends_async`ブランチfan-outにおけるキャンセル伝播のライブ検証。
- pgvector RAG の例。

---

## [未リリース] — ステージ 4

ステージ4は、非同期パス上の最後の`run_sync`ホップを閉じる。`run_async`は、呼び出し元のexecutor上にエンドツーエンドで留まるようになった: 1つの`io_context`スレッド上の3つの50msエージェントが、`examples/27_async_concurrent_runs`において約150ms(直列)から約50ms(オーバーラップ)に低下する。

### 破壊的変更

- **`GraphNode::execute_full_async` デフォルトがasync-firstに変更されました。** 現在は `co_await execute_async(state)` を `NodeResult` にラップしており、同期の `execute_full(state)`を呼び出す代わりになっています。同期の `Command`/`Send` オーバーライドからのみ `execute_full` を発行するサブクラスは、1行の `execute_full_async` ブリッジを追加する必要があります:
  ```cpp
  asio::awaitable<NodeResult>
  execute_full_async(const GraphState& state) override {
      co_return execute_full(state);
  }
  ```
ブリッジがない場合、`Command`/`Send`は非同期パス上で静かに破棄される — 3.0が同期経由のルーティングによって修正した2.0の潜在的なディスパッチバグであり、スーパーステップごとに`io_context`スポーンのコストがかかる。ツリー内のすべてのサブクラス(`deep_research_graph`、例10/14/21、テスト5サイト)は現在、ブリッジを備えている。

### パフォーマンス

- 例27のウォールタイム: **152 ms → 53 ms**(1つの`io_context`スレッド上の3エージェント×50msタイマーステップ、完全オーバーラップ)。
- シングルラン・ベンチマークで測定可能な回帰はありません。 `run()` 依然として同じコルーチンを、新しいシングルスレッドの `io_context` 経由で駆動します。 `run_sync`.

### テスト

- 341/341 ctest グリーン
- 295/295 ASan+UBSan グリーン
- コルーチン多用サブセット(20 テスト、2.4 秒)で Valgrind クリーン

### リリース後検証（同日）

- **全30例を再実行:** 26/29 PASS、0 FAIL、3件は環境依存(clay_chatbot → raylib、postgres_react_hitl → docker compose、deep_researchフルループ → crawl4aiサービス)。`21_mcp_fanout`は3 MCPコール/8msウォルで測定 — ステージ4のオーバーラップは、実際のネットワークI/O下でも維持される。

- **ARM64互換性(docker buildx --platform linux/arm64):** リポジトリルートの`Dockerfile.arm64-smoke`。ubuntu:24.04-arm64 + core+llm+async+sqlite+testsビルドはQEMUエミュレーション下で約15分で完了; **ARM64上で306/306 ctest green**。ストリップされたバイナリサイズは0.81〜0.88 MB(x86_64とほぼ同一)。例27はエミュレーション下で65 msで実行(ネイティブx86_64: 53 ms)。macOSベータ(Apple Silicon)に加えて、Linux/ARM64がサポート対象として確認された。

- **キャッシュ局所性(Ryzen 5800X / Zen 3、Valgrind cachegrind、32 KB L1i/d 8-way、32 MB L3 16-way):** `bench_concurrent_neograph`スイープN=1 → 10,000。

    | N | I refs | LLiミス | LLiミス率 | ネイティブp50 |
  |---:|---:|---:|---:|---:|
    | 1 | 5.3 M | 4,313 | 0.08% | 17 µs |
    | 100 | 11.8 M | 4,320 | 0.04% | 6 µs |
    | 10,000 | 648 M | 4,329 | 0.00% | 5 µs |

最終レベル命令ミスは、N の 4 桁の範囲にわたって約 4,320 で横ばいのまま。ユニークなホットコードのワーキングセットは約 277 KB(L3 の 0.85%)。N=10,000 での 648 M 命令は、わずか 4,329 回の LL ミスを発生させる — 命令 150,000 件あたり約 1 回のミス。ネイティブ p50 は、純粋に I-cache のウォーミングにより 17 µs から 5 µs に低下する。「バースト並行性の堅牢性」というポジショニングに対する最初の測定された証拠。

---

## [3.0.0] — 2026-04-22

3.0はTaskflow依存を削除し、同期および非同期のスーパーステップ実行を単一のasioコルーチンパスに統合します。グラフ定義JSON、ノードABI、チェックポイントスキーマ、および公開エントリポイント（`run`, `run_async`, `run_stream`, `resume`）は2.0とソース互換です。破壊的変更は、 `GraphNode` サブクラスに限定され、これらは**同期** `Command`/`Send` を出力します。 `execute_full` オーバーライドからのみ

### 破壊的変更

- **`deps/taskflow/`とTaskflow INTERFACEターゲットは削除された。** 同期スーパーステップループ、`run_one`、`run_parallel`、`run_sends`、およびプロセス全体の`tf::Executor`静的変数は削除された。NeoGraphのインクルードパスを介して`#include <taskflow/...>`する下流コンシューマは、Taskflowを別途ベンダー化する必要がある。
- **`GraphNode::execute_full_async` default は現在、同期 `execute_full` への直接呼び出しを介してブリッジします（ `co_await execute_async`なし）。**これにより、同期専用オーバーライドから発行される `Command`/`Send` が保持されます — これは一般的な2.0パターンです — すべてのエントリポイントが現在共有する非同期パスを通じて。非ブロッキングI/Oと `Command`/`Send` の両方を必要とするAsyncネイティブノードは、 `execute_full_async` を直接オーバーライドする必要があります。docstringは2.0以降これを述べてきましたが、2.0は同期 `run()` がコルーチンパスを完全にバイパスしたため、これを実行したことはありませんでした。
- **`NodeExecutor::run_one` / `run_parallel` / `run_sends` の同期メソッドは削除されました。** `_async` の同等メソッドを使用してください。
- **CPU並列fan-outはオプトインです。** 以前はTaskflowがデフォルトでプロセス全体のスレッドプールを提供していました。3.0では、`run_parallel_async` と `run_sends_async` のマルチSendブランチは、コルーチンを駆動するエグゼキュータ（同期 `run()` によって起動されるシングルスレッドのio_context、または `run_async()` の呼び出し元自身のエグゼキュータ）に応じてブランチをディスパッチします。I/Oバウンドのfan-outは依然としてオーバーラップします（シングルスレッドでのco_awaitサスペンション）。CPUバウンドのfan-outは、呼び出し元が `run_async()` にマルチスレッドエグゼキュータを使用するか、`engine->set_worker_count(N)` を介してエンジン所有のプールをオプトインしない限り、シリアル化されます。

### 追加

- `neograph::async::run_sync_pool(awaitable, n_threads)` — 既存のシングルスレッド `run_sync` に加えて、Nワーカー同期↔非同期ブリッジ。呼び出しのために新しい `asio::thread_pool` を起動し、内部の `make_parallel_group` ブランチが別々のワーカーで実行されるようにします。
- `GraphEngine::set_worker_count(n)` — 並列fan-outディスパッチのために `NodeExecutor` が使用する、オプトインのエンジン所有スレッドプール。エグゼキュータを再構築します。並行実行の前に呼び出す必要があります。

### 変更

- `GraphEngine::execute_graph` (sync) は廃止されました。すべてのエントリポイント（`run`、`run_stream`、`resume`）は`execute_graph_async`を経由して`neograph::async::run_sync`にルーティングされるため、スーパーステップループ、リトライバックオフ、チェックポイントI/O、および並列fan-outは、エンドツーエンドで単一のコルーチンパス上に存在します。
- `benchmarks/concurrent/bench_concurrent_neograph.cpp` は、呼び出し側ドライバーを `tf::Executor` / `tf::Taskflow` から `asio::thread_pool` + `asio::post` に切り替えました。

### パフォーマンス（リファレンスLinux上のbench_neograph Release -O3 -DNDEBUG、10回実行の中央値）

- `seq` エンジンオーバーヘッド（3ノードチェーン、カウンター）：呼び出しあたり **約5.0 µs**。
- `par` エンジンオーバーヘッド（5ワーカーfan-out + サマライザー）：呼び出しあたり **約11.8 µs**。
- ベンチプロセス全体のピークRSS（ウォームアップ＋seq＋parイテレーション）：**4.8 MB**。
- 同じワークロードでのLangGraph 1.1.9との比較: イテレーションあたり**seqで131倍、parで199倍高速**、RSS約12倍軽量。

このCHANGELOGの以前のドラフトでは、「約46 µs シーケンシャル / 約114 µs パラレル」を3.0のリグレッションとして挙げていました。これらの数値は、`CMAKE_BUILD_TYPE` が未設定のビルドツリーからのものであり、ベンチバイナリが `-O3 -DNDEBUG` なしでコンパイルされていました。適切なReleaseビルドでは、非同期ピアの統合は2.0のTaskflow同期パス（2.0 READMEが同じホスト上で20.65 µs シーケンシャル / 150.7 µs パラレルと宣伝していた）に対して**有利**です。修正されたチャートは [`docs/images/bench-engine-overhead.png`](docs/images/bench-engine-overhead.png) にあります。

### 移行

- ノードが `execute()` / `execute_async()` をオーバーライドし、`Command` / `Send` を発行しない場合、アクションは不要です。
- 同期 `execute_full` をオーバーライドして `Command` / `Send` を発行する場合：変更は不要です — 3.0の非同期パスのデフォルトは、現在、同期オーバーライドを直接呼び出します。`Command.goto_node` ルーティングは、同期エントリポイントと非同期エントリポイントの両方で機能します。
- オーバーライドする場合 `execute_async` (async-native I/O) かつ `Command` / `Send`を希望する場合: `execute_full_async` を直接オーバーライドし、 `NodeResult` をそこで組み立ててください。`execute_async` のみをオーバーライドすると `Command` / `Send` を黙って失います。なぜなら、デフォルトの `execute_full_async` は現在、同期 `execute_full`経由でルーティングされ、非同期 `execute_async` ではないからです。
- Taskflowのプロセス全体のプールをCPU並列fan-outに依存していた場合、 `engine->run()`: compile()の後に `engine->set_worker_count(N)` を一度呼び出すか、または `run_async()` を介してエンジンを自身のマルチスレッド `asio::thread_pool` / io_context上で駆動してください。

---

## [2.0.0] — 2026-04-22

Stage 3 async APIを備えた最初の公開リリース。これは破壊的リリースです。以下の変更はコンパイル（C++標準）とABI（抽象基底クラスにasyncピアが追加）に影響します。Sync呼び出しサイトはビット単位で保持されるため、**`Provider` / `CheckpointStore` / `GraphNode` / `Tool`をオーバーライドしないアプリケーションコードは変更なしで動作し続けます**。

### 破壊的変更

- **C++20 必須。** 公開APIは `asio::awaitable<T>` を公開しており、これには `std::coroutine` のサポートが必要です。利用者は `-std=c++20` （またはそれ以上）でコンパイルする必要があります。GCC 13+、Clang 15+ でテスト済み。GCC 13のコルーチン回避策については `docs/ASYNC_GUIDE.md` §4.1 を参照してください。
- **libpqxx依存関係を削除。** `neograph::postgres`は現在libpqを直接リンクします。Ubuntu 24.04ユーザーは、libpqxx-7.8t64のC++17/C++20 ABI分割によって導入された`pqxx::argument_error::argument_error(..., std::source_location)`リンクエラーに遭遇しなくなりました。CMake findは現在`PostgreSQL::PostgreSQL`（CMakeバンドルFindPostgreSQL）を対象としています。`libpqxx-dev`のみをインストールしたコンシューマは、`libpq-dev`もインストール/保持する必要があります。
- **`Provider`、`CheckpointStore`、`GraphNode`、`MCPClient` ABIを拡張。** それぞれにasyncピア仮想関数（`complete_async`、`save_async`、`execute_async`、`rpc_call_async`およびその変種）が追加されました。下流のサブクラスは2.0ヘッダーに対して再コンパイルします。ソースは、サブクラスがネイティブasyncオーバーライドを提供したい場合を除き変更されません（実際のI/Oを行う実装者には推奨）。
- **`CheckpointStore::save` / `load_latest` / `load_by_id` / `list` / `delete_thread` はもはや純粋仮想ではありません。** これらには、対応する `_async` ピアに `neograph::async::run_sync`を介してブリッジするデフォルト実装が含まれるようになりました。同期側をオーバーライドするサブクラスは引き続き動作します。オーバーライドを一切提供しなかったサブクラス（以前はコンパイルエラーになっていた）は、今では無限再帰します。契約: 各同期/非同期ペアの少なくとも一方をオーバーライドすること。

### 追加

- **Async API** すべてのI/Oレイヤーにわたって（完全なリファレンスは`docs/ASYNC_GUIDE.md`）：
  - ベースクラスとすべての組み込みプロバイダー（OpenAI、Schema、RateLimited）上の`Provider::complete_async`。
  - HTTPとstdioトランスポートの両方の`MCPClient::rpc_call_async`。stdioは`asio::posix::stream_descriptor`を使用します。
  - 8つのsyncメソッドすべての`CheckpointStore::*_async`。
  - `GraphNode::execute_async` + stream / full / full_stream変種。asyncネイティブクロスオーバーデフォルト付き。
  - `GraphEngine::run_async` / `run_stream_async` / `resume_async` を駆動する `execute_graph_async` — 並列fan-outを`asio::experimental::make_parallel_group`経由で含む、エンドツーエンドのコルーチン・スーパーステップループ。
  - `neograph::AsyncTool` ユーザーツール向けのアダプターで、同期`Tool`インターフェースを維持しながらコルーチンボディを必要とするものです。
- **`neograph::async`名前空間** — HTTPクライアント、コネクションプール、SSEパーサー、run_syncブリッジ、URLエンドポイント分割。`include/neograph/async/*.h`を参照。
- **新しい例**:
  - `examples/27_async_concurrent_runs.cpp` — 1つの`io_context`上で複数のエージェント。
  - `examples/05_parallel_fanout.cpp`（書き直し） — `run_parallel_async`を使用した単一グラフ実行内の非同期fan-out。
- **CIベンチマーク回帰ゲート** (`.github/workflows/ci.yml`) — PRチェックが`bench_async_http` / `bench_async_fanout` / `bench_neograph`の下限を強制します。

### パフォーマンス

Stage 2 同期ベースラインに対する feat/async-api ブランチで測定:

- `bench_async_http --mode async_pool --concur 1000`: 6064 ops/s → **17834 ops/s** (2.9×)。
- `bench_async_fanout --concur 50000`: スレッド・パー・エージェントは実現不可能 → **541K ops/s / 67 MB RSS**。
- `examples/27_async_concurrent_runs` (3 × 50msの非同期処理): 150ms (同期) → **50ms** (1 io_contextスレッド)。
- `examples/05_parallel_fanout` (3 × 100-150msの非同期処理): 370ms (逐次) → **150ms** (1 io_contextスレッド)。
- `bench_neograph` エンジンオーバーヘッド: 変化なし (~30 µs 逐次 / ~205 µs 並列)。コルーチン機構はホットパスを劣化させない。

### 2.0.0には未搭載

- **Taskflow依存** は残る。同期の`engine.run()`パスはfan-outに依然としてそれを使用する。Sem 4.5では、同期パスを`run_sync(*_async)`に置き換えられるか再検討し、依存関係を完全に削除できるようにする。

### クロスプラットフォーム

2.0.0では、3つのプラットフォームが異なる安定度レベルでサポートされています。このレベルは、リリース前にプラットフォームが受けた実世界での検証の度合いを反映しています（機能カバレッジではありません。コードベースは`#ifdef _WIN32`分割で単一ソースであり、テストが通れば機能はプラットフォーム間で同等です）。

#### Linux — **GA**（本番対応）

* Ubuntu 24.04、GCC 13。
* ローカルで完全な332/332 ctestグリーン（docker `postgres:16-alpine`経由のPostgres）に加え、コミット済みCI下限内のすべてのベンチマーク。
* fork/pipe/execvp上のMCP stdio + `asio::posix::stream_descriptor`。
* Postgres 非同期ピア（libpq ノンブロッキング上 + `asio::posix::stream_
  descriptor` ラッピング `PQsocket`）
* 上記のすべての性能数値の参照プラットフォーム。

#### macOS — **ベータ**

* macos-latest（Apple Silicon）、Clang via Xcode。
* CIがビルドおよび非Postgresテストを実行する。Postgres統合ケースはサービスコンテナなしで自己スキップされる。POSIXパス（同じfork/pipe + asio::posixコード）は実行される。
* `CoreFoundation` + `Security`フレームワークがhttplib経由でリンクされ、TLSのシステム証明書読み込み用。
* 2〜4週間のCI実行とユーザーレポートでランタイム動作の相違（コルーチンスケジューリング、SIGPIPE/EPIPE形状、パイプバッファサイズ）を確認するまでベータとして扱う。インシデントなしにそれらが実現すればGAへの昇格をターゲットとする。

#### Windows — **アルファ**

* windows-latest、MSVC 19.44 (VS 2022)、x64。
* CIスコープ: **core + async + MCP + LLMのみ**。PostgreSQLとSQLiteバックエンドはWindows CIジョブで無効化されています。vcpkgが毎回の実行でOpenSSL / libpq / zlib / lz4をソースからコンパイルするためです（約20分、`x-gha`が削除されて以来、上流に動作するバイナリキャッシュバックエンドがありません）。Windowsユーザーは、自身のvcpkg / chocoセットアップを介してローカルでこれらをコンパイルします。
* ランナーのプリインストール済みchocoパッケージ（`C:/Program Files/OpenSSL-Win64/`）経由のOpenSSL。httplib + asio::sslのTLSパスがコンパイルおよびリンクされます。
* MCP stdio: `CreateProcess` + 名前付きパイプ（FILE_FLAG_OVERLAPPED）+ `asio::windows::stream_handle`。オーバーラップパイプパスは、ローカルのWindows検証なしでMSDN仕様に基づいて記述されました。最初のユーザーがエッジケース（ERROR_IO_PENDING処理、大きなJSONレスポンスでのパイプバッファ境界）を表面化することが予想されます。
* Postgres 非同期ピア（ローカルで有効な場合）：`asio::ip::tcp::
  socket::assign` が `PQsocket` が返す SOCKET をラップ（64ビット SOCKET 値を保持するため `native_handle_type` を介してキャスト）。Windows CI では実行されない — ローカルのみ。
* コルーチンメカニズムはMSVCの`<coroutine>`に存在します。仕様上GCC/Clangと一致する動作が期待されますが、`examples/27`のクロスランオーバーラップ測定はWindowsではまだ確認されていません。
* 2.0.0 までは **alpha** として扱う。本番ユーザー1名が stdio/パイプまたはコルーチンスケジューラの問題に遭遇せずにマルチエージェントワークロードを1週間実行し、かつ Postgres 非同期ピアが vcpkg の完全な libpq ビルドを実行する意思のあるユーザーによってローカル検証されたら、beta に昇格する。

> **パターン**: CI グリーンは下限であり、上限ではない。レイヤー3 ランタイム
> の動作の違い（コルーチンスケジューリングのタイミング、パイプバッファ
> 境界、ソケットテイクオーバーセマンティクス）は、実際の
> ワークロードでのみ表面化する。上記のティア言語は、3つすべてが初日から
> 交換可能であると装うのではなく、各プラットフォームに対してユーザーに正しい
> 期待値を与える。

### バンプ後の修正

- **`async::HttpResponse` headers map** — レスポンスサーフェスは現在、 `headers` ベクターを公開します。 `(name, value)` ペアはワイヤー順序と元のケーシングを保持し、さらに `get_header(name)` を大文字小文字を区別しないアクセサーとして提供します。Retry-After と Location は後方互換性のための専用フィールドとして残ります。これにより、以下の MCP セッショントラッキング修正が可能になります。
- **MCP `Mcp-Session-Id`ヘッダートラッキング** — Sem 2.6のhttplib→async_post移行でこれが静かに失われました。初期化後のすべてのRPCは、新しいヘッダーアクセサーを介してサーバー割り当てのセッションIDをエコーバックするため、サーバーのセッション状態はルーティング可能なままです。
- **MCP stdio 待機可能ミューテックス** — `StdioSession::rpc_call_async` は `std::mutex` を使用していたが、同じシングルスレッド io_context 上の2つのコルーチンが同じセッションを呼び出したときにデッドロックした（2番目の `lock_guard` が、1番目が必要とするワーカーをブロックした）。容量1のセマフォである `asio::experimental::channel<void(error_code)>` に置き換え、2番目の取得者が協調的にサスペンドするようにした。
- **`PostgresCheckpointStore` async peers** — 8つの CheckpointStore 非同期メソッド（`save_async`、`load_latest_async`、`load_by_id_async`、`list_async`、`delete_thread_async`、`put_writes_async`、`get_writes_async`、`clear_writes_async`）はすべて真の非同期になった。内部構造: `PQsetnonblocking(1)` + `PQsendQueryParams` + `asio::posix::stream_descriptor` を `PQsocket()` + `co_await sock.async_wait(wait_read/wait_write)` 上で使用。4スロットのプール上での4つの並行 `save_async` 呼び出しは、`run_sync` を介して直列化されるのではなく、ワイヤーレベルで並列に commit-fsync されるようになった。

---

## [0.1.0] — 2026-04 より前

プレリリース開発。公開 API の安定性は保証されません。
