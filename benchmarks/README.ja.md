<!-- neograph-i18n: source=benchmarks/README.md locale=ja source_sha256=bbfba1a87c6a9a4f4f7c7a732df64ce07c6ddacc41c9b8ac7024042f05a40fba -->
# NeoGraphとPythonのグラフ／パイプラインフレームワークの比較 — エンジンのオーバーヘッドベンチマーク

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

I/O、スリープ、モデル呼び出し、永続的なチェックポイントのない 2 つの小さなワークロードで、NeoGraphと
Pythonのオーケストレーションフレームワーク 5 つについて、1 回の実行あたりのエンジンのオーバーヘッドを測定し、
それぞれのメモリ、起動時間、サイズも併せて測定します。測定の対象は、ノードのディスパッチ、ステートチャネルへの書き込み、
リデューサーの呼び出しです。

このページのすべての表とチャートは、チェックイン済みの 1 つの記録（[`records/CURRENT`](records/CURRENT) が指すファイル）から
`scripts/render_bench_report.py` が生成します。記録には、各セルの生のサンプルと、セッションの来歴（コミット、コンパイラとフラグ、
CPU、カーネル、パッケージのバージョン、ロードアベレージ、スクリプトの sha256）が含まれます。表が記録と食い違うとテストが失敗します。
別々のセッションでバージョンを混ぜて測定した以前の表は、現在では無効と分かっている項目の注記とともに
[2026-04 の履歴](history/engine-overhead-2026-04.md)に残してあり、このページの表とは比較できません。
現在のNeoGraph `0.16.0` のレシピには、alpha SDK `0.4.0`（インターフェースのリビジョンと共有世代は7）と、それに合ったヘッダーおよびライブラリーが必要です。

Programのadmission、JavaScriptによる制御、SQLite／PostgreSQLのランタイムのコストについては、[Programのコストの測定](../docs/PROGRAM_COST_MEASUREMENT.md)を参照してください。
そちらのマトリクスにはジャーナルとチェックポイントの処理が含まれているので、下のチェックポイントなしのCoreの比較と並べて示してはいけません。

## コホート

