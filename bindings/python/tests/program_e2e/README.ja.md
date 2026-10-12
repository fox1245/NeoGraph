<!-- neograph-i18n: source=bindings/python/tests/program_e2e/README.md locale=ja source_sha256=c1301d128f23b62e0d1095d65f2be8d37b62e268bdc31d3ab520c21222522243 -->
# Program API エンドツーエンドシナリオ

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

これらのテストは、[Python ガイド](../../../../docs/python-binding.md)を読んだユーザーと同じ方法で、Python バインディングを通じて sealed-v1 JavaScript Program API を動かします。Python の `GraphNode` クラス、登録済みの条件、`ProgramRegistryBuilder` のスナップショット、`ProgramCompiler`、`LocalProgramHost` を使います。Program と QuickJS 制御を有効にしてビルドした `neograph_engine` が必要で、どちらかが無い場合このパッケージはスキップされます。

## 対象範囲

| ファイル | シナリオ |
|---|---|
| `test_01_quickstart.py` | ドキュメントの例、Python ノードと条件を使う `define()` グラフ、プロセスをまたぐ識別子、アクティベーションとロールバック |
| `test_02_hitl.py` | `interruptBefore`、`NodeInterrupt`、既定および明示的な pending id、`main()` 内の割り込み、プロセス再起動後の再開 |
| `test_03_failures.py` | ノードの例外、fan-out 分岐の失敗、`main()` の throw、失敗した `callCore`、プロバイダーの無い `llm_call` |
| `test_04_budgets.py` | コンパイル時と実行時の JavaScript タイムアウト、ステップと操作の予算、実時間によるキャンセル、承認済み予算 |
| `test_05_diagnostics.py` | 構文エラーとビルダーエラーのソース位置、レジストリの欠落、lint のスパン、制御文字、承認エラー |
| `test_06_graph_shapes.py` | バリア結合、条件付きルーティング、`Send` によるマップリデュース、サブグラフ、不正なグラフ形状 |
| `test_07_main_programs.py` | `callCore`、`all`、`race`、`quorum`、`await` のタイムアウト、`cancelScope`、外部キャンセル、`spawn` |
| `test_08_live_llm.py` | ノードがモデルを呼び出す Program：既定はローカルピア、有効にすると OpenRouter。Python ノードとバインドされた `llm_call` |
| `test_09_bound_llm.py` | `node_context` でバインドした `llm_call`（#372）：報告費用と推定費用、`provider.max_price` として固定した route ceiling、送信前の拒否（予算、未知のモデル、router alias）、ゼロ予算、プロバイダーのない Program の id、1 つの予算に対する `all` と `quorum`、呼び出しが journal に記録された後のクラッシュと 1 回だけ請求する復旧 |
| `test_10_failure_routing.py` | 失敗のルーティング（#364）：失敗した TEST がレコードとともに `interruptBefore` のハンドラーへルーティングされる、retry と abort のルートでの再開、作成者コード、伏せたエラーテキスト、`max_visits`、`max_node_retries` の上限、ノード内の Program 診断、SQLite を使う新しいプロセスでの再開 |
| `test_11_attempt_timeout.py` | 試行タイムアウト（#364）：タイムアウトしてリトライで回復する遅い Python ノード、常にタイムアウトして `NG.Timeout` としてハンドラーにルーティングされるノード（ハンドラーがなければ実行が失敗）、結果にもトレースにも現れない遅れた完了、停止の期限を過ぎてもキャンセルを無視するノード（`NG.EffectUncertain`）、`wall_time_ms` を越えるバックオフが kind `deadline` で止まる場合 |
| `test_12_send_interrupts.py` | `NodeInterrupt` を送出する `Send` target（#385）：Program がそこで一時停止する、再開時に map ノードと終了した Send が再生される、SQLite を使う再起動後の再開、バインドされた `llm_call` を呼んで終了した Send は再送も再課金もされない |
| `test_13_fork_replays_the_paused_step.py` | 完了したバインド済み `llm_call` の横で一時停止した実行の `host.fork()`（#390）：フォークが質問に答え、完了した呼び出しをステップの pending write から再生するため、リクエストは再送されず、トークンと費用も再課金されない。fan-out ステップと Send、メモリと SQLite ジャーナルの両方 |
| `test_14_await_deadline.py` | `ng.await(..., timeout_ms)` が絶対期限を保つ（#391）：期限内の再開は完了し、期限後の再開は直ちに `P_AWAIT_TIMEOUT` でタイムアウトする（メモリ、SQLite、再起動したホスト）。再開した await の中でクラッシュした後の `reconnect()` も、await が開始したときの期限を適用する |
| `test_15_send_isolation.py` | `Send` の payload は自身の `Send` にだけ見える（#389）：Send 一つと三つ、ワーカー一つと四つ、実行時と SQLite での再開後のどちらでも、次のステップや最終状態に payload を残さない |
| `test_16_interrupt_identity.py` | 完了したバインド済み `llm_call` の横で一つのステップに起きた二つの Core 割り込み（#388）：pending input が最初の Core 割り込みを指定し、再開がそれに答えると二つ目は自身の質問をもう一度行い、次の再開がそれに答える。完了した呼び出しは再送も再課金もされない（メモリと SQLite ジャーナル） |
| `test_17_resume_value_scope.py` | 答えはそれが答える質問にだけ届く（#393）：バインド済み `llm_call` の後に `first` が尋ね、次の Core ステップで `second` が尋ねる。`first` に答えた再開の後も `second` は自身の質問を行い、次の再開がそれに答える。完了した呼び出しは再送も再課金もされない（メモリと SQLite ジャーナル） |

