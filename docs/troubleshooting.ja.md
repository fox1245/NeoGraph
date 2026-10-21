<!-- neograph-i18n: source=docs/troubleshooting.md locale=ja source_sha256=059f7da5000cc79ba45e2bfb3ef2931da6781a19f6caa69b0f778ef2e27e19ff -->
# トラブルシューティング

**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)

## インストールしたパッケージの確認

`neograph_engine.__version__`、Pythonのバージョン、OSとアーキテクチャ、インストール方法、最初に出たエラーを控えておきます。ソースのチェックアウトと古いwheelが同じAPIを公開しているとは思い込まず、そのリリースで入手できるwheelのファイルを確認してください。型付きインターフェースへの切り替え（typed cutover）で、旧来のプロバイダー名は削除されました。古いwheelでimportできても、新しいバインディングを試したことにはなりません。

NeoGraphが公開しているプラットフォームのメタデータには、Linux、macOS、Windowsが含まれます。互換性のあるwheelタグやコンパイラーがあるだけでは、ランタイムの統合が動くことの証明にはなりません。WASMでのプロバイダーランタイムのサポートは、確立されていません。

GLIBCのシンボルが見つからないというエラーが出た場合は、wheelのmanylinuxタグとホストのglibcを比べます。`manylinux_2_34` のwheelには、glibc 2.34以降が必要です。WindowsでDLLのエラーが出た場合は、Pythonがx64であることを確認し、見つからない依存DLLを調べます。原因がアーキテクチャだけとは限りません。同梱されているライブラリを、1つだけ差し替えてはいけません。

## ソースビルドの設定と、見つからない依存ライブラリ

Coreは、`NEOGRAPH_BUILD_LLM=OFF` にしても、`SchemaProvider::runtime` を必要とします。NeoGraphの構成にはCMake 3.20以降が必要です。SchemaProviderの解決順は、明示した `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、インストール済みのパッケージ、リビジョンを固定した公開archiveの既定のフォールバック、の順です。オフラインで構成するには、対応するSDKをインストールし、`CMAKE_PREFIX_PATH` でそのプレフィックスを指定して、`NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` を設定します。SDKをソースからビルドするには、C++20、Python、スタンドアロンのAsio、yyjson、libcurl 7.88以降、OpenSSL Cryptoが必要です。NeoGraphは、リポジトリに含めているAsioとyyjsonを、取得したSDKやソースのSDKとの統合に提供します。

SDK単体は、OpenSSL Cryptoの最低バージョンを宣言していません。NeoGraphのHTTPSをフルに有効にした構成と、wheelやsdistのビルドでは、非同期／MCPのHTTP依存を通じて、OpenSSL 3が必要です。Cryptoのヘッダーだけをインストールしても、これらのビルドには足りません。

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
  -DNEOGRAPH_BUILD_PYBIND=ON
cmake --build build -j
```

ビルドツリーは、インストール済みのパッケージではありません。`PYTHONPATH=build python -c "import neograph_engine"` を実行する前に、`pip install 'pydantic>=2' certifi` でwheelのPython依存をインストールしてください。pydanticがないとimportに失敗し、certifiがない場合のTLSは、ホストの信頼ストアを使います。

`Could NOT find CURL` は、`NEOGRAPH_USE_LIBCURL` を無効にしても解消しません。このフラグが制御するのは、NeoGraphのオプションのCurlH2Poolで、SDKに必須のトランスポートではないためです。libcurlの開発用ファイルをインストールし（Debian/Ubuntuでは `libcurl4-openssl-dev`、Fedoraでは `libcurl-devel`）、ツールチェーンとプレフィックスは一貫したものを使ってください。

SQLiteとPostgreSQLは、NeoGraphのオプションのコンポーネントです。それぞれの開発用パッケージを用意するか、`NEOGRAPH_BUILD_SQLITE` / `NEOGRAPH_BUILD_POSTGRES` を明示的に無効にしてください。無効にしても、SDKのランタイムはなくなりません。ヘッダーやAPIを変更した後にバインディングのシンボルが解決できない場合は、新しいビルドディレクトリで、対応するヘッダーとライブラリを使って、すべてのコンシューマーを再構成して再ビルドします。

