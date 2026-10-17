<!-- neograph-i18n: source=benchmarks/README.md locale=ja source_sha256=a8188a12968245c00f0158e28732bd9f4f30df25b77b3f2eb69c3dbba3c6843b -->
# NeoGraphとPythonのグラフ／パイプラインフレームワークの比較 — エンジンのオーバーヘッドベンチマーク

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

I/O、スリープ、モデル呼び出しのない小さな同等のワークロードで、NeoGraphとPythonのオーケストレーションフレームワークについて、1回の呼び出しあたりのエンジンのオーバーヘッドを測定します。測定の対象は、ノードのディスパッチ、ステートチャネルへの書き込み、リデューサーの呼び出しです。
日付付きの表に載っているエンジンのラベルと依存バージョンは、当時のものです。現在のパッケージのバージョンを示すものではありません。
現在のNeoGraph `0.14.2` のレシピには、alpha SDK `0.3.3`（インターフェースのリビジョンと共有世代は6）と、それに合ったヘッダーおよびライブラリーが必要です。

Programのadmission、JavaScriptによる制御、SQLite／PostgreSQLのランタイムのコストについては、[Programのコストの測定](../docs/PROGRAM_COST_MEASUREMENT.md)を参照してください。
そちらのマトリクスにはジャーナルとチェックポイントの処理が含まれているので、下のチェックポイントなしのCoreの比較と並べて示してはいけません。

比較したフレームワークは、次のとおりです。

| フレームワーク | バージョン | 抽象化 |
|-----------|---------|-------------|
| NeoGraph | 3.0 | ステートチャネル型のグラフ、C++20コルーチン＋asio |
| LangGraph | 1.1.9 | ステートチャネル型のグラフ（Python） |
| Haystack | 2.27 | 型付きソケットを持つコンポーネントのパイプライン |
| pydantic-graph | 1.84 | 次のノードが1つに決まるステートマシン |
| LlamaIndex Workflow | 0.14 | イベント駆動の非同期ワークフロー |
| AutoGen GraphFlow | 0.7.5 | メッセージパッシング型のマルチエージェントグラフ |

## ワークロード

6つの実装は、同じ2つのワークロードを移植したものです。可能な場合は1回だけコンパイルし、ホットループで繰り返し呼び出します。ステートとトポロジーを調整した箇所は、下に挙げています。

| ID | 形状 | ステート |
|----|-------|-------|
| `seq` | 3ノードのチェーン `a → b → c` | `counter` チャネルが1つ。各ノードが `counter+1` を書き込みます（上書きのリデューサー） |
| `par` | ワーカー5つにfan-outし、`summarizer` でjoinする | `results: list`（追加のリデューサー）と `count: int`。各ワーカーは自分のインデックスを追加し、summarizerは `len(results)` を書き込みます |

チェックポイントは、すべてのフレームワークで無効にしています。

3つの移植では、フレームワークの仕様に合わせてワークロードを調整する必要がありました。

* **Haystack** には、追加のリデューサーがありません。そのため、各ワーカーが自分専用の型付きソケットに出力し、summarizerがリストの長さを合計します。1回の実行でディスパッチされるコンポーネントの数は、同じです。
* **pydantic-graph** は、次のノードが1つに決まるステートマシンで、fan-outできません。そのため、`par` は6ノードの直列チェーン（`w1 → w2 → w3 → w4 → w5 → summ`）で代用しました。結果の表には、この点を注記しています。ネイティブの並列fan-outと同じ条件での比較ではありません。
* **AutoGen** は、ステートチャネルではなくメッセージパッシングです。カウンターはテキストメッセージの内容として符号化し、summarizerは届いたワーカーのメッセージを数えます。グラフの形状は同じですが、ステートのモデルが違います。

## 結果

下の**参照実行**は、2026-04-22に、x86_64 Linux、g++ 13 Release（`-O3 -DNDEBUG`）、CPython 3.12.3で、NeoGraph v3.0.0について測定したものです。NeoGraphは `bench_neograph` の10回の中央値、Pythonのフレームワークは3回の中央値です。バージョンは、neograph v3.0.0、langgraph 1.1.9、haystack-ai 2.28.0、pydantic-graph 1.85.1、llama-index-core 0.14.21、autogen-agentchat 0.7.5です。

参照実行の下に、当時のmaster（2026-04-29）で測り直した結果も載せています。`par` の行には、サポートしている2つの実行方式を分けて示しました。現在のデフォルトである `worker_count=1` と、`set_worker_count_auto()` で有効にするエンジン所有のプールです。I/Oのないマイクロベンチマークでこの2つの数値を別々に扱うべき理由は、表のあとの「注記」を参照してください。