NeoGraphと 5 つのフレームワークは、1 台のマシンで 1 回のセッションの中でインストール、ビルド、測定しました。**HEAD** は
フレームワークの既定ブランチの先頭を、表に示すコミットで git からインストールしたもの、**release** は当時の最新の PyPI リリースです。
release のビルドは対応セルだけを実行するので、HEAD とリリースに差があるかどうかを読み取れます。HEAD のビルドが
インストールまたはインポートに失敗した場合は、リリースで代用し、表にその旨を記します。NeoGraphは、表に示す `origin/master` の
コミットを、ハードニングのフラグを含む既定の Release 構成でビルドしたものです（[エンジン上限](#engine-ceiling-and-the-hardening-delta)を参照）。

<!-- bench-record:begin cohort -->
| フレームワーク | 最新リリース | HEAD（コミット、日付、バージョン） |
| :--- | :--- | :--- |
| LangGraph | langgraph 1.2.16, langchain-core 1.6.12 | langgraph 1.2.16, langchain-core 1.6.12; `langchain@e6d83ef` (2026-10-20), `langgraph@3cd57c0` (2026-10-21) |
| Haystack | haystack-ai 3.4.0 | haystack-ai 3.5.0-rc0; `haystack@276746c` (2026-10-21) |
| pydantic-graph | pydantic-graph 2.59.0 | pydantic-graph 2.59.1.dev4+e0ccafa8a; `pydantic-ai@e0ccafa` (2026-10-21) |
| LlamaIndex Workflow | llama-index-core 0.14.25, llama-index-workflows 2.26.0 | llama-index-core 0.14.25, llama-index-workflows 2.26.0; `llama_index@619d746` (2026-10-14), `workflows-py@f3528e8` (2026-10-21) |
| AutoGen GraphFlow | autogen-agentchat 0.7.5, autogen-core 0.7.5 | autogen-agentchat 0.7.5, autogen-core 0.7.5; `autogen@004749e` (2026-10-16) |

ホスト: ARM Neoverse-N1、4 コア、カーネル 6.17.0-1018-oracle、Python 3.12.3、NeoGraph のビルドに使ったコンパイラ g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0; CPU ガバナー: ホストが公開していない。

セッション: 2026-10-21T13:24:06Z ～ 2026-10-21T13:59:49Z UTC; 1 分間のロードアベレージは、最初のフェーズの開始時（アイドルになるのを待った後）が 0.26、終了時が 0.97（セッション自身のプロセスを含む）; インターリーブラウンド 12 回; 記録 `2026-10-21-compare-arm-neoverse-n1.json`。

NeoGraph エンジン: コミット `db0fc651e2c9`; ハーネスツリー: コミット `10d6c77a7d42`。ハーネスのコミットはセッションの実行元のローカルコミットで、このリポジトリの履歴に存在しない場合があります。記録は編集せず、記録にはハーネスの各スクリプトの sha256 が入っていて、ファイルを特定できます。このツリーのファイルは、ハッシュされたファイルとバイト単位で一致します。例外は `scripts/render_bench_report.py`（これらの表を生成したバージョンのハッシュは下に示します）と、`benchmarks/compare/` の `port_llamaindex.py`・`port_autogen.py`（それぞれセッション後にコメントを 1 か所修正）です。

表は `scripts/render_bench_report.py`（sha256 `db933a39bedf3733`…）が生成しました。
<!-- bench-record:end cohort -->

## ワークロード

すべてのフレームワークが同じ 2 つのワークロードを移植しています。グラフ、パイプライン、ワークフローは計測区間の外で 1 回だけ構築し
（フレームワークが許す限りインスタンスを再利用します）、計測ループで呼び出します。ステートとトポロジーの調整は以下のとおりです。

| Id | 形 | ステート |
|----|-------|-------|
| `seq` | 3 ノードのチェーン `a → b → c` | 単一の `counter` チャネル、各ノードが `counter+1` を書き込む（overwrite リデューサー） |
| `par` | 5 ワーカーへのファンアウトのあと `summarizer` で結合 | `results: list`（append リデューサー）と `count: int`；各ワーカーが自分のインデックスを追加し、summarizer が `len(results)` を書き込む |

すべての実行を検証します。最終ステートが期待どおり（counter が 3、インデックス 5 つと count が 5）でなければなりません。検証は、
各サンプルの最後の実行と、時計を止めたあとの追加の 1 回の実行に対して行います。答えが誤っている、実行が失敗した、完了した実行がない、
のいずれかのセルはエラーセルで、そのタイミングは公開せず、記録にエラーとして残します。

どのフレームワークにも永続的なチェックポインターは設定していません。AutoGen は実行ごとにメッセージスレッドを蓄積します。
これはフレームワークの実行ごとのコストです。

移植の注記：

* **Haystack** には append リデューサーがないため、各ワーカーが自分の型付きソケットに出力し、summarizer がリストの長さを合計します。
  1 回の実行でディスパッチされるコンポーネントの数は同じです。ベンチマークのプロセスではテレメトリーを無効にしています。
* **pydantic-graph** の API は現在 `GraphBuilder` の 1 つだけです。ステップは `async def` で、複数のステップへのエッジはブロードキャストし、
  結合がそれらを待ちます。`par` は本物のファンアウトで、開始ノードから 5 つのステップに送り、`reduce_list_append` で結合して
  summarizer ステップに渡します。ドキュメントで最初に示される `BaseNode` クラスは次のノードをちょうど 1 つだけ返すため、
  その版の `par` は 6 ノードの直列チェーンであり、慣用の既定値のブロックにだけ載せます。
* **LlamaIndex** は `ctx.collect_events` で結合するため、summarizer のステップ関数は結果イベントごとに 1 回（5 回、待機中の 4 回は `None` を返す）
  実行されます。`Workflow` インスタンスは複数の実行にわたって、また同時にも再利用できるので（プローブを参照）、対応する行は
  インスタンスを 1 つ再利用し、慣用の既定値の行は実行ごとに新しいインスタンスを作ります。
* **AutoGen** はステートチャネルではなくメッセージパッシングです。カウンターはメッセージのテキストにエンコードし、summarizer は受け取った
  ワーカーのメッセージを報告し、新しいタスクの前にチームをリセットする AutoGen のドキュメントに倣って `reset()` を毎回の実行前に呼びます。
  これはマネージャーのメッセージスレッドを消します。`reset()` なしで繰り返し実行しても正しい答えが返り（0.7.5：グラフは完了すると
  自分で実行状態をリセットする）、その場合はここでは計測していません。
* **LangGraph** のノードは `ainvoke` では `async def`、`invoke` では通常の `def` で、グラフはそれ以外は同一です。

## 対応するレジーム：比率を載せる唯一のブロック

2 つの実行レジームを比較し、それぞれに専用の NeoGraph の行があります。

* **シングルスレッド。** `worker_count=1` の NeoGraph と、各フレームワークの `async def` ノードによるコルーチン経路：LangGraph の `ainvoke`、
  コルーチンのコンポーネントを使う Haystack の `run_async`、pydantic-graph（唯一の経路）、`async def` ステップの LlamaIndex、
  AutoGen（唯一の経路）。ノードは呼び出し側のスレッドで動きます。
* **スレッドプール。** `set_worker_count_auto()` の NeoGraph と、スレッドプール経路があり計測したフレームワークのその経路：LangGraph の `invoke`
  （ファンアウトのステップを langchain-core が呼び出しごとに作るスレッドプールで実行）と、同期専用コンポーネントを使う Haystack の `run_async`
  （各コンポーネントを `asyncio.to_thread` で実行）。そうした経路がない、または計測していないフレームワークは、理由を添えた `該当なし` の行にし、
  エミュレートはしません。LlamaIndex Workflow は後者です。通常の `def` ステップを受け付けてイベントループの既定のエグゼキューターで実行しますが、
  その経路はこのコホートに含まれません。プールは既定の幅のままです（NeoGraph：ハードウェアスレッドごとに 1 本、ここでは 4 本。Python のエグゼキューター：
  `min(32, コア数 + 4)`）。[`dr_compare` のガイド](dr_compare/README.md)ではこれらを揃えています。`seq` では、どのステップも準備のできたノードが
  1 つだけで、NeoGraph も LangGraph の `invoke` もインラインで実行するため、どちらのプールも使われません。それでも LangGraph の行は、そのフレームワークの
  既定の同期経路です。

比率は、フレームワークの中央値を、同じレジームとワークロードの NeoGraph の行の中央値で割った値です。NeoGraphの `par` はワーカーモードで変わり、
その範囲は 2 つ目の表の下に示します。1 つの比率にまとめることはしません。

表の見出しの下の行は、同じフレームワークの HEAD ビルドと最新リリースです。

### シングルスレッド

<!-- bench-record:begin matched_single -->
| フレームワーク | ソース | `seq` 1 実行あたり µs | `par` 1 実行あたり µs | `seq` 対 NeoGraph | `par` 対 NeoGraph |
| :--- | :--- | ---: | ---: | ---: | ---: |
| **NeoGraph `worker_count=1`** |  | **9.72 [9.68, 10.1]** | **21.5 [21.4, 23.6]** | 1× | 1× |
| Haystack — Pipeline.run_async、コルーチンコンポーネント | HEAD `276746c` (3.5.0-rc0) | 272 [266, 275] | 499 [497, 507] | 27.9× | 23.2× |
| Haystack — Pipeline.run_async、コルーチンコンポーネント | リリース 3.4.0 | 273 [266, 276] | 501 [499, 505] | 28.1× | 23.3× |
| pydantic-graph — GraphBuilder のステップ、本物のファンアウト | HEAD `e0ccafa` (2.59.1.dev4+e0ccafa8a) | 483 [476, 494] | 1,277 [1,252, 1,308] | 49.7× | 59.3× |
| pydantic-graph — GraphBuilder のステップ、本物のファンアウト | リリース 2.59.0 | 487 [480, 501] | 1,271 [1,256, 1,294] | 50.0× | 59.1× |
| LangGraph — ainvoke、async def ノード | HEAD `3cd57c0` (1.2.16) | 1,248 [1,232, 1,273] | 1,777 [1,754, 1,808] | 128× | 82.6× |
| LangGraph — ainvoke、async def ノード | リリース 1.2.16 | 1,258 [1,242, 1,280] | 1,773 [1,750, 1,792] | 129× | 82.4× |
| LlamaIndex Workflow — ワークフローインスタンスを 1 つ再利用 | HEAD `619d746` (0.14.25) | 2,272 [2,219, 2,297] | 7,069 [6,937, 7,136] | 234× | 329× |
| LlamaIndex Workflow — ワークフローインスタンスを 1 つ再利用 | リリース 0.14.25 | 2,364 [2,339, 2,396] | 7,393 [7,235, 7,458] | 243× | 344× |
| AutoGen GraphFlow — GraphFlow を reset して run | HEAD `004749e` (0.7.5) | 4,192 [4,135, 4,286] | 9,543 [9,346, 9,734] | 431× | 444× |
| AutoGen GraphFlow — GraphFlow を reset して run | リリース 0.7.5 | 4,224 [4,115, 4,274] | 9,521 [9,405, 9,645] | 434× | 443× |

数値は 12 回のインターリーブラウンドの中央値で、括弧はラウンド間の [最小, 最大] です。各ラウンドは全セルをシャッフルした順で 1 回ずつ、セルごとに新しいプロセスで実行します。比率は中央値同士の割り算です。
<!-- bench-record:end matched_single -->

![エンジンオーバーヘッドのベンチマーク、対応するシングルスレッドのレジーム：1 実行あたりのマイクロ秒](../docs/images/bench-engine-overhead.svg)

### スレッドプール

<!-- bench-record:begin matched_pool -->
| フレームワーク | ソース | `seq` 1 実行あたり µs | `par` 1 実行あたり µs | `seq` 対 NeoGraph | `par` 対 NeoGraph |
| :--- | :--- | ---: | ---: | ---: | ---: |
| **NeoGraph `set_worker_count_auto()`** |  | **10.2 [10.1, 10.5]** | **86.0 [82.8, 86.9]** | 1× | 1× |
| Haystack — Pipeline.run_async、スレッドで動く同期コンポーネント | HEAD `276746c` (3.5.0-rc0) | 759 [745, 779] | 1,424 [1,389, 1,460] | 74.8× | 16.6× |
| Haystack — Pipeline.run_async、スレッドで動く同期コンポーネント | リリース 3.4.0 | 757 [738, 780] | 1,426 [1,388, 1,459] | 74.5× | 16.6× |
| pydantic-graph |  | 該当なし — スレッドプール経路なし: ステップはコルーチンで、通常の関数は拒否される |  |  |  |
| LangGraph — invoke、通常の def ノード | HEAD `3cd57c0` (1.2.16) | 1,063 [1,049, 1,079] | 3,184 [3,146, 3,224] | 105× | 37.0× |
| LangGraph — invoke、通常の def ノード | リリース 1.2.16 | 1,066 [1,053, 1,088] | 3,194 [3,163, 3,301] | 105× | 37.1× |
| LlamaIndex Workflow |  | 該当なし — 未計測: 通常の `def` ステップを受け付けてイベントループの既定のエグゼキューターで実行するスレッドプール経路があるが、このコホートでは計測していない |  |  |  |
| AutoGen GraphFlow |  | 該当なし — スレッドプール経路なし: 単一スレッドのエージェントランタイム 1 つ |  |  |  |

NeoGraph の `par` は `worker_count=1` で 21.5 µs、`set_worker_count_auto()` で 86.0 µs です（ワーカースレッド 4 本）。
<!-- bench-record:end matched_pool -->

## 慣用の既定値：絶対時間のみ、比率なし

各フレームワークの公式ドキュメントが最初に示す API です。これらの行は互いに違う処理をするため（実行ごとの構築、スレッドの切り替え、直列チェーン）、
絶対時間だけを示し、比率は作りません。

<!-- bench-record:begin idiomatic -->
| フレームワーク | 測定した API | `seq` 1 実行あたり µs | `par` 1 実行あたり µs |
| :--- | :--- | ---: | ---: |
| **NeoGraph** | **NeoGraph `run()` (既定 `worker_count=1`)** | **9.72 [9.68, 10.1]** | **21.5 [21.4, 23.6]** |
| Haystack | Pipeline.run、同期コンポーネント | 143 [143, 144] | 321 [318, 325] |
| pydantic-graph | GraphBuilder のステップ、本物のファンアウト | 483 [476, 494] | 1,277 [1,252, 1,308] |
| pydantic-graph | BaseNode クラス（par は直列チェーン） | 535 [525, 547] | 818 [802, 823] |
| LangGraph | invoke、通常の def ノード | 1,063 [1,049, 1,079] | 3,184 [3,146, 3,224] |
| LlamaIndex Workflow | 実行ごとに新しいワークフローインスタンス | 2,615 [2,577, 2,656] | 7,536 [7,408, 7,729] |
| AutoGen GraphFlow | GraphFlow を reset して run | 4,192 [4,135, 4,286] | 9,543 [9,346, 9,734] |
| AutoGen GraphFlow | 実行ごとに新しい GraphFlow | 5,274 [5,239, 5,372] | 11,280 [11,082, 11,549] |
<!-- bench-record:end idiomatic -->

<a id="engine-ceiling-and-the-hardening-delta"></a>
## エンジンの上限とハードニングの差

上のNeoGraphの行は既定の Release ビルドで、`-O3 -DNDEBUG` に `-D_GLIBCXX_ASSERTIONS`、`-fstack-protector-strong`、
`-D_FORTIFY_SOURCE=2`、位置独立コード、RELRO が加わります。これらを外した 2 つ目のビルド（`-DNEOGRAPH_ENABLE_HARDENING=OFF` に加え、
ディストリビューションのコンパイラが既定で有効にする項目を外す `-U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection`）がエンジンの上限です。
両方のビルドのフラグと、各バイナリがコンパイルされたときのマクロは、表の下に出力しています。

<!-- bench-record:begin ceiling -->
| ビルド | `seq`, `worker_count=1` | `par`, `worker_count=1` | `par`, `set_worker_count_auto()` |
| :--- | ---: | ---: | ---: |
| 既定の Release ビルド（ハードニング）— 見出しの値 | 9.72 [9.68, 10.1] | 21.5 [21.4, 23.6] | 86.0 [82.8, 86.9] |
| ハードニングなしのエンジン上限 | 9.88 [9.86, 10.6] | 20.7 [20.5, 22.2] | 84.6 [81.9, 86.5] |
| ハードニング ÷ 非ハードニング | 0.98× | 1.04× | 1.02× |

| ビルド | µs |
| :--- | ---: |
| `seq`、すべてのライフサイクルイベントを生成してコールバックに配信 | 10.7 [10.7, 11.6] |

既定ビルドのフラグとコンパイラマクロ（バイナリとビルドツリーの出力）:

```text
[hardened] build type: Release; g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
[hardened] NEOGRAPH_ENABLE_HARDENING: ON
[hardened] compile: /usr/bin/c++ -DASIO_NO_DEPRECATED -DASIO_STANDALONE -DNEOGRAPH_BENCH_BUILD_TYPE=\"Release\" -DNEOGRAPH_STATIC_BUILD -I<work>/ng/include -I<work>/ng/deps/yyjson -I<work>/ng/deps/asio/include -I<work>/build-hardened/generated -I<work>/build-hardened/_deps/neograph_schemaprovider-src/src -I<work>/build-hardened/_deps/neograph_schemaprovider-build/generated -O3 -DNDEBUG -std=gnu++20 -fPIE -D_GLIBCXX_ASSERTIONS -fstack-protector-strong -D_FORTIFY_SOURCE=2 -o CMakeFiles/bench_neograph.dir/benchmarks/bench_neograph.cpp.o -c <work>/ng/benchmarks/bench_neograph.cpp
[hardened] macros seen by the binary: _GLIBCXX_ASSERTIONS=True, _FORTIFY_SOURCE=2, stack protector=strong, PIE=True
[unhardened] build type: Release; g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
[unhardened] CMAKE_CXX_FLAGS: -U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection
[unhardened] NEOGRAPH_ENABLE_HARDENING: OFF
[unhardened] compile: /usr/bin/c++ -DASIO_NO_DEPRECATED -DASIO_STANDALONE -DNEOGRAPH_BENCH_BUILD_TYPE=\"Release\" -DNEOGRAPH_STATIC_BUILD -I<work>/ng/include -I<work>/ng/deps/yyjson -I<work>/ng/deps/asio/include -I<work>/build-unhardened/generated -I<work>/build-unhardened/_deps/neograph_schemaprovider-src/src -I<work>/build-unhardened/_deps/neograph_schemaprovider-build/generated -U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection -O3 -DNDEBUG -std=gnu++20 -fPIE -o CMakeFiles/bench_neograph.dir/benchmarks/bench_neograph.cpp.o -c <work>/ng/benchmarks/bench_neograph.cpp
[unhardened] macros seen by the binary: _GLIBCXX_ASSERTIONS=False, _FORTIFY_SOURCE=0, stack protector=none, PIE=True
```
<!-- bench-record:end ceiling -->

## メモリ

数値はすべてプロセス全体のもので、Python では**インタプリタとインポート済みフレームワークを含みます**（NeoGraph では C++ ランタイムと
SchemaProvider ランタイムを含む）。デプロイ済みアプリケーションのメモリではありません。

* **アイドル**は、フレームワークをインポートした後（NeoGraph ではエンジンを構築した後）で、実行を始める前のプロセスです。最初の 2 行は空のベースラインで、
  何もインポートしていない Python インタプリタと、NeoGraph のバイナリです。
* **ピーク**は、対応するセルの計測ループの終了時点の `VmHWM` で、増加量はピークからアイドルを引いた値です。
* **実行中の run あたりの限界メモリ**は、N 個の run を 1 つのノードの中で止めた状態（`seq` は中央のノード、`par` は 5 つのワーカーが結果を書いた後の summarizer）で、
  単一のイベントループ（NeoGraph では `io_context` スレッド 1 本）上に保ち、その時点のプロセスの `VmRSS` と `Pss` を読みます。数値は、run を 1 回完了させ、
  アロケーターに空きメモリの返却を求めたあとの、N=64 と N=1 の差を 63 で割った値です。AutoGen は実行中の run ごとにチームが 1 つ必要で、計測区間の中で作ります。
  他のフレームワークは、コンパイル済みのグラフまたはワークフローのインスタンスを 1 つ共有します。

<!-- bench-record:begin memory -->
| フレームワーク (HEAD) | アイドル: インポートのみ、実行なし (MiB) | `seq` ループ中のピーク (MiB) | `par` ループ中のピーク (MiB) | アイドルからの増加、`seq` / `par` (MiB) |
| :--- | ---: | ---: | ---: | ---: |
| Python インタプリタのみ、インポートなし | 10.9 | — | — | — |
| **NeoGraph**: エンジンを構築したバイナリ、実行なし | 5.3 | 6.1 | 6.2 | 0.8 / 0.9 |
| Haystack | 63.5 | 70.2 | 70.2 | 6.7 / 6.7 |
| pydantic-graph | 25.1 | 26.2 | 26.2 | 1.1 / 1.2 |
| LangGraph | 62.8 | 63.0 | 63.1 | 0.2 / 0.3 |
| LlamaIndex Workflow | 35.5 | 36.7 | 36.7 | 1.1 / 1.2 |
| AutoGen GraphFlow | 42.2 | 42.7 | 42.8 | 0.5 / 0.5 |

**実行中の run あたりの限界メモリ**

| フレームワーク (HEAD) | `seq` run あたり KiB (RSS / PSS) | `par` run あたり KiB (RSS / PSS) |
| :--- | ---: | ---: |
| **NeoGraph** | 33.9 / 33.8 | 49.9 / 50.0 |
| Haystack | 14.2 / 14.2 | 27.3 / 27.4 |
| pydantic-graph | 24.3 / 24.4 | 57.8 / 57.7 |
| LangGraph | 37.1 / 37.3 | 93.8 / 93.8 |
| LlamaIndex Workflow | 58.5 / 58.3 | 119.5 / 119.5 |
| AutoGen GraphFlow | 96.4 / 96.3 | 174.1 / 174.1 |
<!-- bench-record:end memory -->

## 起動時間とサイズ

**コールドスタート**は、親がプロセスを生成した時点から、最初の `seq` の実行が完了して検証された時点までの時間です。親はプロセスを生成する直前に
`CLOCK_MONOTONIC` を読み、プロセスは実行が終わったときに `CLOCK_MONOTONIC` を出力します。ローダーまたはインタプリタの起動、インポート（Python）または
静的初期化（NeoGraph）、グラフの構築、1 回の実行を含み、ページキャッシュは温まっていて、バイトコードキャッシュがある状態です。フレームワークごとに新しいプロセスを
20 回、フレームワーク間でインターリーブして実行し、数値は中央値と [最小, 最大] です。

**サイズ。**ネイティブのバイナリは `strip` 後に測定し、`libneograph_core.a` はビルドしたままのオブジェクトのアーカイブで、WebAssembly のモジュールは
WebAssembly ワークフローのサイズゲートから取っています。Python 環境は、フレームワークとその依存関係の全体を入れた新しい仮想環境の `site-packages` の
ディスク上のサイズから、空の仮想環境を引いた値で、パッケージ数を添えます。ネイティブの実行ファイル、WebAssembly のモジュール、Python 環境は
**デプロイ単位が異なる**ので、表では並べて示しますが、それらの間で比率は作りません。

<!-- bench-record:begin footprint -->
| フレームワーク (HEAD) | プロセス開始から最初の `seq` 結果まで、ms | インポート / ビルド / 初回実行（中央値 ms） |
| :--- | ---: | ---: |
| **NeoGraph** | **2.49 [2.40, 10.7]** | — / 0.55 / 0.09 |
| Haystack | 561 [550, 583] | 395 / 0.84 / 99.6 |
| pydantic-graph | 153 [149, 157] | 72.0 / 0.56 / 12.8 |
| LangGraph | 756 [735, 775] | 683 / 0.72 / 4.13 |
| LlamaIndex Workflow | 314 [309, 330] | 219 / 22.8 / 4.63 |
| AutoGen GraphFlow | 331 [326, 345] | 257 / 0.73 / 8.05 |

| 項目 | デプロイ単位 | サイズ | 計測対象 |
| :--- | :--- | ---: | :--- |
| NeoGraph `bench_footprint`（既定のハードニング Release） | ネイティブ実行ファイル 1 つ | 7.15 MB | 最小のアプリ: ノードを登録し、`seq` をコンパイルして 1 回実行; strip 後 |
| NeoGraph `bench_neograph`（既定のハードニング Release） | ネイティブ実行ファイル 1 つ | 7.15 MB | ベンチマークのバイナリ; strip 後 |
| NeoGraph `libneograph_core.a`（既定のハードニング Release） | ライブラリファイル 1 つ | 25.84 MB | ビルドしたままの静的アーカイブ |
| NeoGraph `bench_footprint`（MinSizeRel と LTO（`-Os`）） | ネイティブ実行ファイル 1 つ | 2.82 MB | 最小のアプリ: ノードを登録し、`seq` をコンパイルして 1 回実行; strip 後 |
| NeoGraph `bench_footprint`（ハードニングなしの Release） | ネイティブ実行ファイル 1 つ | 6.49 MB | 最小のアプリ: ノードを登録し、`seq` をコンパイルして 1 回実行; strip 後 |
| NeoGraph `bench_neograph`（ハードニングなしの Release） | ネイティブ実行ファイル 1 つ | 6.49 MB | ベンチマークのバイナリ; strip 後 |
| NeoGraph `libneograph_core.a`（ハードニングなしの Release） | ライブラリファイル 1 つ | 23.86 MB | ビルドしたままの静的アーカイブ |
| NeoGraph `bench_footprint`（共有 `libneograph_core.so`） | ネイティブ実行ファイル 1 つ | 0.26 MB | 実行ファイルのみ; strip 後 |
| NeoGraph の最小アプリの共有ライブラリ配布 | 実行ファイルとその共有ライブラリ | 10.44 MB | 実行ファイルと NeoGraph・SDK の共有ライブラリ 7 個; strip 後 |
| NeoGraph グラフスモーク `smoke.wasm` (Emscripten 6.0.13, -O3 -flto, Release) | WebAssembly モジュール 1 つ | 5.53 MB / 1.61 MB gzip | CI の WebAssembly ワークフローのサイズゲート、シングルスレッドのジョブ、コミット db0fc651 |
| NeoGraph JavaScript モジュール `neograph_module.wasm` (Emscripten 6.0.13, -O3 -flto, Release) | WebAssembly モジュール 1 つ | 6.29 MB / 1.84 MB gzip | CI の WebAssembly ワークフローのサイズゲート、シングルスレッドのジョブ、コミット db0fc651 |
| Haystack（HEAD のインストール） | Python 環境: フレームワークと依存関係の全体 | 140.75 MB | パッケージ 42 個; site-packages から空の venv を引いた値 |
| pydantic-graph（HEAD のインストール） | Python 環境: フレームワークと依存関係の全体 | 12.03 MB | パッケージ 9 個; site-packages から空の venv を引いた値 |
| LangGraph（HEAD のインストール） | Python 環境: フレームワークと依存関係の全体 | 65.46 MB | パッケージ 38 個; site-packages から空の venv を引いた値 |
| LlamaIndex Workflow（HEAD のインストール） | Python 環境: フレームワークと依存関係の全体 | 216.19 MB | パッケージ 62 個; site-packages から空の venv を引いた値 |
| AutoGen GraphFlow（HEAD のインストール） | Python 環境: フレームワークと依存関係の全体 | 36.93 MB | パッケージ 11 個; site-packages から空の venv を引いた値 |
<!-- bench-record:end footprint -->

## プローブ

行の読み方を左右する問いに、同じセッションの中で答えます。(1) LlamaIndex のワークフローインスタンスは複数の実行にわたって再利用できるか、
(2) LangGraph のファンアウトのコストのうち、呼び出しごとのスレッドプールが占める分はどれだけか、Haystack のワーカーはどのスレッドで動くか、
(3) pydantic-graph は async 専用か、ファンアウトは本物か、(4) ハードニングはエンジンにどれだけのコストを課すか、
(5) 従来の 2 つのハーネスが、同じ pydantic-graph の呼び出しを 227 µs と 70〜100 µs と、なぜ違って測ったのか。

<!-- bench-record:begin probes -->
| # | 結果 |
| :--- | :--- |
| 1 | LlamaIndex: 1 つの `Workflow` インスタンスを繰り返し、また同時に実行できる — 逐次の実行 [3, 13]、同時の実行 [3, 103]、llama-index-workflows 2.26.0。 |
| 1 | LlamaIndex 計測: `seq` は再利用インスタンス 2,272 µs、実行ごとに新規 2,615 µs; `par` は 7,069 µs と 7,536 µs。 |
| 2 | LangGraph: `invoke` は 5 つのワーカーをプールスレッド 1 本で実行し、呼び出しスレッドは使わない; `async def` ノードの `ainvoke` は呼び出しスレッドで実行。計測: `par` は `invoke` 3,184 µs 対 `ainvoke` 1,777 µs（1.79 倍）、`seq` は 1,063 µs 対 1,248 µs（0.85 倍）。 |
| 2 | Haystack: 同期コンポーネントでは `run_async` がワーカーを呼び出しスレッド以外の 3 本に任せる; `run_async` コンポーネントでは `run` と同様に呼び出しスレッドで実行。 |
| 3 | pydantic-graph 2.59.1.dev4+e0ccafa8a: `Graph.run` はコルーチンで、`run_sync` はイベントループ上で駆動するだけ; 通常のステップ関数は拒否される（TypeError: object int can't be used in 'await' expression）; 各 50 ms 待つステップ 5 つがスレッド 1 本で 52 ms で終わるため、ファンアウトは本物で単一スレッド。 |
| 4 | `bench_neograph` のハードニング差: 既定のハードニングビルド対非ハードニングビルド、`seq` 0.98 倍、`worker_count=1` の `par` 1.04 倍、ワーカープールの `par` 1.02 倍。 |
| 5 | pydantic-graph 1.x、レガシー `Graph(nodes=[...])` API、このホストで連続計測: await ループ 79.3 µs/run、同じ run を gather したタスク 87.2 µs、1 つのタスク内のループ 79.2 µs、循環 GC オフ 79.2 µs。ハーネスの形では従来の 227 µs 対 70〜100 µs を説明できず、2 つの数値は別々のセッションと環境のものである。 |
| 5 | 同じレガシー pydantic-graph のループで、オプションの `logfire` パッケージを環境にインストールすると 1 run あたり 498 µs で、クリーンな環境（79.3 µs）の 6.3 倍。計測スパンはパッケージがなければ何もせず、あれば実際のスパンになるため、これを含む共有環境では数値が変わる。 |
<!-- bench-record:end probes -->

## 数値の読み方

1. 比率は、そのレジームの組、ワークロード、ホスト、コホートの表にあるバージョンに固有です。NeoGraphの `par` は 2 つのワーカーモードの間で、
   スレッドプールの表の下に示す倍率だけ異なるので、1 つの「N 倍速い」という数値でエンジンを表すことはできません。
2. 各行はフレームワークとワークロードの測定値であり、コストの原因を切り分けて説明するものではありません。各フレームワークが既定で実行ごとに行う処理
   （コールバック、スパン、ステートのスナップショット、メッセージスレッド）を含みます。
3. ワーカー 1 でのディスパッチと、エンジンが所有するスレッドプールは、異なる実行レジームです。意図するワークロードで両方を測定してください。
   仕事のないファンアウトのベンチマークは調整のコストを見せますが、モデルや I/O のレイテンシーを予測するものではありません。
4. メモリの数値にはインタプリタとインポート済みフレームワークが含まれます。デプロイした Python サービスでエンジンが占める分は、プロセス全体のピークではなく、
   アイドルからの増加量と実行ごとの限界値です。

## 注意事項 — このベンチマークで測っていないもの

* **実際のエージェントのワークロード。**フレームワークの比較には、モデルの推論、ネットワークリクエスト、ツールの I/O が含まれません。
  それらのコストがアプリケーションを支配しうるため、これらの表はエンドツーエンドのエージェントの高速化を示すものではありません。
* **フレームワークに適したワークロード。**AutoGen、LlamaIndex、pydantic-graph はそれぞれ、このベンチマークが扱わないパラダイム（マルチエージェントのチャット、
  イベント駆動の長時間ワークフロー、ステートマシンの制御フロー）に最適化されています。
* **チェックポイントのスループット。**各フレームワークで永続化を有効にすると、シリアライズのコストが支配的になります。それは別のベンチマークです。
* **1 台のホスト、1 回のセッション。**コホートは 4 コアの Arm マシン 1 台で実行しました。比率はプラットフォームとフレームワークのバージョンで動き、
  HEAD とリリースの行は、バージョンだけでどれだけ動くかを示します。
* **公平性。**NeoGraphはハードニングのフラグを含む既定の Release ビルドで（上に出力）、ハードニングなしの行が上限です。Python のフレームワークはすべて、
  それぞれの仮想環境で、記録に残した依存バージョンの素の CPython 3.12 を使い、独自のチューニングはありません。あるフレームワークのためにインストールした
  パッケージが、別のフレームワークのコストを変えないようにするためです。

## 再現手順

現在のソースビルドには、Coreだけのベンチマークを含めて、外部の `SchemaProvider::runtime` パッケージが必要です。CMake 3.20以降は、
明示した `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、インストール済みのランタイム、固定した公開GitHubのソースアーカイブの順で選びます。
`NEOGRAPH_FETCH_SCHEMAPROVIDER` の既定値はONで、インストール済みのパッケージまたは明示したソースを使ってオフラインでビルドするときはOFFにします。
明示したチェックアウトまたはインストール済みのパッケージは、インターフェース／共有世代7を提供している必要があります。
新しい出力は別に保存してください。`records/` 配下の記録は、書き込んだ後に編集しません。

```bash
# Build native Core + v1 Program benchmarks (Release is required for
# representative timings; the default CMake build type is not optimized).
cmake -B build-program-bench -DCMAKE_BUILD_TYPE=Release \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_ASYNC=ON
cmake --build build-program-bench --target \
    bench_neograph bench_program bench_program_dispatch \
    bench_program_serialization_poc bench_program_binary_poc -j

# Positional arguments are iterations, warmup runs, and measured samples.
# bench_neograph additionally accepts par_workers before warmup/samples.
# bench_program's optional fourth argument measures closed-batch outer-run concurrency.
# Its burst rows are throughput-equivalent time, not individual request latency.
./build-program-bench/bench_neograph 10000 5000 1 10 5
./build-program-bench/bench_neograph 10000 5000 auto 10 5
./build-program-bench/bench_program 1000 10 5
./build-program-bench/bench_program 1000 10 5 8
./build-program-bench/bench_program_dispatch 100000 10 5
./build-program-bench/bench_program_serialization_poc 25 100
./build-program-bench/bench_program_binary_poc 25 100

# Build the opt-in protobuf/Cap'n Proto transport-envelope experiment.
# This target alone requires protoc/libprotobuf and capnp/libcapnp.
cmake -S . -B build-program-codec-poc -DCMAKE_BUILD_TYPE=Release \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_PROGRAM_CODEC_POC=ON
cmake --build build-program-codec-poc --target bench_program_codec_poc -j
taskset -c 0 ./build-program-codec-poc/bench_program_codec_poc 25 100
```

ネイティブのベンチマークはいずれも、`config`、`runtime`、`header`、`result` のレコードを出力します。明示的なウォームアップのあとに測定したサンプルの中央値を報告してください。短い実行を1回だけ比べるのは避けてください。`bench_program` はインメモリのストアを使い、プロバイダーやネットワークの呼び出しはありません。`bench_program_dispatch` が測るのは、不変の `ProgramPlan` の検索とディスクリプターの走査だけで、Coreの実行は含みません。
シリアライズのPOC（概念実証）は、オフラインでインメモリの測定です。完了したProgramの実行から、不変のpublicationを取り込んで使います。シリアライズのPOCは、正規化したバイト列の再利用を測ります。バイナリのPOCは、現在の正規JSONのエンベロープと、入れ子になった各レコードの正規バイト列を保持する、長さプレフィックス付きのエンベロープを比べます。バイナリの結果は下限を見る実験であり、永続化の契約を置き換えるものではありません。
`bench_program_codec_poc` は別にオプトインで実行するもので、同じ入れ子の正規バイト列を包むprotobufとCap'n Protoの**トランスポートエンベロープ**を比べます。`*_envelope_only_*` が測るのは、バイト列が用意できたあとのエンベロープの構築だけです。`*_transport_total_lower_bound_*` は入れ子の正規バイト列の構築も含みますが、`ProgramTransitionPublication` のレコード間にまたがる外側の検証は、意図的に省いています。永続化やIDフォーマットのベンチマークではありません。復元（recovery）の指標は、所有権を持つProgramレコードにパースし直します。受信側のコンシューマー全体を模したコーデックの数値は、これだけです。どのPOCも、SQLiteやPostgresのトランザクションのレイテンシーや、`ProgramRuntime` のエンドツーエンドのレイテンシーは測定していません。

フレームワークの比較には、フレームワークとソースごとに仮想環境が 1 つずつ必要です。あるフレームワークのためにインストールしたものが、
別のフレームワークのコストを変えないようにするためです。バージョンは [`requirements-compare.txt`](requirements-compare.txt)（最新リリース）と
[`requirements-compare-head.txt`](requirements-compare-head.txt)（git のコミット）に固定してあります。`compare/driver.py` はセッション全体を
インターリーブしたラウンドで、セルごとに新しいプロセスで実行して記録を書き出し、エラーセルが 1 つでもあれば終了ステータスが 0 以外になります。

```bash
# One virtual environment per framework and per source, from the pinned requirement files
# (creates $WORK/venvs/<framework>-release and $WORK/venvs/<framework>-head):
benchmarks/compare/setup_venvs.sh "$WORK"

# NeoGraph: the default hardened Release build and the unhardened engine ceiling.
cmake -S . -B "$WORK/build-hardened" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_EXAMPLES=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$WORK/build-hardened" --target bench_neograph bench_footprint
cmake -S . -B "$WORK/build-unhardened" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_ENABLE_HARDENING=OFF \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_EXAMPLES=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "-DCMAKE_CXX_FLAGS=-U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection" \
    "-DCMAKE_C_FLAGS=-U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection"
cmake --build "$WORK/build-unhardened" --target bench_neograph bench_footprint

# One cell by hand; the last line of standard output is the record:
"$WORK/venvs/langgraph-head/bin/python" benchmarks/compare/cell.py --framework langgraph \
    --variant ainvoke_async --workload seq --iters 600 --samples 3
"$WORK/build-hardened/bench_neograph" 80000 36000 1 10 5 --rows=seq --record

# The whole session on a quiet machine (it waits for a load average under 0.3), then the tables and the chart:
python3 benchmarks/compare/driver.py run --work "$WORK" --out record.json \
    --harness-commit "$(git rev-parse HEAD)" --engine-commit "$(git rev-parse HEAD)" \
    --harness-dirty "$([ -n "$(git status --porcelain)" ] && echo true || echo false)"
python3 scripts/render_bench_report.py --record record.json
```

サービスを使うチェックポイント、HTTP、並行実行のDockerベンチマークは、別の実験です。上で説明した、決定的なネイティブのCore／Programの実行には、これらは不要です。

出力形式は、タブ区切りの `config`、`runtime`、`header`、`result`、`metric` のレコードです。ネイティブの結果の行には、中央値の合計時間と
1 反復あたりの時間が入っています。`--record` を付けると、`bench_neograph` と `bench_footprint` は `record<TAB>{json}` の行も 1 行出力し、
`benchmarks/compare/cell.py` のすべてのセルは、そうした行を最後の行として出力します。生のサンプル、完了数と失敗数、結果の検証、プロセスのメモリが入っています。
答えが誤っている、実行が失敗した、何も完了しなかった、のいずれかのセルは、`status` が `error` で、タイミングがなく、終了ステータスが 0 以外になります。

## 以前の結果

2026-04（x86_64）の表と、2026-07 の Arm64 のベースラインは、バージョンと日付、および現在では無効と分かっている項目の注記とともに
[履歴](history/engine-overhead-2026-04.md)に移しました。2026-04-19 の測定環境のブロックもそちらにあります。以下の型付きプロバイダーの節は
NeoGraphだけを測定したもので、変更していません。

## CIの性能ゲート

`CI extended` ワークフローの2つのジョブが、ベンチマークの結果を検査します。このワークフローは、毎晩、手動実行のとき、ゲートのファイルが変わったときに動きます。

* **`bench-ab`** は、テスト対象のコミットとベース（base）のコミットを同じランナーでビルドし、[`ci_ab_gates.json`](ci_ab_gates.json) に書かれたベンチマークを両方で実行して、ケースごとに中央値を比較します。ホステッドランナー同士の差は、検出する価値のある回帰よりも大きいので、1台のマシンで2つのビルドを並べて測らなければ、数十パーセントの速度低下は見えません。
* **`bench-regression`**（`scripts/check_ci_benchmarks.py`）は、HTTP、ファンアウト、Coreのベンチマークを1回ずつ実行し、絶対的な上限・下限と照らし合わせます。ホステッドランナーでは余裕が大きいので、これらの限界値は最後の砦としてだけ残してあります。

`bench-ab` が実行するもの:

| ワークロード | ベンチマーク | ケース |
|---|---|---|
| Coreの実行経路 | `bench_neograph` | 直列のチェーン、同じチェーンをsyncノードにしたもの、呼び出し側のexecutorとエンジンのワーカープールで動かす5本のファンアウト、何も受け取らないストリームコールバックとすべてのイベントを受け取るストリームコールバックを付けたチェーン |
| 非同期のファンアウト | `bench_async_fanout` | タイマーコルーチン50,000個のスループットとピークメモリ |
| HTTP | `bench_async_http` | ループバックインターフェースのプロセス内サーバーへ、プールした接続で送る呼び出しのスループット |
| Program | `bench_program_dispatch`、`bench_program` | プランのディスパッチ記述子の走査、インメモリのストアでadmission済みのProgramを動かしたときの `start()` と実行全体 |
| チェックポイントのストア | `bench_checkpoint_store` | インメモリのストアとSQLite（プロセス専用の一時ディレクトリ）への1秒あたりの保存回数 |
| canonical JSONとidentity | `bench_canonical_json`、`bench_sha256` | ASCII、混在、エスケープの64 KiB文字列のcanonicalバイト列、64 KiBレコードのSHA-256 identity |
| プロバイダーのoutcome | `bench_provider_outcome_codec` | チャットのoutcomeとツール呼び出しを含むターンのエンコードとデコード |

どれも、ループバックインターフェースを超えるネットワーク、キー、データベースサービスは必要としません。

### 比較の進め方

1. **ベース。** ブランチやプルリクエストはmasterとのmerge baseと、masterはその直前の最新のリリースタグと比較します。手動実行では、別のコミットやタグ、あるいはビルドを自分自身と比べる `head`（A/A実行）を指定できます。ベースは、そのコミット自身のツリーから同じオプションでビルドします。
2. **サンプル。** 各ワークロードは、エントリに書かれた回数だけ、サンプルごとに新しいプロセスで実行します。サンプルの番号に応じてheadとbaseが交互に先に走るので、実行中にマシンの速度が少しずつ変わっても、両方に同じように影響します。
3. **判定。** 各ケースで、headのサンプルの中央値とbaseのサンプルの中央値を比べます。比が `tolerance` を上回る（小さいほど良い場合）か下回る（大きいほど良い場合）とき、またはheadの中央値が `floor` の絶対的な限界を超えたときは失敗です。
4. **再確認。** 失敗したケースがあるワークロードは、新しいサンプルでもう1回実行し、再実行の結果だけで判定します。失敗してから合格したケースは、警告として報告します。
5. **baseなし。** baseのビルドにそのベンチマークや出力がないワークロードは、baseなしとして報告し、失敗にはしません。新しいベンチマークを追加できるようにするためです。絶対的な限界は引き続き適用されます。
6. **レポート。** ジョブのログとステップのサマリーに比の表が残り、ジョブは、すべてのサンプル、比、マシン、各バイナリのSHA-256を含むJSONファイル `bench-ab-report` をアップロードします。

### 許容値

[`ci_ab_gates.json`](ci_ab_gates.json) には、ゲートが依存するものがすべて入っています。ワークロードごとのベンチマークのターゲット、引数、サンプル数、そしてケースごとの、出力から読む値、方向、許容値（tolerance）、任意の絶対的な限界、許容値の較正に使ったノイズ（`calibrated_on` が指す実行で測った `a_a_noise`）です。`a_a_noise` は、A/A実行で中央値の比が1から最も離れた距離です。ランナーは、許容値がその距離の `minimum_noise_margin`（1.5）倍に満たないファイルと、元のゲートの絶対的な限界が1つでも欠けているファイルを受け付けません。

ノイズはマシンによって違うので、ゲートに使うランナーで較正してください。`bench_ab_base` を `head` にして `CI extended` を実行すると、再実行なしでビルドを自分自身と5回比較し、レポートに各回の比が並びます。許容値は、1に、0.15とノイズの2倍のうち大きいほうを足した値です（大きいほど良い場合は、その値を引いた値）。あるケースを許容値の外へ意図的に動かす変更は、同じプルリクエストで許容値と較正の値を直して記録します。

### ローカルでの実行

```bash
python3 scripts/verify_ci.py bench-ab --work-dir /tmp/bench-ab --jobs 4 --base v0.15.1
python3 scripts/verify_ci.py bench-ab --work-dir /tmp/bench-ab-aa --jobs 4 --base head --calibrate 5
# Two build directories that already exist:
python3 scripts/run_ci_ab_gates.py --head-dir build --base-dir build-base --output report.json
```

`verify_ci.py bench-ab` がビルドするのは、ファイルに書かれたベンチマークのターゲットだけです。マシンがコンパイルも行っている間は測定値がぶれるので、絶対的な時間ではなく比を読むか、静かなマシンを使ってください。

## 型付きプロバイダーへの切り替え：実際のGraphEngineでの前後比較

以下の切り替え、最終検証、handoffの測定は、SDKのインターフェース／共有世代3で記録したものです。これらの記録にある「current」と「final」は、当時のコホートを指しており、現在のリリースではありません。

これは、上のチェックポイントなしのPythonフレームワーク比較とは別の、**ローカルのTLS HTTP/SSE**の測定です。モデルの推論は含みません。本番の `GraphEngine.llm_call/tool_dispatch` を通して、Linux x64向けのstatic Release／GCC 13.3ビルドで、**16構成×新規プロセス3回＝48レコード**を測定しました。内訳は、共通のH1ワークロード3つ、5つのファミリーすべてのバッファ型／SSEのネイティブcontinuation、H2のケース3つです。有料のAPIは呼んでいません。

[元の9レコード](provider-cutover-legacy-results.json)は、未変更の `7b47ad43` で取得したものです。[現在の生レコード](provider-cutover-current-results.json)と[スカラー値の比較](provider-cutover-summary.json)には、実効のコントロール、分布、RSSとスレッド数、実際のpeerカウンター、所有された結果が残っています。

| 共通のグラフワークロード | 前のp50 ms | 後のp50 ms | 前のグラフ実行/s | 後のグラフ実行/s | 前のピークRSS MiB | 後のピークRSS MiB |
|---|---:|---:|---:|---:|---:|---:|
| H1 バッファ型のテキスト | 0.707 | 1.320 | 1,375.58 | 733.65 | 12.617 | 15.465 |
| H1 バッファ型のツールループ | 29.337 | 30.510 | 800.30 | 702.64 | 17.832 | 20.219 |
| H1 SSEのツールループ | 600.540 | 37.593 | 51.43 | 615.38 | 14.414 | 20.367 |

値は3回の繰り返しの中央値で、HTTPリクエスト単位ではなく**グラフ実行単位**です。テキストは並行数1でpeerの遅延なし、ツールループは並行数32でリクエストごとに5 msの遅延、グラフ実行ごとに生成リクエストが2回です。どのコホートも、ウォームアップを10回、測定を100回行います。ツールとネイティブのコホートでは、繰り返しごとにキャンセルも8回リクエストします。

現在のコホート全体で、測定中に失敗した実行はありませんでした。

トレードオフもあります。所有された生／ネイティブ／nullableの結果をすべて保持し、権限のゲートを通すため、テキストのレイテンシーとメモリは増えます。バッファ型のツールのp50も少し増えます。一方、SSEでは、従来のバッファ型の経路にあった遅延がなくなりました。従来の実装では、ネイティブ／nullableの権限や一部のワーカーの設定がサポートされていなかったので、この比較は同じワークロードでの測定にとどまります。意味やリソースが同等であることも、モデルが速くなることも示していません。現在のp95/p99、最初の意味イベント、キャンセルが落ち着くまでの時間、拡張ファミリーの行は、JSONのサマリーにあります。

```sh
cmake -S . -B build-provider-bench -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DNEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=OFF \
  -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_PROGRAM=OFF -DNEOGRAPH_BUILD_BENCHMARKS=ON \
  -DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR="$SCHEMAPROVIDER_SOURCE_DIR" \
  -DSP_BUILD_TESTS=OFF -DSP_BUILD_BENCHMARKS=OFF
cmake --build build-provider-bench --target neograph_provider_cutover_benchmark
for config in benchmarks/provider_cutover_h1_*.json benchmarks/provider_cutover_extended_*.json; do
  build-provider-bench/neograph_provider_cutover_benchmark --config "$config" || exit "$?"
done
```

NeoGraphのルートから、Node.js／OpenSSLのCLIとSDKの通常の依存を用意して実行します。peerは一時的なプライベートCAを使います。システムの信頼ストアも、依存ライブラリも置き換えません。

## 最終検証コホート：新規の型付きプロバイダーでのGraphEngineの測定

同じGraphEngineとローカルTLS HTTP/SSEの構成で、あらためて実行したReleaseコホートは、**16構成×新規プロセス3回の繰り返し＝48レコード、失敗0件、38.29秒**で完了しました。有料のモデル呼び出しはありません。上の過去の表を置き換えるものではありません。[最終のスカラー値のサマリー](provider-cutover-final-summary.json)と[最終の生の所有された合成オブジェクト](provider-cutover-final-results.json)は、[従来の9レコードのコホート](provider-cutover-legacy-results.json)、[以前の現在の48レコードのコホート](provider-cutover-current-results.json)、[以前の比較](provider-cutover-summary.json)とは別のファイルです。約60 MBの生ファイルに入っているのは、実際の合成の結果オブジェクトで、プロバイダーのシークレットは含みません。

以下の値は、サマリーにある、独立した3回のプロセスの中央値で、**グラフ実行単位**です。モデルのトークンやHTTPリクエストの単位ではありません。リソースやコントロールの違い、およびネイティブ／nullableの権限の違いは残っています。**意味やリソースが同等であるとは言えません。** これらはローカルでの測定によるトレードオフであり、モデルが速くなることを示すものではありません。

### 最終の16構成の測定結果

| ファミリー | HTTP | ワークロード | p50 ms | p95 ms | p99 ms | グラフ実行/s | ピークRSS MiB | ピークスレッド数 |
|---|---|---|---:|---:|---:|---:|---:|---:|
| `openai.chat` | H1 | バッファ型のテキスト | 1.296943 | 1.349401 | 1.397812 | 769.975584 | 15.898438 | 3 |
| `openai.chat` | H1 | バッファ型のツール | 30.672507 | 100.492667 | 112.663949 | 678.507993 | 20.585938 | 19 |
| `openai.chat` | H1 | SSEのツール | 35.800113 | 105.431034 | 117.559931 | 627.047345 | 21.042969 | 19 |
| `openai.chat` | H1 | バッファ型のネイティブ | 32.546693 | 102.680190 | 115.244483 | 683.637830 | 20.925781 | 19 |
| `openai.chat` | H1 | SSEのネイティブ | 38.135188 | 115.463732 | 126.668501 | 615.357297 | 21.902344 | 19 |
| `anthropic.messages` | H1 | バッファ型のネイティブ | 30.612805 | 103.979300 | 123.110448 | 678.785191 | 20.324219 | 19 |
| `anthropic.messages` | H1 | SSEのネイティブ | 44.021986 | 71.548005 | 93.941146 | 604.744184 | 22.125000 | 19 |
| `openai.responses` | H1 | バッファ型のネイティブ | 35.070430 | 100.615954 | 114.597387 | 647.751880 | 22.179688 | 19 |
| `openai.responses` | H1 | SSEのネイティブ | 46.670934 | 77.636952 | 89.664406 | 571.458359 | 25.511719 | 19 |
| `google.generate` | H1 | バッファ型のネイティブ | 31.449930 | 99.319459 | 112.385528 | 698.783682 | 21.726562 | 19 |
| `google.generate` | H1 | SSEのネイティブ | 37.774704 | 105.703383 | 120.826886 | 627.040287 | 22.500000 | 19 |
| `google.interactions` | H1 | バッファ型のネイティブ | 32.588486 | 101.365734 | 109.841457 | 690.967042 | 21.664062 | 19 |
| `google.interactions` | H1 | SSEのネイティブ | 46.786625 | 76.036675 | 92.590316 | 573.469300 | 23.058594 | 19 |
| `openai.chat` | H2 | バッファ型のテキスト | 1.311873 | 2.443117 | 2.604176 | 701.785481 | 16.148438 | 3 |
| `openai.chat` | H2 | SSEのツール | 40.349311 | 54.813608 | 59.912728 | 714.064349 | 20.386719 | 19 |
| `google.interactions` | H2 | SSEのネイティブ | 44.561127 | 56.692743 | 64.665852 | 663.966743 | 22.750000 | 19 |

### 共通のH1ワークロード3つ：未変更の従来のコホートと最終コホート

| H1のグラフワークロード | 前のp50 ms | 最終のp50 ms | 前のグラフ実行/s | 最終のグラフ実行/s |
|---|---:|---:|---:|---:|
| バッファ型のテキスト | 0.706631 | 1.296943 | 1375.578826 | 769.975584 |
| バッファ型のツール | 29.336983 | 30.672507 | 800.301406 | 678.507993 |
| SSEのツール | 600.539987 | 35.800113 | 51.429767 | 627.047345 |

「前」の値は、元の7b47ad43のコホートです。テキストはレイテンシーが増えてスループットが下がり、バッファ型のツールは変化が小さく、SSEは従来のバッファ型の経路にあった遅延がなくなりました。拡張ファミリー／プロトコル、最初の意味イベント、キャンセル、ネイティブのリプレイ、保持した結果は、最終のサマリーと生レコードにあります。

## イベント駆動のProvider handoff：同条件のポーリングと、集約したチャネルの比較

[測定サマリー](provider-notification-summary.json)：コホートごとに8構成×新規プロセス3回＝48レコード、測定したグラフ実行は4,800回、失敗は0件でした。GCC 13.3のReleaseビルド（静的リンク、ハードニング）で、ネイティブ最適化はOFF、同じローカルTLSのオラクルと許可済みのコントロールを使い、プロセスごとにウォームアップ10回、測定100回です。同時に走らせたコンパイル作業も、モデルの推論もありません。値はプロセスの統計の中央値で、信頼区間でもモデルのトークンレートでもありません。下のペイロードサイズはフィクスチャのテキストのパディング量で、0でも253バイトの応答エンベロープが付きます。

| H1ワークロード | 前のp50 ms | 後のp50 ms | 前のグラフ実行/s | 後のグラフ実行/s |
|---|---:|---:|---:|---:|
| text256、並行数1 | 1.302474 | 0.952992 | 738.901074 | 1022.958838 |
| text0、並行数1 | 1.286646 | 0.912571 | 761.940993 | 1070.657541 |
| text4KiB、並行数1 | 1.387636 | 1.128831 | 709.024147 | 870.631703 |
| text64KiB、並行数1 | 4.873767 | 4.144619 | 196.243991 | 240.268946 |
| text256、peerの遅延5 ms | 6.601920 | 6.425313 | 146.906115 | 155.052016 |
| text256、並行数32 | 7.641085 | 6.939675 | 1645.559595 | 1803.087419 |
| バッファ型のツール、並行数32 | 30.548201 | 31.348443 | 710.487696 | 692.440963 |
| SSEのツール、並行数32 | 33.597342 | 38.607118 | 655.586834 | 611.740359 |

小さなテキストでは、p50が26.83%短くなり、スループットが38.44%上がりました。ツールとSSEのスループットは、それぞれ2.54%と6.69%下がっています。「後」の側は、ポーリングの代わりに、容量1の並行チャネルでプロバイダーの通知を集約します。別に計装した180オペレーションの実行では、SDKのpublicationからdrainまでのp50が、前の383.7125 µsから後の36.4165 µsになり、タイマー待ちは298回から0回に減り、通知待ちは180回でした。これらのプローブの数値は、本番のレイテンシーの表には含めていません。

リソースのピークは1 msごとのサンプルです。これらの測定は、Linux/POSIXでのみ行いました。