GCC 13では、コルーチンに関するコンパイラー内部エラーが報告されたことがあります。適切なコンパイラーにアップグレードするか、報告された式の書き方を変えてください。catchハンドラーの中で `co_await` してはいけません。C++自体が、そこでのawait式を禁止しています。エラーを捕捉しておき、ハンドラーの外で復旧処理をawaitしてください。

## 削除されたプロバイダーAPIと、型付きの失敗

`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、ディスクリプターのインタープリターの設定、`prefer_libcurl`、ResponsesのWebSocket選択は、削除されました。代わりに `SchemaProvider(load_provider_descriptor(descriptor_json), ProviderRuntimeOptions(...), SchemaProviderDefaults(...))` と `make_provider_request` を使ってください。コンストラクターの正確な仕様は[Pythonバインディング](python-binding.md)を、C++については[マイグレーション](migration-v0.4-to-v1.0.md)を参照してください。

`prepare` は、1回しか使えない `PreparedProviderRequest` を返し、`dispatch` がそれを消費します。`invoke` は、その両方をまとめて行います。ディスパッチのレシートを永続化した後で、admission済みのリクエストを組み立て直したり、消費済みのハンドルを再試行したりしてはいけません。失敗した結果はデータであり、本物のエラーや部分的なエビデンスを含みます。成功に見える形のcompletionとして読まずに、`outcome.failure` を確認してください。

オブザーバーの例外では、型付きの結果と原因が `ProviderObserverError` に保持されます。予算の精算と、最終レシートの永続化で起きたエラーも、本物の結果を保持します。そのエビデンスがあっても、もう一度送信してよいことにはなりません。配送が不明な場合に必要なのは照合であり、闇雲な再試行ではありません。

## ストリーミングとPythonの非同期の境界

`ProviderMode.Stream` は、明示的に設定します。オブザーバーを設定しただけでは、ストリーミングは選択されません。トークンを表示する用途では、空でないコンテンツテキストのデルタだけを選び出してください。使用量、reasoning、ツール、rawワイヤー、エンベロープの各イベントは、トークンではありません。C++のイベントビューはコールバックの間だけ借用されるもので、PythonのProviderEventは、借用したデータのコピーを所有します。遅いオブザーバーは、ホストの配信容量を消費するので、避けてください。

```python
import asyncio
import neograph_engine as ng

text = ng.Text()
text.value = "Hello"
request = ng.make_provider_request(
    provider, model, [ng.ProviderMessage(ng.ProviderRole.User, [text])],
    mode=ng.ProviderMode.Stream,
)
request.on_event = observe
outcome = await asyncio.to_thread(provider.invoke, request)
if outcome.failure is not None:
    handle_failure(outcome.failure)
else:
    handle_completion(outcome.completion)
```

プロバイダーの invoke / dispatch は、GILを解放する同期のPythonメソッドです。asyncioのネイティブなawaitableではないので、`asyncio.to_thread` を使ってください。コールバックの呼び出し、コピー、破棄では、GILを取得します。キャンセルにはグラフのCancelTokenを使い、停止の要求はSDKに転送されます。タイムアウトしたことや、待機側がキャンセルされたことは、リモートでの効果が起きなかったことの証明にはなりません。

継続には、ツール、refusal、reasoningのパートを含む、完全な `ProviderMessage` を保持してください。`ChatMessage` と `ToolCall` は、グラフ向けの便宜的な値で、完全なプロバイダー履歴の別名ではありません。使用量が欠けている場合は、ゼロではなく `None` です。ゼロと判明しているカウンターは、エビデンス付きのUsageCountの値のままです。ポータブルな結果の射影から、ネイティブリプレイの権限や金銭に関する権限を取り込むことはできません。ネイティブな継続を永続化するのは、admission済みのNativeArchiveの保管下に置く場合だけにしてください。

## TLSとローカルエンドポイント

信頼するバンドルを明示するには、`ProviderRuntimeOptions.ca_file` を設定します。Pythonのimport時には、既存の `SSL_CERT_FILE` があればそれを保持し、なければ、使える場合にcertifiを選びます。選ばれたCAファイルは、ランタイムオプションを通じてSDKのlibcurlに渡されます。`NEOGRAPH_SKIP_CERT_AUTOFIX=1` を設定すると、ホスト側の選択には手を加えません。エンドポイントや信頼ストアのエラーを隠すために、証明書の検証を無効にしてはいけません。

ローカルのHTTPエンドポイントのadmissionは、生のURLの上書きではなく、検証済みのディスクリプターで行います。[サンプル31](../examples/31_local_transformer.cpp)に従ってください。

## グラフ定義と登録

未知のリデューサー、条件、ノードタイプという診断は、コンパイルに使ったレジストリにその名前がないことを意味します。組み込みのリデューサーは `overwrite` と `append` です。カスタムのリデューサー、条件、ノードファクトリーは、コンパイルの前に登録してください。`has_tool_calls` と `route_channel` は、組み込みの条件です。登録したPythonのコールバックは、GILの下で実行されます。

```python
import neograph_engine as ng

ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)
ng.ConditionRegistry.register_condition("is_long",
    lambda state: "long" if len(state.get("messages") or []) > 10 else "short")