![Engine-overhead benchmark: per-iteration latency and peak RSS](../docs/images/bench-engine-overhead.png)

### 1反復あたりのオーバーヘッド（µs、小さいほど良い）

| フレームワーク | `seq`（3ノードのチェーン） | `par`（fan-out 5＋join） | `seq`のNeoGraphとの比 | `par`のNeoGraphとの比 |
|-----------|---------------------:|-------------------------:|-------------------:|-------------------:|
| **NeoGraph v3.0.0** *（参照、2026-04-22）* | **5.0** | **11.8** | 1× | 1× |
| **NeoGraph master** *（2026-04-29、デフォルトの `worker_count=1`）* | **5.25** | **14.4** | 1× | 1× |
| **NeoGraph master** *（2026-04-29、`set_worker_count_auto()`）* | **5.25** | **278** | 1× | 1× |
| Haystack 2.28.0 | 139.85 | 278.48 | 28.0× / 26.6× / 26.6× | 23.6× / 19.3× / 1.0× |
| pydantic-graph 1.87.0 | 227.14 | 280.26¹ | 45.4× / 43.3× / 43.3× | 23.7×¹ / 19.5×¹ / 1.0×¹ |
| LangGraph 1.1.10 | 642.62 | 2,261.55 | 128.5× / 122.4× / 122.4× | 191.7× / 157.1× / 8.1× |
| LlamaIndex Workflow 0.14.21 | 1,564.54 | 4,373.76 | 312.9× / 298.0× / 298.0× | 370.7× / 303.7× / 15.7× |
| AutoGen GraphFlow 0.7.5 | 3,126.86 | 7,281.08 | 625.4× / 595.6× / 595.6× | 617.0× / 505.6× / 26.2× |

右の2列には、比を3つずつ示しています。v3.0.0の参照値との比、masterのデフォルト（worker=1）との比、masterのauto-workerモードとの比の順です。

日付付きの `seq` の測定値は、近い値になりました。NeoGraphは5.0 µsから5.25 µsに変わっています。`par` の測定値は実行モードによって異なります（11.8、14.4、278 µs）。参照の全行を±10%以内で再現できたわけではなく、現在のHEADを測定したものでもありません。

¹ pydantic-graphの `par` は、6ノードを直列に並べた代用であり、fan-outはサポートしていません。並列のワークロードではありませんが、一覧を揃えるために載せています。

### `par` の行についての注記（`seq` は変わりません）

`par` が14.4 µsから278 µsに増えた分は、実際の処理をしない5つのノードのために、エンジン所有のスレッドプールを使うことにしたコストを測ったものです。

* **現在の `build()` / `compile()` のデフォルトは `worker_count=1` です。** エンジン所有のプールは作られず、fan-outは呼び出し元のexecutor上でディスパッチされます。これが14.4 µsの行です。CPUをほとんど使わないノードや、スレッドセーフでない状態をノードが持つグラフでは、この行が回帰を見つける正しい指標になります。
* **並列のfan-outは、明示的に指定したときだけ使います。** `set_worker_count_auto()` は、プールのサイズを `hardware_concurrency` に合わせます。`set_worker_count(N)` は、上限を固定値で指定します。これが278 µsの行です。ここでは各ワーカーが整数を1つ追加するだけなので、調整のコストがそのまま見えます。100 msのLLM呼び出しと比べれば無視できる大きさで、独立したノードを重ねて実行できる利点があります。

ベンチマークのソースを変えずに、2つの方式を実行できます。

```bash
./build/bench_neograph 10000 5000 1
./build/bench_neograph 10000 5000 auto
```

3番目の引数は `par` のエンジンに適用され、`1`（デフォルト）、`auto`、または正のワーカー数を指定できます。出力には `config\tpar_workers\t...` の行が含まれるので、保存した結果から実行モードが分かります。

記録したworker=1の `par` では、LangGraphの2,261.55 µsはNeoGraphの14.4 µsの157.1倍です。エンジン所有のプールを使うと、8.1倍になります。Haystackとpydantic-graphは、NeoGraphのauto-workerの行（278 µs）とほぼ同じです。これらの比が表すのは、このワークロードとこの設定での値だけです。

### プロセス全体のエンドツーエンドの指標

ウォームアップと2つのワークロードを含めた、バイナリまたはスクリプト全体の実行時間です。`seq` は10,000反復、`par` は5,000反復で、`/usr/bin/time -f "%e s, %M KB"` で測定しました。