`support.py` には、共有の Python ノード、条件、レジストリ、予算、JavaScript ビルダーがあります。

## 実行

リポジトリのルートで、Python バインディング、Program、QuickJS 制御を有効にしたビルドを使って実行します。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_PYBIND=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON
cmake --build build
PYTHONPATH=build:bindings/python python -m pytest -q bindings/python/tests/program_e2e
```

オフラインのシナリオは約 20 秒かかります。`-m "not slow"` を付けると、意図的に 1 秒以上待つシナリオをスキップします。CI は、すべての push と pull request で、ネイティブ Linux レーン（`scripts/verify_ci.py native-linux`）の `bindings/python/tests/` の一部としてこれらを実行します。

## ライブモデルテスト

`test_live_openrouter_model_call_inside_a_program` と `test_live_openrouter_bound_llm_call_is_priced` は、それぞれ有料の OpenRouter リクエストを 1 回送ります（`max_output_tokens=64`、データ非保持のルーティング）。2 つ目は `LocalProgramHost(node_context=...)` でバインドした組み込み `llm_call` を 1,000,000 microunit（1 USD）の金額予算で実行し、精算済みの記録を確認します。このリクエストはカタログの route ceiling を `provider.max_price` として載せるので、OpenRouter は ceiling より高く掲載された endpoint にルーティングせず、リクエストを失敗させます。両方の変数が設定されていない限りスキップされ、CI はこれらを設定しません。

```bash
NEOGRAPH_E2E_LIVE=1 OPENROUTER_API_KEY=... PYTHONPATH=build:bindings/python \
  python -m pytest -q -rs bindings/python/tests/program_e2e/test_08_live_llm.py
```

キーは環境変数からのみ読み取ります。`OPENROUTER_MODEL`、`OPENROUTER_PRICED_MODEL`（カタログ項目に route ceiling のあるモデル、既定は `deepseek/deepseek-v4-flash-0731`）、`OPENROUTER_BASE_URL` でモデルとエンドポイントを変更できます。

## 既知の問題

意図した動作は決まっているがまだ実装されていないシナリオには、strict な `xfail` である `support.known_issue("#NNN", reason)` を付けます。修正が入るとテストが成功し、strict モードはそれを失敗として報告するため、同じ変更でマーカーを削除する必要があります。現在の失敗を確認するには次を実行します。

```bash
PYTHONPATH=build:bindings/python python -m pytest -q --runxfail -m known_issue bindings/python/tests/program_e2e
```
