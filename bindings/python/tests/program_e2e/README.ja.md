<!-- neograph-i18n: source=bindings/python/tests/program_e2e/README.md locale=ja source_sha256=e533a404db0c3cc5c518e94ac14f4d9968a8e383470bbdf4de2bb8cfcd56c898 -->
# Program APIのエンドツーエンドのシナリオ

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

これらのテストは、[Pythonガイド](../../../../docs/python-binding.md)を読んだユーザーが行うのと同じ方法で、Pythonバインディングを通して、sealed-v1のJavaScript Program APIを動かします。使うのは、Pythonの `GraphNode` クラス、登録済みの条件、`ProgramRegistryBuilder` のスナップショット、`ProgramCompiler`、`LocalProgramHost` です。ProgramとQuickJSによる制御を有効にしてビルドした `neograph_engine` が必要です。それがない場合、これらのテストはスキップされます。

## カバレッジ

| ファイル | シナリオ |
|---|---|
| `test_01_quickstart.py` | ドキュメントのサンプル、Pythonのノードと条件を持つ `define()` のグラフ、プロセスをまたいだID、アクティベーションとロールバック |
| `test_02_hitl.py` | `interruptBefore`、`NodeInterrupt`、デフォルトと明示指定の保留ID、`main()` の中での割り込み、プロセスの再起動後の再開 |
| `test_03_failures.py` | ノードの例外、fan-outの分岐の失敗、`main()` の例外、失敗した `callCore`、プロバイダーのない `llm_call` |
| `test_04_budgets.py` | コンパイル時と実行時のJavaScriptのタイムアウト、ステップ数と操作数の予算、実時間によるキャンセル、admission済みの予算、元の実行の予算から切り分けたフォークの取り分（#414） |
| `test_05_diagnostics.py` | 構文エラーとビルダーのエラーのソース位置、レジストリのミス、lintのスパン、制御文字、admissionのエラー |
| `test_06_graph_shapes.py` | バリアのjoin、条件付きルーティング、`Send` によるmap-reduce、サブグラフ、不正なグラフの形状 |
| `test_07_main_programs.py` | `callCore`、`all`、`race`、`quorum`、`await` のタイムアウト、`cancelScope`、外部からのキャンセル、`spawn` |
| `test_08_live_llm.py` | ノードがモデルを呼び出すProgram。デフォルトではローカルのpeerを使い、有効にするとOpenRouterを使う。Pythonのノードと、バインド済みの `llm_call` |
| `test_09_bound_llm.py` | `node_context` を通してバインドした `llm_call`（#372）：報告されたコストと推定のコスト、`provider.max_price` として固定したルートの上限、送信前の拒否（予算、未知のモデル、ルーターのエイリアス）、予算0、プロバイダーのないID、1つの予算に対する `all` と `quorum`、呼び出しがジャーナルに記録された後のクラッシュと、それを1回だけ課金する復旧、モデルトークン予算が尽きたときの1つの終了の形（#420）、実行リースに保持された実行のエラー（#421） |
| `test_10_failure_routing.py` | 失敗時のルーティング（#364）：失敗したTESTを、そのレコードとともに `interruptBefore` のハンドラーへルーティングする、リトライと中止のルートに沿った再開、作成者のコード、秘匿したエラーテキスト、`max_visits`、`max_node_retries` の上限と、`main()` がある場合とない場合の残り（#419）、上限と `main()` を併用しても、再試行を終えた失敗がハンドラーで止まること（#418）、ノード内のProgramの診断、SQLiteを使った新しいプロセスでの再開 |
| `test_11_attempt_timeout.py` | 試行のタイムアウト（#364）：タイムアウトしてリトライで回復する遅いPythonのノード、常にタイムアウトして `NG.Timeout` としてハンドラーにルーティングされるノード（ハンドラーがなければ実行が失敗する）、結果にもトレースにも届かない遅れた完了、停止のデッドラインを過ぎてもキャンセルを無視するノード（`NG.EffectUncertain`）、`wall_time_ms` を超えることになるバックオフが、kind `deadline` で止まること |
| `test_12_send_interrupts.py` | `NodeInterrupt` を投げる `Send` のターゲット（#385）：Programがそこで一時停止し、再開するとmapノードと完了済みのSendがリプレイされる。SQLiteでの再起動後の再開。バインド済みの `llm_call` を呼んだ完了済みのSendは、再送信も再課金もされない |
| `test_13_fork_replays_the_paused_step.py` | 完了したバインド済みの `llm_call` の隣で一時停止した実行の `host.fork()`（#390）：フォークは質問に答え、完了した呼び出しを、そのステップの保留中の書き込みからリプレイする。そのため、リクエストは送信されず、トークンもお金も再課金されない。fan-outのステップとSendの両方で、インメモリとSQLiteのジャーナルの両方を確認する |
| `test_14_await_deadline.py` | `ng.await(..., timeout_ms)` が絶対的なデッドラインを保持すること（#391）：デッドライン内の再開は完了し、デッドラインを過ぎた再開は `P_AWAIT_TIMEOUT` ですぐにタイムアウトする（インメモリ、SQLite、再起動したホストから）。再開された await の中でクラッシュした後の `reconnect()` は、その await が開始されたときのデッドラインを適用する |
| `test_15_send_isolation.py` | `Send` のペイロードは、その `Send` だけに見えること（#389）：1つのSendと3つのSend、ワーカー1つと4つ、ライブの場合とSQLiteで再開した後の場合のどちらでも、次のステップにも最終ステートにもペイロードが残らない |
| `test_16_interrupt_identity.py` | 完了したバインド済みの `llm_call` の隣にある、1つのステップの2つのCore割り込み（#388）：保留中の入力が最初のCore割り込みを指す。再開するとそれに答え、2つ目は自分の質問をもう一度出し、次の再開でそれに答える。完了した呼び出しは、再送信も再課金もされない。インメモリとSQLiteのジャーナルの両方を確認する |
| `test_17_resume_value_scope.py` | 回答は、その回答が対象とする質問にだけ届くこと（#393）：バインド済みの `llm_call` の後、`first` が質問し、次のCoreステップで `second` が質問する。`first` に答える再開では、`second` は自分の質問を出し、次の再開でそれに答える。完了した呼び出しは、再送信も再課金もされない。インメモリとSQLiteのジャーナルの両方を確認する |
| `test_18_step_failure_outranks_interrupt.py` | 質問するノードのインデックスにかかわらず、失敗がそのステップを終わらせること（#392）：1つのCoreステップが、質問するノードと失敗するノードの隣で、バインド済みの `llm_call` を実行する（順序は両方を試す）。Programは失敗したノードの分類済みの失敗で失敗し、保留中の質問は残らず、モデル呼び出しは1回だけ送信され、1回だけ課金される。インメモリとSQLiteのジャーナルの両方を確認する |
| `test_19_fork_interrupt_identity.py` | 完了したバインド済みの `llm_call` の隣で、Coreのステップが2つの割り込みで一時停止した実行の `host.fork()`（#394）：フォークは、保留中の呼び出しの `core_interrupt_id` が指す割り込みに答え、もう一方のノードはフォークの中で再び質問する。完了した呼び出しは、再送信も再課金もされない。インメモリとSQLiteのジャーナルの両方を確認する |
| `test_20_javascript_fork.py` | それぞれにバインド済みの `llm_call` を持つ2回目の `callCore` で一時停止した、JavaScriptの `main()` の `host.fork()`（#395）：フォークは、コマンドジャーナルから最初のコマンドをリプレイし、一時停止中の呼び出しを回答とともに再開して完了する。どちらのモデル呼び出しも、再送信も再課金もされない。インメモリとSQLiteのジャーナルの両方を確認する |
| `test_21_fork_recorded_failure.py` | 収集を行う `ng.all` が、バインド済みの `llm_call` を拒否されたメンバーを記録した、JavaScriptの `main()` の `host.fork()`（#398）：フォークは、記録されたプロバイダーのエビデンスを自分自身に再バインドし、一時停止中のメンバーに答え、記録された失敗をそのエビデンスとともに報告する。どちらの呼び出しも再送信されない。インメモリとSQLiteのジャーナルの両方を確認する |