```

書き込みでは、既存のチャネルの名前を正確に指定する必要があります。条件が返すのはルートのラベルです。openな条件では、`default` を明示できます。それがなければ、一致しないラベルで例外が発生します。closedな条件では、宣言した集合にないラベルは拒否されます。`__start__` から出るエッジと、ループの抜け道となるルートを用意してください。`RunConfig.max_steps` のデフォルトは `50` です。打ち切られた場合は、通常の完了として扱わず、ステップ上限のステータスを確認してください。

`schema_version: 1` では、strictなトポロジーのパースが、未知のキー、使われないキー、ラウンドトリップで失われる内容を拒否します。メタデータには `_` や `x-` で始まるアノテーションを使い、バリアには空でない `wait_for` を、条件付きエッジには `routes` を指定してください。カスタムの登録を済ませた後で、`ng.export_schema()` を使って現在のスキーマをエクスポートしてください。エディター用のパレットを別に管理してはいけません。バージョンがない、または0の旧来の定義は、`0.x` の経路では緩やかに扱われます。`ng.upgrade_topology()` は、無視されたデータを、衝突しないアノテーションとして保持します。

## fan-outと管理操作

worker_countのデフォルトは `1` で、エンジンが所有するプールは作られません。I/Oを行う分岐は、中断している間は重なって進みます。CPUを使う処理は、呼び出し側のスレッドが1つだけだと、直列に実行されます。CPUを使う処理がプールで速くなる場合は、並行実行の前に、`set_worker_count(N)` か `set_worker_count_auto()` を設定してください。CPUを使うPythonのコールバックは、ネイティブの処理がGILを解放しない限り、GILの下で直列に実行されます。

同じエンジンで実行（run / resume）中に、状態の取得、履歴、更新、フォークといった管理系の呼び出しを行うと、`std::logic_error` で失敗します。先にキャンセルして処理をdrainし、完了を待ってください。例外を握りつぶしてはいけません。同じスレッドIDで同時に実行した場合、チェックポイントが書き込まれる順序は不定です。ストアを共有する別々のエンジンの間では、ホスト側での調整が必要です。詳しくは[並行性](concurrency.md)を参照してください。

## PythonのProgramエラー

これらのエラーは、`LocalProgramHost` とQuickJSのProgramコンパイラーから出るものです。背景にある規則は、[ProgramとQuickJS](python-binding.md#program-and-quickjs)にあります。

| エラー | 原因 | 対処 |
|---|---|---|
| ノードから出る `TypeError: get(): incompatible function arguments` | `GraphState.get` が受け取るのは、チャネル名だけです | `state.get("value") or 0` を使います |
| `add_registered_node`、`add_registered_reducer`、`add_registered_condition` が出す `ng.ProgramRegistryError` | ノードタイプ、リデューサー、条件が、このプロセスに登録されていません | 先に `@ng.node`、`ng.NodeFactory.register_type`、`ng.ReducerRegistry.register_reducer`、`ng.ConditionRegistry.register_condition` のいずれかで登録します |
| `P_JS_DEFINE_MISSING` | モジュールが `define()` をエクスポートしていません | `define()` をエクスポートし、そのProgramのグラフを1つ返します |
| `P_JS_GRAPH_ENTRY`、または `__start__` / `__end__` という名前のノードに対する `P_JS_GRAPH_ARGUMENT` | グラフに `.entry()` がないか、ノードが予約済みの名前を使っています | `.entry()` を呼び、ノードの名前を変えます。`.entry()` と `.exit()` が、開始と終了の番兵ノードに接続します |
| `P_CORE_E13` または `P_CORE_E14` | ノードに、最初の条件付きエッジのほかに、静的エッジか2本目の条件付きエッジがあります。どちらも発火しません | すべての行き先を、最初の条件付きエッジのroutesに入れます |
| `P_JS_CONTROL_COMMAND`: JavaScript command references a Core outside the admitted root | `ng.callCore` が、`define()` が返すグラフ以外の名前を指定しています | `ng.graph(...)` に渡した名前を使います |
| `ValueError: Every policy budget ceiling except child budgets must be positive; zero: ...` | 挙げられた上限のフィールドが0です | 正の値にします。Programがモデルを一切呼び出さない場合も同じです |
| `ng.ProgramAdmissionError` の中の `P_ADMIT_PROVIDER` | `llm_call` ノードが、ホストが結び付けていないプロバイダーを必要としています。または、`llm_call` か `intent_classifier` のノードが、`provider=` なしで登録されています | `add_provider(...)` と `add_registered_node("llm_call", ..., provider=...)` を登録し、`LocalProgramHost` に `node_context=` と `binding_label=` を渡します |
| `P_MONEY_BUDGET`（`BudgetExhausted`）、または別の `P_MONEY_*` コード（`Failed`）が出て、プロバイダーにリクエストが届いていない | 上限を決めた呼び出しの予約額が `monetary_microunits` に収まりません（0は何も許可しません）。または、呼び出しの上限を決められません。原因は、プロバイダーのポリシーのカタログにそのモデルの価格がないこと、ルートの上限がないルーターのエイリアス、出力の上限がないこと、入力の上限が分からないこと、価格のない service tier やサーバーツール、使用の上限がないツールのいずれかです | `monetary_microunits` を増やすか、`provider_controls.max_output_tokens` を設定するか、プロバイダーのポリシーのカタログレイヤーでそのモデルに価格を付けます。詳しくは[価格の証拠とモデルカタログ](reference-en.md#pricing-evidence-and-the-model-catalogue)を参照してください |
| `P_MONEY_BUDGET`（`BudgetExhausted`）が出て、`failure.witness["monetary"]` に `granted`、`used`、`over_by` がある | プロバイダーがリクエストを受け取り、その応答が予約額を上回る費用を報告したため（たとえば upstream がリクエストの出力 cap より多い出力を返した場合）、実行の請求額が `monetary_microunits` を超えました | `monetary_microunits` を増やす、呼び出しの費用を下げる、または呼び出しを出力 cap を守る upstream にだけ向けます（provider の routing の `only` または `order`）。`handle.notices` にはそのような呼び出しごとに `P_MONEY_OVER_HOLD` notice があり、`result.monetary_records` で `microunits` が `reserved_microunits` を上回る記録が、その呼び出しです。どちらも、応答が名前を示す場合は、呼び出しを処理したアップストリーム（`served_by`）を持ちます |
| `P_START_BUDGET`: `Program invocation budget <field>=<n> is outside its admitted bounds` | 実行時の予算が、`compile_admit` に渡した予算と異なります | 同じ予算を渡すか、コンパイルとadmissionをやり直します |
| `ng.spawn` が出す `P_CHILD_BINDING` | `LocalProgramHost` に、子のバインディングを解決するリゾルバーがありません | 子Programは、C++のホストで実行します |
| `resume()` が出す `P_RUN_NOT_FOUND`: `run '<id>' was not found in this LocalProgramHost` | その実行は、別の、または破棄済みの `LocalProgramHost` のものです | その実行を開始したホストを通して再開します |
| `reconnect()` が出す `P_RUN_LEASE_HELD` | 子の予算なしで始めた `define()` だけの Program は、`wall_time_ms` の間、実行リースを保持します。所有者がまだ生きているかもしれない実行を、別のランタイムが続行しないようにするためです | 診断の witness にある `retry_after_ms` の後で再試行します。その後、実行は `TimedOut` で終わります。止まった実行を最後のチェックポイントから続行するには、`main()` を使うか、子の予算（`max_child_depth` と `max_total_children` を 0 より大きく）を付けて Program を開始します |
| `fork()` が出す `P_FORK_SOURCE_BUDGET` | フォークの予算が、一時停止した実行に残っている `wall_time_ms` または `max_core_steps` をすべて取るため、その実行が再開できなくなります | フォークには `paused.remaining_budget` の一部（たとえば半分）を渡します。`max_concurrency` はそのまま渡して構いません |
| `P_CORE_BARRIER_STALLED` | バリアのメンバーが、joinにつながらないルートを取ったため、joinはもう発火できません | すべてのメンバーを、joinか `__end__` にルーティングします。そうしたルートは、コンパイル時に `P_CORE_E15` が警告します |
| `P_RUNTIME_TIMEOUT`: `Program wall-time budget (N ms) expired` | 実行が `wall_time_ms` を使い切ったか、その後に完了しました。`failure.witness["wall_time_ms"]` がどれだけ超えたかを示し、中断された操作のテキストは `failure.witness["cause_message"]` にあります | `wall_time_ms` を大きくして、コンパイルとadmissionをやり直します |

`callCore` が失敗すると、実行はその失敗で終わります。`main()` で `try`/`catch` を使っても捕捉できません。Python側で `result.status` と `result.failure` を確認してください。

## チェックポイントとPostgreSQLの障害

PostgresCheckpointStoreがエクスポートされていない場合は、そのwheelやソースビルドでコンポーネントが無効になっている可能性があります。古いwheelの機能一覧ではなく、そのパッケージの構成を確認してください。オプションのターゲットをビルドするときは、対応するlibpqをインストールします。接続URIでは、パスワードの特殊文字をパーセントエンコードするか、libpqのkey=value形式を使ってください。実際の認証情報は、決して公開しないでください。

非同期の接続と再接続では、すべてのホストとIPを通じて、デッドラインは1つです。`connect_timeout=N` に正の値を明示した場合は、秒数として扱われます。ただし `1` は2に切り上げられます。指定がない場合、0または負の値の場合、そしてPGCONNECT_TIMEOUTやサービスファイルでしかタイムアウトが指定されていない場合は、非同期のデフォルトである30秒が使われます。これは、同期のlibpqがホストごとにタイムアウトを適用するのとは異なります。

ストアは、権限があればテーブルを自分で作成します。CREATE権限がない場合は、[PostgreSQLのヘッダー](../include/neograph/graph/postgres_checkpoint.h)にあるスキーマを、自分で適用する必要があります。保留中の書き込みの機能がない場合は、スーパーステップ全体がリプレイされます。外部への効果が1回だけ実行される（exactly-once）ことにはなりません。非同期専用のカスタムバックエンドでは、AsyncCheckpointStoreと `adapt_async_checkpoint_store` を使います。以前のような、同期と非同期の既定実装が互いに呼び合う仕組みは使いません。

## トレーシングのアダプターとその他の落とし穴

トレーサーのアダプターは、記録したデータを自分で所有する必要があります。セッションを閉じたときに破棄されるSpanラッパーへの生のポインターを持ってはいけません。[C++のトレーシングのサンプル](../examples/49_openinference.cpp)を参照してください。Pythonのcontextvarsは、C++のコールバックの境界を自動では越えません。親のコンテキストを明示的に渡すかアタッチし、ラッパーはエンジンのコンパイル前に組み込んでください。

ヘッダーオンリーのhttplibを複数の翻訳単位でインスタンス化するコンシューマーは、CPPHTTPLIB_OPENSSL_SUPPORTの定義をそろえる必要があります。型付きプロバイダーのHTTPディスパッチは、SDKのlibcurlを使います。

便宜的なvectorを返すプロパティは、Pythonのlistではないことがあります。中身を確認するには、イテレーションか `list(value)` を使ってください。ChatMessage.image_urlsのような、コピーとして返されるシーケンスのフィールドは、組み立ててから代入します。返されたコピーを変更すれば、C++のリクエストも変わると思わないでください。

## ツール呼び出し、Hook、プロトコルエラー

グラフの実行中に、ツール呼び出し、ノード、サブグラフが必須のHookによるブロックに遭遇した場合、`NodeExecutionError` で包むのではなく、`llm::Agent` と同じく `HookBoundaryBlocked` そのものが送出されます。このブロックに対する `ERROR` イベントは発行されません。`HookBoundaryBlocked`（Pythonでは `ng.HookBoundaryBlocked`）を捕捉し、`invocation_id` と `state` を読んでください。たとえば入れ子のエンジン実行から、これを送出したPythonノードは、リトライされません。[ツールの失敗に関する契約](reference-en.md#tool-failure-contract)を参照してください。

エンジンの `HookRuntime` は、そのエンジンのツール呼び出しについて、`BeforeToolExecution` と `AfterToolExecution` のHookも実行します。ProgramホストのHookランタイムも、そのCoreのツール呼び出しについて同様です。グラフの実行が、状態 `failed`、メッセージ `lifecycle hook could not run` の `HookBoundaryBlocked` で止まった場合は、実行にオーナースコープ（`RunMetadata::owner_scope`）を与えてください。オーナースコープがないHook境界は、チェックポイントのHookでもツールのHookでも、フェイルクローズになります。

`state == "timed_out"` は、Hookのデッドラインが過ぎたか、その対象がタイムアウトしたことを意味します。最後の試行が失敗したHookは `failed` を報告します。

ステータスが `expired` で、エラーが `run deadline elapsed before the tool started` のツール結果は、呼び出しを始められる前に、実行のデッドラインが過ぎていたことを意味します。ツールもそのHookも実行されていません。呼び出しをリトライするのではなく、`RunMetadata::deadline`（Pythonでは `timeout_ms`、Programでは `wall_time_ms`）を延ばしてください。実行のデッドライン、またはクライアントのリクエストタイムアウトによって止められたMCP呼び出しは、`timed_out` になります。実行をキャンセルすると、実行中のMCP呼び出しはただちに止まります。

`MCPServer` は、クライアントがキャンセルした `tools/call` に対しては、どのトランスポートでも応答を返しません。Hookによるブロックには `-32010`、割り込みには `-32011` で応答します。それ以外の失敗は、すべて `isError` の結果になり、その `_meta["io.neograph/failure"].code` が失敗の種類を示します。テキストは公開用のメッセージで、例外のテキストが使われることはありません（テキストを指定するには、`public_message` を付けて `NodeFailureError` を送出します）。gRPCクライアントは、`OK` のレスポンスの `RunGraphResponse.error` ではなく、ステータスを読む必要があります。失敗した実行は、OKではないステータスになり、`google.rpc.ErrorInfo` の `metadata["code"]` にコードが入ります。詳しくは[プロトコルの失敗のマッピング](reference-en.md#protocol-failure-mapping)を参照してください。

有効なUTF-8でないツール出力や例外メッセージは、不正なバイト列の部分がU+FFFDに置き換えられて届きます。空のツールメッセージ、空のMCPやgRPCのペイロード、Pythonの `UnicodeDecodeError` にはなりません。

## バグを安全に報告するには

バージョンとプラットフォーム、最小限のトポロジーと呼び出し、execution_traceとステータス、機微な情報を伏せた型付きの失敗／停止／使用量のエビデンスを添えてください。失敗が、インストールしたwheelで起きたのか、ビルドし直したチェックアウトで起きたのかも書いてください。認証情報、非公開のプロンプト、エンコードされたネイティブのリクエスト本体、サニタイズしていないパケットキャプチャは、含めないでください。報告先は <https://github.com/fox1245/NeoGraph/issues> です。