| フレームワーク | 経過時間の合計 | ピークRSS | Executor |
|-----------|--------------:|---------:|---------:|
| **NeoGraph 3.0** | **0.11 s** | **4.5 MB** | デフォルトはシングルスレッドのio_context |
| pydantic-graph | 3.98 s | 35.1 MB | シングルスレッドのasyncio（GIL） |
| Haystack | 3.85 s | 80.3 MB | シングルスレッドのasyncio（GIL） |
| LangGraph | 18.95 s | 60.1 MB | シングルスレッドのasyncio（GIL） |
| LlamaIndex Workflow | 39.49 s | 101.4 MB | シングルスレッドのasyncio（GIL） |
| AutoGen GraphFlow | 63.29 s | 52.3 MB | シングルスレッドのasyncio（GIL） |

NeoGraph 3.0のデフォルトのスーパーステップループは、`run_sync` を通じて、シングルスレッドのio_context上でコルーチンを実行します。CPUを使った並列のfan-outは、`engine->set_worker_count(N)` で有効にします。I/O待ちが中心のノードのワークロードでは、スレッドが1つでも、co_awaitで中断している間に他の処理が進みます。

## Linux ARM64のベースライン：Neoverse-N1

これは、[#165](https://github.com/fox1245/NeoGraph/issues/165)に基づく、ネイティブARM64プラットフォーム単独のベースラインです。上のx86_64の表との回帰比較ではありません。リビジョン `d7a6477` と専用の依存セットに固定しているので、後の測定値と照らし合わせられます。

| 項目 | 値 |
|---|---|
| 日付 | 2026-07-22 |
| OS | Ubuntu 24.04、Linux `6.17.0-1018-oracle` |
| アーキテクチャ | `aarch64` |
| CPU | 4 vCPU、ARM Neoverse-N1 |
| メモリ | 23 GiB、スワップなし |
| コンパイラー / CMake | GCC 13.3.0 / CMake 3.28.3 |
| Python | CPython 3.12.3 |
| NeoGraphのリビジョン | `d7a6477` |

ワークロードと反復回数は、主要なベンチマークと同じです。各グラフを1回だけコンパイルしたあと、`seq` を10,000反復、`par` を5,000反復しました。NeoGraphは10回、Pythonの各実装は3回測定し、表には中央値を載せています。チェックポイント、ネットワークI/O、モデル呼び出し、スリープは無効です。プロセス全体のピークRSSは、`/usr/bin/time -f "%e s, %M KB"` で取得しました。

| フレームワーク | `seq`（µs/反復） | `par`（µs/反復） | `seq`のNeoGraphとの比 | `par`のNeoGraphとの比 | ピークRSS |
|---|---:|---:|---:|---:|---:|
| **NeoGraph** | **9.50** | **21.80** | 1× | 1× | **4.35 MB** |
| Haystack 3.0.0 | 153.44 | 329.67 | 16.2× | 15.1× | 73.6 MB |
| pydantic-graph 1.87.0 | 342.60 | 405.87¹ | 36.1× | 18.6×¹ | 32.1 MB |
| LangGraph 1.2.9 | 1,037.55 | 3,289.22 | 109.2× | 150.9× | 63.7 MB |
| LlamaIndex 0.14.23 | 2,765.04 | 7,824.85 | 291.1× | 358.9× | 96.9 MB |
| AutoGen 0.7.5 | 4,166.39 | 9,571.11 | 438.6× | 439.0× | 47.8 MB |

NeoGraphはデフォルトのworker=1で測定しました。そのため、`par` の行が測っているのは、トポロジー、リデューサー、直列のディスパッチのオーバーヘッドであり、エンジン所有のスレッドプールによる実行ではありません。並列のfan-outを測りたい場合は、明示的な `auto` のベンチマークモードで別に測定してください。2つのモードの値を、1つの見出しの数字にまとめてはいけません。

¹ pydantic-graphでは、このfan-outのトポロジーを表現できません。この `par` の行は、上で説明したものと同じ、6ノードを直列に並べた代用です。バージョンを1.87.0に固定したのは、当時の最新版2.15.0では、ベンチマークで使っている `Graph(...)` コンストラクターのAPIがサポートされなくなったためです。プロセスは、CPUへの固定もcgroupによる制限もしていません。

## 数値の読み方

1. x86_64の参照実行で、`seq` のオーバーヘッドがNeoGraphの何倍かを見ると、Haystackが28.0倍、AutoGenが625.4倍です。これはフレームワークとワークロードの組み合わせで測った値で、それぞれのコストの原因を切り分けたものではありません。
2. 参照実行のプロセス全体のピークRSSは、NeoGraphが4.5 MB、Pythonの実装が35.1〜101.4 MBです。各プロセスのランタイムとインポートが含まれた値で、デプロイしたアプリケーションのメモリ使用量ではありません。
3. worker=1でのディスパッチと、エンジン所有のスレッドプールは、別の実行方式です。想定するワークロードで両方を測ってください。何も処理しないfan-outのベンチマークからは、調整のコストが分かりますが、モデルやI/Oのレイテンシーは予測できません。

## 注意事項 — このベンチマークで測っていないもの

* **実際のエージェントのワークロード。** フレームワークの比較には、モデルの推論、ネットワークリクエスト、ツールのI/Oが含まれていません。アプリケーション全体では、それらが大半を占めることもあります。この表から、エンドツーエンドのエージェントが速くなるとは言えません。
* **各フレームワークに向いたワークロード。** AutoGen、LlamaIndex、pydantic-graphは、それぞれ別の用途（マルチエージェントのチャット、イベント駆動の長時間のワークフロー、ステートマシンによる制御フロー）に向けて最適化されています。このベンチマークは、それらを使いません。つまり、NeoGraphが得意な条件で測っています。
* **チェックポイントのスループット。** 各フレームワークで永続化を有効にすると、シリアライズのコストが支配的になります。それは別のベンチマークです。
* **コールドスタート。** どの実装も、測定の前に10反復のウォームアップを行っています。プロセス全体の数値には、Pythonインタープリターの起動（約200 ms）と、フレームワークのインポート時間が含まれます。後者は大きくばらつきます（LlamaIndexとAutoGenは、多数のモジュールを読み込みます）。
* **公平性。** NeoGraphは、CMakeの `-DCMAKE_BUILD_TYPE=Release` でビルドしました。GCCでは `-O3 -DNDEBUG` になります。Pythonのフレームワークはいずれも標準のCPython 3.12で、依存バージョンはコホートごとに記録しており、独自のチューニングはしていません。スタンドアロンのベンチマークコマンドは `-O2` を使っていました。CMakeのビルドでは、`Release` は `-O3` になります。

## 再現手順

現在のソースビルドには、Core単体のベンチマークも含めて、外部の `SchemaProvider::runtime` パッケージが必要です。CMake 3.20以降は、明示した `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、インストール済みのランタイム、リビジョンを固定した公開GitHubのソースアーカイブの順に選びます。`NEOGRAPH_FETCH_SCHEMAPROVIDER` のデフォルトはONです。インストール済みのパッケージか明示したソースを使ってオフラインでビルドするときは、OFFにしてください。
再現用のコマンドがビルドするのは、現在メンテナンスしているターゲットです。過去のバイナリを再生成することも、表に載せたPythonのバージョンを自動で固定することもありません。
明示的に指定するチェックアウトやインストール済みのパッケージは、インターフェースと共有世代が6のものでなければなりません。新しい出力は別に保存してください。過去の表とJSONファイルは、SDK 0.3.3で測定した結果ではありません。

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

Pythonフレームワークの比較は引き続きオプションで、サードパーティのパッケージが必要です。

```bash
# Shared Python venv for every Python framework:
python3 -m venv /tmp/bench_venv
/tmp/bench_venv/bin/pip install \
    langgraph \
    haystack-ai \
    pydantic-graph \
    llama-index-core \
    "autogen-agentchat" "autogen-core" "autogen-ext"

# Run each bench (10k seq + 5k par matches the C++ side):
/tmp/bench_venv/bin/python benchmarks/bench_langgraph.py      10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_haystack.py       10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_pydantic_graph.py 10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_llamaindex.py     10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_autogen.py        10000 5000

# Peak RSS + wall time:
/usr/bin/time -f "%e s, %M KB" ./build-program-bench/bench_neograph
```

サービスを使うチェックポイント、HTTP、並行実行のDockerベンチマークは、別の実験です。上で説明した、決定的なネイティブのCore／Programの実行には、これらは不要です。

出力形式は、タブ区切りの `config`、`runtime`、`header`、`result`、`metric` のレコードです。ネイティブの結果の行には、合計時間の中央値と1反復あたりの時間が入ります。Pythonのスクリプトは、従来どおり `workload<TAB>iters<TAB>total_ms<TAB>per_iter_us` の行を出力します。

## 2026-04-19の数値を測定した環境

```
OS:        Linux 6.6.87.2-microsoft-standard-WSL2 (Ubuntu 24.04 userland)
CPU:       host CPU (8 logical cores exposed to WSL)
Compiler:  g++ 13.x, -std=c++20 -O2 -DNDEBUG
Python:    3.12.3 (system)
Versions:  langgraph 1.1.7, haystack-ai 2.27.0, pydantic-graph 1.84.1,
           llama-index-core 0.14.20, autogen-agentchat 0.7.5
```

ハードウェア、ランタイムのバージョン、ワークロードの形状、ワーカーのモードによって、レイテンシーも比率も変わります。この文書は、プラットフォーム間の許容誤差を定めていません。

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