`support.py` には、共有のPythonのノード、条件、レジストリ、予算、JavaScriptのビルダーが入っています。

## 実行方法

リポジトリのルートから実行します。Pythonバインディング、Program、QuickJSによる制御を有効にしたビルドが必要です。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_PYBIND=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON
cmake --build build
PYTHONPATH=build:bindings/python python -m pytest -q bindings/python/tests/program_e2e
```

オフラインのシナリオは、約20秒で終わります。`-m "not slow"` を指定すると、意図的に1秒以上待つシナリオをスキップします。CIは、すべてのpushとプルリクエストで、ネイティブLinuxのレーン（`scripts/verify_ci.py native-linux`）の `bindings/python/tests/` の一部として、これらを実行します。

## ライブモデルのテスト

`test_live_openrouter_model_call_inside_a_program` と `test_live_openrouter_bound_llm_call_is_priced` は、どちらも有料のOpenRouterリクエストを1回ずつ送信します（`max_output_tokens=64`、データ保持なしのルーティング）。後者は、`LocalProgramHost(node_context=...)` を通してバインドした組み込みの `llm_call` を、1,000,000マイクロユニット（1 USD）の金額の予算で実行し、精算されたレコードを確認します。そのリクエストには、カタログのルートの上限が `provider.max_price` として付くので、上限を超えるエンドポイントにルーティングされることはなく、OpenRouterがリクエストを失敗させます。2つの変数の両方を設定しない限り、これらのテストはスキップされます。CIがこれらを設定することはありません。

```bash
NEOGRAPH_E2E_LIVE=1 OPENROUTER_API_KEY=... PYTHONPATH=build:bindings/python \
  python -m pytest -q -rs bindings/python/tests/program_e2e/test_08_live_llm.py
```

キーは、環境変数からだけ読み取ります。`OPENROUTER_MODEL`、`OPENROUTER_PRICED_MODEL`（カタログにルートの上限があるモデル。デフォルトは `deepseek/deepseek-v4-flash-0731`）、`OPENROUTER_BASE_URL` で、モデルとエンドポイントを上書きできます。

## 既知の問題

期待される動作は決まっているがまだ実装されていないシナリオには、`support.known_issue("#NNN", reason)` を付けます。これは、strictな `xfail` です。修正が入るとテストが通りますが、strictモードではそれが失敗として報告されるので、同じ変更の中でマーカーを外す必要があります。現在の失敗を確認するには、次のようにします。

```bash
PYTHONPATH=build:bindings/python python -m pytest -q --runxfail -m known_issue bindings/python/tests/program_e2e
```
